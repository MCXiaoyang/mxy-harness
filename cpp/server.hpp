#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "json.hpp"
#include "store.hpp"
#include "engine.hpp"
#include "util.hpp"

using jsonlite::Value;

class Server {
public:
    Server(Store& store, const std::string& sock_path)
        : store_(store), path_(sock_path) {}

    void run() {
        ::unlink(path_.c_str());

        int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd < 0) { std::perror("socket"); std::exit(1); }

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::perror("bind"); std::exit(1);
        }
        if (::listen(sfd, 128) < 0) { std::perror("listen"); std::exit(1); }
        ::chmod(path_.c_str(), 0600);

        std::fprintf(stdout, "[mxy-harnessd] listening on %s\n", path_.c_str());
        std::fflush(stdout);

        while (true) {
            int cfd = ::accept(sfd, nullptr, nullptr);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                break;
            }
            std::thread([this, cfd]() { this->handle_client(cfd); }).detach();
        }
        ::close(sfd);
    }

private:
    // [Fix #1] Client 携带 cancel 标志
    struct Client {
        int fd = -1;
        std::mutex wmtx;
        std::atomic<bool> closed{false};
        std::atomic<bool> cancel{false};

        void send(const Value& v) {
            if (closed.load()) return;
            std::string s = v.dump();
            s += '\n';
            std::lock_guard<std::mutex> lk(wmtx);
            size_t off = 0;
            while (off < s.size()) {
                ssize_t n = ::write(fd, s.data() + off, s.size() - off);
                if (n <= 0) { closed.store(true); break; }
                off += (size_t)n;
            }
        }
    };
    using ClientPtr = std::shared_ptr<Client>;

    Store& store_;
    std::string path_;

    void ok(ClientPtr c, const std::string& id, const Value& data) {
        Value r = Value::object();
        r.set("id", id);
        r.set("type", "result");
        r.set("data", data);
        c->send(r);
    }
    void err(ClientPtr c, const std::string& id, const std::string& msg) {
        Value r = Value::object();
        r.set("id", id);
        r.set("type", "error");
        r.set("message", msg);
        c->send(r);
    }
    void delta(ClientPtr c, const std::string& id, const std::string& txt) {
        Value r = Value::object();
        r.set("id", id);
        r.set("type", "delta");
        r.set("text", txt);
        c->send(r);
    }

    void handle_client(int fd) {
        auto cli = std::make_shared<Client>();
        cli->fd = fd;

        std::string buf;
        char tmp[8192];
        while (true) {
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n <= 0) break;
            buf.append(tmp, (size_t)n);

            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (line.empty()) continue;

                Value req = Value::parse_or(line, Value());
                if (!req.is_obj()) continue;

                std::string id  = req["id"].as_str();
                if (id.empty()) id = std::to_string(util::now_ms());
                std::string cmd = req["cmd"].as_str();
                Value params    = req["params"];

                std::thread([this, cli, id, cmd, params]() {
                    try {
                        this->dispatch(cli, id, cmd, params);
                    } catch (const std::exception& e) {
                        this->err(cli, id, std::string("internal: ") + e.what());
                    } catch (...) {
                        this->err(cli, id, "internal error");
                    }
                }).detach();
            }
        }
        // [Fix #1] 客户端断开 → 通知所有在跑的任务取消
        cli->cancel.store(true);
        cli->closed.store(true);
        ::close(fd);
    }

    void dispatch(ClientPtr c, const std::string& id, const std::string& cmd, const Value& p) {
        if (cmd == "ping")              return do_ping(c, id);
        if (cmd == "list_models")       return do_list_models(c, id);
        if (cmd == "save_model")        return do_save_model(c, id, p);
        if (cmd == "delete_model")      return do_delete_model(c, id, p);
        if (cmd == "list_conversations")return do_list_convs(c, id);
        if (cmd == "new_conversation")  return do_new_conv(c, id, p);
        if (cmd == "get_conversation")  return do_get_conv(c, id, p);
        if (cmd == "delete_conversation")return do_del_conv(c, id, p);
        if (cmd == "rename_conversation")return do_rename_conv(c, id, p);
        if (cmd == "chat")              return do_chat(c, id, p);
        if (cmd == "regenerate")        return do_regenerate(c, id, p);
        if (cmd == "edit_message")      return do_edit_message(c, id, p);
        if (cmd == "delete_message")    return do_delete_message(c, id, p);
        if (cmd == "set_active_leaf")   return do_set_leaf(c, id, p);
        if (cmd == "get_config")        return do_get_config(c, id);
        if (cmd == "set_settings")      return do_set_settings(c, id, p);
        if (cmd == "cancel")            return do_cancel(c, id, p);
        err(c, id, "unknown command: " + cmd);
    }

    // ---------------- 基础命令 ----------------
    void do_ping(ClientPtr c, const std::string& id) {
        Value d = Value::object();
        d.set("pong", true);
        d.set("time", (long long)util::now_ms());
        ok(c, id, d);
    }

    // [Fix #1] 允许 Python 侧通过同一连接发 cancel 命令
    void do_cancel(ClientPtr c, const std::string& id, const Value&) {
        c->cancel.store(true);
        ok(c, id, Value(true));
    }

    void do_list_models(ClientPtr c, const std::string& id) {
        std::lock_guard<std::mutex> lk(store_.mtx);
        Value arr = Value::array();
        for (auto& m : store_.models) {
            Value v = m.to_json();
            std::string k = m.api_key;
            if (k.size() > 8) v.set("api_key", k.substr(0, 4) + "****" + k.substr(k.size() - 4));
            arr.push(v);
        }
        ok(c, id, arr);
    }

    void do_save_model(ClientPtr c, const std::string& id, const Value& p) {
        ModelConfig mc = ModelConfig::from_json(p);
        std::lock_guard<std::mutex> lk(store_.mtx);
        ModelConfig* exist = store_.find_model(mc.id);
        if (exist) {
            if (mc.api_key.find("****") != std::string::npos) mc.api_key = exist->api_key;
            *exist = mc;
        } else {
            store_.models.push_back(mc);
        }
        store_.save_config_locked();
        Value d = Value::object();
        d.set("id", mc.id);
        ok(c, id, d);
    }

    void do_delete_model(ClientPtr c, const std::string& id, const Value& p) {
        std::string mid = p["id"].as_str();
        std::lock_guard<std::mutex> lk(store_.mtx);
        std::vector<ModelConfig> keep;
        for (auto& m : store_.models) if (m.id != mid) keep.push_back(m);
        store_.models.swap(keep);
        store_.save_config_locked();
        ok(c, id, Value(true));
    }

    void do_get_config(ClientPtr c, const std::string& id) {
        std::lock_guard<std::mutex> lk(store_.mtx);
        Value d = Value::object();
        d.set("root", store_.root);
        d.set("settings", store_.config["settings"]);
        ok(c, id, d);
    }

    void do_set_settings(ClientPtr c, const std::string& id, const Value& p) {
        std::lock_guard<std::mutex> lk(store_.mtx);
        store_.config.set("settings", p);
        store_.save_config_locked();
        ok(c, id, Value(true));
    }

    // ---------------- 会话 ----------------
    void do_list_convs(ClientPtr c, const std::string& id) {
        std::lock_guard<std::mutex> lk(store_.mtx);
        std::vector<Conversation*> list;
        for (auto& kv : store_.convs) list.push_back(&kv.second);
        std::sort(list.begin(), list.end(), [](Conversation* a, Conversation* b) {
            return a->updated > b->updated;
        });
        Value arr = Value::array();
        for (auto* cv : list) arr.push(cv->summary_json());
        ok(c, id, arr);
    }

    void do_new_conv(ClientPtr c, const std::string& id, const Value& p) {
        Conversation cv;
        cv.id = util::gen_id("conv_");
        cv.title = p["title"].as_str("New chat");
        cv.model_id = p["model_id"].as_str();
        cv.created = cv.updated = util::now_ms();
        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            if (cv.model_id.empty() && !store_.models.empty())
                cv.model_id = store_.models.front().id;
            store_.convs[cv.id] = cv;
            store_.save_conv_locked(cv.id);
        }
        Value d = Value::object();
        d.set("id", cv.id);
        d.set("model_id", cv.model_id);
        ok(c, id, d);
    }

    void do_get_conv(ClientPtr c, const std::string& id, const Value& p) {
        std::string cid = p["conv_id"].as_str();
        std::lock_guard<std::mutex> lk(store_.mtx);
        Conversation* cv = store_.get(cid);
        if (!cv) return err(c, id, "conversation not found");

        auto path = cv->path_to(cv->active_leaf);
        Value arr = Value::array();
        for (auto& m : path) {
            Value v = m.to_json();
            auto kids = cv->children_of(m.id);
            Value karr = Value::array();
            for (auto& k : kids) karr.push(k);
            v.set("children", karr);
            arr.push(v);
        }
        Value d = Value::object();
        d.set("id", cv->id);
        d.set("title", cv->title);
        d.set("model_id", cv->model_id);
        d.set("active_leaf", cv->active_leaf);
        d.set("messages", arr);
        ok(c, id, d);
    }

    void do_del_conv(ClientPtr c, const std::string& id, const Value& p) {
        std::string cid = p["conv_id"].as_str();
        std::lock_guard<std::mutex> lk(store_.mtx);
        store_.delete_conv(cid);
        ok(c, id, Value(true));
    }

    void do_rename_conv(ClientPtr c, const std::string& id, const Value& p) {
        std::string cid = p["conv_id"].as_str();
        std::string title = p["title"].as_str();
        std::lock_guard<std::mutex> lk(store_.mtx);
        Conversation* cv = store_.get(cid);
        if (!cv) return err(c, id, "conversation not found");
        cv->title = title;
        cv->updated = util::now_ms();
        store_.save_conv_locked(cid);
        ok(c, id, Value(true));
    }

    void do_set_leaf(ClientPtr c, const std::string& id, const Value& p) {
        std::string cid = p["conv_id"].as_str();
        std::string mid = p["message_id"].as_str();
        std::lock_guard<std::mutex> lk(store_.mtx);
        Conversation* cv = store_.get(cid);
        if (!cv) return err(c, id, "conversation not found");
        if (!cv->find(mid)) return err(c, id, "message not found");
        cv->active_leaf = mid;
        store_.save_conv_locked(cid);
        ok(c, id, Value(true));
    }

    // ---------------- 生成核心 ----------------
    void generate_reply(ClientPtr c, const std::string& id,
                        const std::string& conv_id,
                        const std::string& override_model,
                        const std::string& override_sys)
    {
        std::vector<Message> history;
        std::string use_model;
        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            Conversation* cv = store_.get(conv_id);
            if (!cv) { err(c, id, "conversation not found"); return; }
            if (!override_model.empty()) cv->model_id = override_model;
            use_model = cv->model_id;
            history = cv->path_to(cv->active_leaf);
        }

        ModelConfig mc;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            if (ModelConfig* pm = store_.find_model(use_model)) { mc = *pm; found = true; }
        }
        if (!found) { err(c, id, "model not found: " + use_model); return; }

        std::string sys = override_sys.empty() ? mc.system_prompt : override_sys;

        // [Fix #1] 传 &c->cancel
        ChatOutcome oc = run_chat(mc, history, sys,
            [&](const std::string& d) { delta(c, id, d); },
            &c->cancel);

        // 完全失败且没拿到任何内容 → 报错
        if (!oc.ok && oc.content.empty()) { err(c, id, oc.error); return; }
        // 有部分内容就存下来（用户可能中途取消）

        std::string asst_id = util::gen_id("m_");
        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            Conversation* cv = store_.get(conv_id);
            if (cv && (!oc.content.empty() || oc.ok)) {
                Message am;
                am.id      = asst_id;
                am.role    = "assistant";
                am.content = oc.content;
                am.parent  = cv->active_leaf;
                am.created = util::now_ms();
                cv->messages.push_back(am);
                cv->active_leaf = am.id;
                cv->updated = am.created;
                store_.save_conv_locked(conv_id);
            }
        }

        Value data = Value::object();
        data.set("conv_id", conv_id);
        data.set("message_id", asst_id);
        data.set("content", oc.content);
        data.set("aborted", oc.aborted);
        // [Fix #12]
        data.set("finish_reason", oc.finish_reason);
        Value usage = Value::object();
        usage.set("prompt_tokens", (long long)oc.prompt_tokens);
        usage.set("completion_tokens", (long long)oc.completion_tokens);
        usage.set("total_tokens", (long long)oc.total_tokens);
        data.set("usage", usage);

        Value d = Value::object();
        d.set("type", "done");
        d.set("id", id);
        d.set("data", data);
        c->send(d);
    }

    void do_chat(ClientPtr c, const std::string& id, const Value& p) {
        std::string conv_id = p["conv_id"].as_str();
        std::string content = p["content"].as_str();
        std::string model_id = p["model_id"].as_str();
        std::string sys = p["system_prompt"].as_str();

        if (content.empty()) return err(c, id, "empty content");

        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            Conversation* cv = store_.get(conv_id);
            if (!cv) return err(c, id, "conversation not found");

            Message um;
            um.id      = util::gen_id("m_");
            um.role    = "user";
            um.content = content;
            um.parent  = cv->active_leaf;
            um.created = util::now_ms();
            cv->messages.push_back(um);
            cv->active_leaf = um.id;
            cv->updated = um.created;
            if (cv->title.empty() || cv->title == "New chat") {
                std::string t = content;
                if (t.size() > 40) t = t.substr(0, 40) + "...";
                cv->title = t;
            }
            store_.save_conv_locked(conv_id);
        }
        generate_reply(c, id, conv_id, model_id, sys);
    }

    void do_regenerate(ClientPtr c, const std::string& id, const Value& p) {
        std::string conv_id = p["conv_id"].as_str();
        std::string model_id = p["model_id"].as_str();
        std::string sys = p["system_prompt"].as_str();

        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            Conversation* cv = store_.get(conv_id);
            if (!cv) return err(c, id, "conversation not found");

            auto path = cv->path_to(cv->active_leaf);
            if (path.empty()) return err(c, id, "nothing to regenerate");
            if (path.back().role != "assistant")
                return err(c, id, "last message is not assistant");

            std::string last_id = path.back().id;
            std::string parent  = path.back().parent;
            cv->remove_subtree_including(last_id);
            cv->active_leaf = parent;
            store_.save_conv_locked(conv_id);
        }
        generate_reply(c, id, conv_id, model_id, sys);
    }

    void do_edit_message(ClientPtr c, const std::string& id, const Value& p) {
        std::string conv_id  = p["conv_id"].as_str();
        std::string mid      = p["message_id"].as_str();
        std::string content  = p["content"].as_str();
        bool regen           = p["regenerate"].as_bool(true);
        std::string model_id = p["model_id"].as_str();

        {
            std::lock_guard<std::mutex> lk(store_.mtx);
            Conversation* cv = store_.get(conv_id);
            if (!cv) return err(c, id, "conversation not found");
            Message* m = cv->find(mid);
            if (!m) return err(c, id, "message not found");

            m->content = content;
            cv->remove_descendants(mid);
            cv->active_leaf = mid;
            cv->updated = util::now_ms();
            store_.save_conv_locked(conv_id);
        }
        if (regen) generate_reply(c, id, conv_id, model_id, "");
        else ok(c, id, Value(true));
    }

    void do_delete_message(ClientPtr c, const std::string& id, const Value& p) {
        std::string conv_id = p["conv_id"].as_str();
        std::string mid     = p["message_id"].as_str();
        std::lock_guard<std::mutex> lk(store_.mtx);
        Conversation* cv = store_.get(conv_id);
        if (!cv) return err(c, id, "conversation not found");
        Message* m = cv->find(mid);
        if (!m) return err(c, id, "message not found");
        std::string parent = m->parent;
        cv->remove_subtree_including(mid);
        if (cv->active_leaf == mid || !cv->find(cv->active_leaf))
            cv->active_leaf = parent;
        store_.save_conv_locked(conv_id);
        ok(c, id, Value(true));
    }
};