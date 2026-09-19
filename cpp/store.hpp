#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <set>
#include <algorithm>
#include <dirent.h>
#include "json.hpp"
#include "util.hpp"

using jsonlite::Value;

// ---------- 模型配置 ----------
struct ModelConfig {
    std::string id;
    std::string name;
    std::string provider = "openai";   // openai | anthropic | ollama | custom
    std::string base_url;
    std::string api_key;
    std::string model;
    std::string system_prompt;
    int max_time = 900;                // [Fix 小-1] 单次 HTTP 请求超时秒数
    std::vector<std::pair<std::string,std::string>> headers;
    Value extra = Value::object();

    // custom provider
    std::string url;
    std::string method = "POST";
    std::string body_template;
    std::string stream_mode = "sse";
    std::string stream_path;
    std::string response_path;

    Value to_json() const {
        Value v = Value::object();
        v.set("id", id);
        v.set("name", name);
        v.set("provider", provider);
        v.set("base_url", base_url);
        v.set("api_key", api_key);
        v.set("model", model);
        v.set("system_prompt", system_prompt);
        v.set("max_time", (long long)max_time);
        Value h = Value::object();
        for (auto& kv : headers) h.set(kv.first, kv.second);
        v.set("headers", h);
        v.set("params", extra);
        v.set("url", url);
        v.set("method", method);
        v.set("body_template", body_template);
        v.set("stream_mode", stream_mode);
        v.set("stream_path", stream_path);
        v.set("response_path", response_path);
        return v;
    }

    static ModelConfig from_json(const Value& v) {
        ModelConfig m;
        m.id            = v["id"].as_str();
        m.name          = v["name"].as_str();
        m.provider      = v["provider"].as_str("openai");
        m.base_url      = v["base_url"].as_str();
        m.api_key       = v["api_key"].as_str();
        m.model         = v["model"].as_str();
        m.system_prompt = v["system_prompt"].as_str();
        m.max_time      = (int)v["max_time"].as_int(900);
        if (m.max_time <= 0) m.max_time = 900;
        if (m.id.empty()) m.id = util::gen_id("model_");
        if (m.name.empty()) m.name = m.id;
        const Value& hs = v["headers"];
        if (hs.is_obj()) for (auto& kv : hs.obj) m.headers.push_back({kv.first, kv.second.as_str()});
        m.extra = v["params"].is_obj() ? v["params"] : Value::object();
        m.url             = v["url"].as_str();
        m.method          = v["method"].as_str("POST");
        m.body_template   = v["body_template"].as_str();
        m.stream_mode     = v["stream_mode"].as_str("sse");
        m.stream_path     = v["stream_path"].as_str();
        m.response_path   = v["response_path"].as_str();
        return m;
    }
};

// ---------- 消息 ----------
struct Message {
    std::string id;
    std::string role;       // user | assistant
    std::string content;
    std::string parent;
    long long   created = 0;

    Value to_json() const {
        Value v = Value::object();
        v.set("id", id);
        v.set("role", role);
        v.set("content", content);
        v.set("parent", parent);
        v.set("created", (long long)created);
        return v;
    }
    static Message from_json(const Value& v) {
        Message m;
        m.id      = v["id"].as_str();
        m.role    = v["role"].as_str();
        m.content = v["content"].as_str();
        m.parent  = v["parent"].as_str();
        m.created = v["created"].as_int(0);
        return m;
    }
};

// ---------- 会话 ----------
struct Conversation {
    std::string id;
    std::string title;
    std::string model_id;
    long long created = 0;
    long long updated = 0;
    std::vector<Message> messages;
    std::string active_leaf;

    const Message* find(const std::string& mid) const {
        for (auto& m : messages) if (m.id == mid) return &m;
        return nullptr;
    }
    Message* find(const std::string& mid) {
        for (auto& m : messages) if (m.id == mid) return &m;
        return nullptr;
    }

    std::vector<Message> path_to(const std::string& leaf) const {
        std::vector<Message> out;
        std::string cur = leaf;
        int guard = 0;
        while (!cur.empty() && guard++ < 100000) {
            const Message* m = find(cur);
            if (!m) break;
            out.push_back(*m);
            cur = m->parent;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> children_of(const std::string& mid) const {
        std::vector<std::string> out;
        for (auto& m : messages) if (m.parent == mid) out.push_back(m.id);
        return out;
    }

    void remove_descendants(const std::string& id) {
        std::set<std::string> desc;
        std::vector<std::string> stack{id};
        while (!stack.empty()) {
            std::string cur = stack.back(); stack.pop_back();
            for (auto& m : messages) {
                if (m.parent == cur && !desc.count(m.id)) {
                    desc.insert(m.id);
                    stack.push_back(m.id);
                }
            }
        }
        std::vector<Message> keep;
        keep.reserve(messages.size());
        for (auto& m : messages) if (!desc.count(m.id)) keep.push_back(m);
        messages.swap(keep);
    }

    void remove_subtree_including(const std::string& id) {
        remove_descendants(id);
        std::vector<Message> keep;
        keep.reserve(messages.size());
        for (auto& m : messages) if (m.id != id) keep.push_back(m);
        messages.swap(keep);
    }

    Value to_json() const {
        Value v = Value::object();
        v.set("id", id);
        v.set("title", title);
        v.set("model_id", model_id);
        v.set("created", (long long)created);
        v.set("updated", (long long)updated);
        v.set("active_leaf", active_leaf);
        Value arr = Value::array();
        for (auto& m : messages) arr.push(m.to_json());
        v.set("messages", arr);
        return v;
    }

    Value summary_json() const {
        Value v = Value::object();
        v.set("id", id);
        v.set("title", title);
        v.set("model_id", model_id);
        v.set("created", (long long)created);
        v.set("updated", (long long)updated);
        v.set("message_count", (long long)messages.size());
        return v;
    }

    static Conversation from_json(const Value& v) {
        Conversation c;
        c.id          = v["id"].as_str();
        c.title       = v["title"].as_str("Untitled");
        c.model_id    = v["model_id"].as_str();
        c.created     = v["created"].as_int(0);
        c.updated     = v["updated"].as_int(0);
        c.active_leaf = v["active_leaf"].as_str();
        const Value& arr = v["messages"];
        if (arr.is_arr()) for (auto& m : arr.arr) c.messages.push_back(Message::from_json(m));
        return c;
    }
};

// ---------- 存储 ----------
class Store {
public:
    std::string root;
    Value config = Value::object();
    std::vector<ModelConfig> models;
    std::map<std::string, Conversation> convs;
    std::mutex mtx;

    Store() {
        root = util::data_dir();
        util::mkdir_p(root);
        util::mkdir_p(root + "/conversations");
        load_config();
        load_all_convs();
    }

    void load_config() {
        std::string s = util::read_file(root + "/config.json");
        if (s.empty()) {
            config = Value::object();
            config.set("models", Value::array());
            config.set("settings", Value::object());
        } else {
            config = Value::parse_or(s, Value::object());
        }
        refresh_models();
    }

    void refresh_models() {
        models.clear();
        const Value& arr = config["models"];
        if (arr.is_arr()) for (auto& m : arr.arr) models.push_back(ModelConfig::from_json(m));
    }

    void save_config_locked() {
        Value arr = Value::array();
        for (auto& m : models) arr.push(m.to_json());
        config.set("models", arr);
        util::write_file(root + "/config.json", config.dump(2));
    }

    std::string conv_path(const std::string& id) {
        return root + "/conversations/" + id + ".json";
    }

    std::vector<std::string> list_conv_ids() {
        std::vector<std::string> out;
        DIR* d = ::opendir((root + "/conversations").c_str());
        if (!d) return out;
        struct dirent* e;
        while ((e = ::readdir(d))) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.substr(n.size() - 5) == ".json")
                out.push_back(n.substr(0, n.size() - 5));
        }
        ::closedir(d);
        return out;
    }

    void load_all_convs() {
        for (auto& id : list_conv_ids()) {
            std::string s = util::read_file(conv_path(id));
            if (s.empty()) continue;
            Value v = Value::parse_or(s, Value());
            if (v.is_obj() && v.has("id")) convs[id] = Conversation::from_json(v);
        }
    }

    Conversation* get(const std::string& id) {
        auto it = convs.find(id);
        return it == convs.end() ? nullptr : &it->second;
    }

    void save_conv_locked(const std::string& id) {
        auto it = convs.find(id);
        if (it == convs.end()) return;
        util::write_file(conv_path(id), it->second.to_json().dump(2));
    }

    void delete_conv(const std::string& id) {
        convs.erase(id);
        ::unlink(conv_path(id).c_str());
    }

    ModelConfig* find_model(const std::string& id) {
        for (auto& m : models) if (m.id == id) return &m;
        return nullptr;
    }

    void ensure_default_model() {
        if (!models.empty()) return;
        ModelConfig m;
        m.id = "deepseek";
        m.name = "DeepSeek Chat";
        m.provider = "openai";
        m.base_url = "https://api.deepseek.com/v1/chat/completions";
        m.model = "deepseek-chat";
        m.system_prompt = "You are a helpful assistant.";
        models.push_back(m);

        ModelConfig o;
        o.id = "ollama-local";
        o.name = "Ollama (local)";
        o.provider = "ollama";
        o.base_url = "http://127.0.0.1:11434/api/chat";
        o.model = "qwen2.5:7b";
        models.push_back(o);

        save_config_locked();
    }
};