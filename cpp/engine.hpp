#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <algorithm>
#include "json.hpp"
#include "store.hpp"
#include "http.hpp"

using jsonlite::Value;

struct ChatOutcome {
    bool ok = false;
    bool aborted = false;
    std::string error;
    std::string content;
    // [Fix #12] 元信息
    std::string finish_reason;
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long total_tokens = 0;
};

inline std::string apply_template(std::string tpl,
                                  const std::string& model,
                                  const std::string& messages_json,
                                  const std::string& prompt,
                                  const std::string& system_prompt)
{
    // [Fix 小-10] 先把 messages 塞进去，用一个哨兵占位，避免 messages_json
    // 内部若含 {{xxx}} 被后续 replace 二次替换。
    util::replace_all(tpl, "{{messages}}", "\x01MESSAGES\x01");
    util::replace_all(tpl, "{{model}}",    util::json_escape_inner(model));
    util::replace_all(tpl, "{{prompt}}",   util::json_escape_inner(prompt));
    util::replace_all(tpl, "{{system}}",   util::json_escape_inner(system_prompt));
    util::replace_all(tpl, "\x01MESSAGES\x01", messages_json);
    return tpl;
}

inline ChatOutcome run_chat(const ModelConfig& mc,
                            const std::vector<Message>& history,
                            const std::string& system_prompt,
                            std::function<void(const std::string&)> on_delta,
                            std::atomic<bool>* cancel)
{
    ChatOutcome out;
    std::string content;
    std::string provider = mc.provider;
    std::string url;
    std::vector<std::pair<std::string,std::string>> headers;
    std::string body;
    std::string mode = "sse";
    std::string spath = "choices.0.delta.content";
    std::string rpath = "choices.0.message.content";

    auto build_msgs = [&](bool include_system) {
        Value msgs = Value::array();
        if (include_system && !system_prompt.empty()) {
            Value m = Value::object();
            m.set("role", "system");
            m.set("content", system_prompt);
            msgs.push(m);
        }
        for (auto& h : history) {
            Value m = Value::object();
            m.set("role", h.role);
            m.set("content", h.content);
            msgs.push(m);
        }
        return msgs;
    };

    if (provider == "openai" || provider == "deepseek" ||
        provider == "compatible" || provider.empty())
    {
        url = mc.base_url;
        Value p = Value::object();
        p.set("model", mc.model);
        p.set("messages", build_msgs(true));
        p.set("stream", true);
        if (mc.extra.is_obj()) for (auto& kv : mc.extra.obj) p.set(kv.first, kv.second);
        body = p.dump();

        headers.push_back({"Content-Type", "application/json"});
        headers.push_back({"Accept", "text/event-stream"});
        if (!mc.api_key.empty()) headers.push_back({"Authorization", "Bearer " + mc.api_key});
        for (auto& h : mc.headers) headers.push_back(h);
        mode  = "sse";
        spath = "choices.0.delta.content";
        rpath = "choices.0.message.content";
    }
    else if (provider == "anthropic")
    {
        url = mc.base_url;
        Value p = Value::object();
        p.set("model", mc.model);
        p.set("max_tokens", mc.extra["max_tokens"].is_num() ? mc.extra["max_tokens"] : Value(4096));
        p.set("stream", true);
        if (!system_prompt.empty()) p.set("system", system_prompt);
        Value msgs = Value::array();
        for (auto& h : history) {
            Value m = Value::object();
            m.set("role", h.role == "assistant" ? "assistant" : "user");
            m.set("content", h.content);
            msgs.push(m);
        }
        p.set("messages", msgs);
        if (mc.extra.is_obj())
            for (auto& kv : mc.extra.obj)
                if (kv.first != "max_tokens") p.set(kv.first, kv.second);
        body = p.dump();

        headers.push_back({"Content-Type", "application/json"});
        headers.push_back({"x-api-key", mc.api_key});
        headers.push_back({"anthropic-version", "2023-06-01"});
        for (auto& h : mc.headers) headers.push_back(h);
        mode  = "anthropic-sse";
        spath = "delta.text";
        rpath = "content.0.text";
    }
    else if (provider == "ollama")
    {
        url = mc.base_url.empty() ? "http://127.0.0.1:11434/api/chat" : mc.base_url;
        Value p = Value::object();
        p.set("model", mc.model);
        p.set("messages", build_msgs(true));
        p.set("stream", true);
        if (mc.extra.is_obj()) for (auto& kv : mc.extra.obj) p.set(kv.first, kv.second);
        body = p.dump();

        headers.push_back({"Content-Type", "application/json"});
        for (auto& h : mc.headers) headers.push_back(h);
        mode  = "ndjson";
        spath = "message.content";
        rpath = "message.content";
    }
    else if (provider == "custom")
    {
        url = mc.url.empty() ? mc.base_url : mc.url;
        Value msgs = build_msgs(true);
        std::string last_user;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->role == "user") { last_user = it->content; break; }
        }
        std::string tpl = mc.body_template;
        if (tpl.empty())
            tpl = "{\"model\":\"{{model}}\",\"messages\":{{messages}},\"stream\":true}";
        body = apply_template(tpl, mc.model, msgs.dump(), last_user, system_prompt);

        headers.push_back({"Content-Type", "application/json"});
        headers.push_back({"Accept", "text/event-stream"});
        for (auto& h : mc.headers) headers.push_back(h);
        mode  = mc.stream_mode.empty() ? "sse" : mc.stream_mode;
        spath = mc.stream_path.empty()   ? "choices.0.delta.content"   : mc.stream_path;
        rpath = mc.response_path.empty() ? "choices.0.message.content" : mc.response_path;
    }
    else {
        out.error = "unknown provider: " + provider;
        return out;
    }

    std::string parse_err;

    // [Fix #12] 从 chunk 里抽 finish_reason / usage
    auto capture_meta = [&](const Value& j) {
        if (!j.is_obj()) return;
        if (const Value* x = jsonlite::path(j, "choices.0.finish_reason"); x && x->is_str())
            out.finish_reason = x->str;
        if (const Value* x = jsonlite::path(j, "delta.stop_reason"); x && x->is_str())
            out.finish_reason = x->str;
        if (const Value* x = jsonlite::path(j, "message.stop_reason"); x && x->is_str())
            out.finish_reason = x->str;
        if (const Value* u = jsonlite::path(j, "usage"); u && u->is_obj()) {
            if (u->has("prompt_tokens"))     out.prompt_tokens     = (*u)["prompt_tokens"].as_int();
            if (u->has("completion_tokens")) out.completion_tokens = (*u)["completion_tokens"].as_int();
            if (u->has("total_tokens"))      out.total_tokens      = (*u)["total_tokens"].as_int();
            if (u->has("input_tokens"))      out.prompt_tokens     = (*u)["input_tokens"].as_int();
            if (u->has("output_tokens"))     out.completion_tokens = (*u)["output_tokens"].as_int();
        }
    };

    auto handle_line = [&](const std::string& line) -> bool {
        if (cancel && cancel->load()) return false;
        if (line.empty()) return true;

        if (mode == "text") {
            content += line;
            if (on_delta) on_delta(line);
            return true;
        }
        if (mode == "none") return true;

        std::string data = line;
        if (mode == "sse" || mode == "anthropic-sse") {
            if (line.rfind("data:", 0) != 0) return true;
            data = line.substr(5);
            while (!data.empty() && (data[0] == ' ' || data[0] == '\t')) data.erase(0, 1);
            if (data.empty() || data == "[DONE]") return true;
        }

        Value j = Value::parse_or(data, Value());
        if (!j.is_obj()) return true;
        capture_meta(j);

        if (mode == "anthropic-sse") {
            std::string t = j["type"].as_str();
            if (t == "content_block_delta") {
                std::string txt = jsonlite::jpath_str(j, spath);
                if (!txt.empty()) { content += txt; if (on_delta) on_delta(txt); }
            } else if (t == "error") {
                parse_err = jsonlite::jpath_str(j, "error.message");
            }
            return true;
        }

        if (mode == "ndjson") {
            if (j["done"].as_bool(false)) return true;
            std::string txt = jsonlite::jpath_str(j, spath);
            if (!txt.empty()) { content += txt; if (on_delta) on_delta(txt); }
            return true;
        }

        // 通用 SSE
        if (j.has("error")) {
            if (j["error"].is_str()) parse_err = j["error"].as_str();
            else parse_err = jsonlite::jpath_str(j, "error.message");
        }
        std::string txt = jsonlite::jpath_str(j, spath);
        if (txt.empty() && spath != rpath) txt = jsonlite::jpath_str(j, rpath);
        if (!txt.empty()) { content += txt; if (on_delta) on_delta(txt); }
        return true;
    };

    httpc::Result r = httpc::request("POST", url, headers, body, true, handle_line,
                                     cancel, mc.max_time);
    out.aborted = r.aborted;

    if (!r.ok && !r.aborted) {
        out.error = "HTTP " + std::to_string(r.status);
        if (!parse_err.empty()) out.error += ": " + parse_err;
        else if (!r.stderr_text.empty()) out.error += " (" + r.stderr_text + ")";
        else if (!r.body.empty()) out.error += " " + r.body.substr(0, 400);
        return out;
    }

    // 回退：有些“小厂”API 即使 stream=true 也一次性返回完整 JSON
    if (content.empty() && !r.body.empty()) {
        Value j = Value::parse_or(r.body, Value());
        if (j.is_obj()) {
            capture_meta(j);
            std::string txt = jsonlite::jpath_str(j, rpath);
            if (txt.empty()) txt = jsonlite::jpath_str(j, spath);
            if (!txt.empty()) { content = txt; if (on_delta) on_delta(txt); }
            else {
                const Value* x = jsonlite::path(j, "choices.0.text");
                if (x && x->is_str()) { content = x->str; if (on_delta) on_delta(x->str); }
            }
        } else if (r.body.find_first_not_of(" \t\r\n") != std::string::npos) {
            content = r.body;
            if (on_delta) on_delta(r.body);
        }
    }

    out.ok = true;
    out.content = content;
    return out;
}