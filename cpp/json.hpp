#pragma once
// 极简 JSON 库，C++17，无第三方依赖
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jsonlite {

class Value {
public:
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };

    Type type = NUL;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;
    Value(std::nullptr_t) : type(NUL) {}
    Value(bool v) : type(BOOL), b(v) {}
    Value(int v) : type(NUM), num((double)v) {}
    Value(long long v) : type(NUM), num((double)v) {}
    Value(double v) : type(NUM), num(v) {}
    Value(const char* s) : type(STR), str(s ? s : "") {}
    Value(const std::string& s) : type(STR), str(s) {}
    Value(std::string&& s) : type(STR), str(std::move(s)) {}

    static Value array()  { Value v; v.type = ARR; return v; }
    static Value object() { Value v; v.type = OBJ; return v; }

    bool is_null() const { return type == NUL; }
    bool is_bool() const { return type == BOOL; }
    bool is_num()  const { return type == NUM; }
    bool is_str()  const { return type == STR; }
    bool is_arr()  const { return type == ARR; }
    bool is_obj()  const { return type == OBJ; }

    double      as_num (double d = 0)        const { return type == NUM ? num : d; }
    long long   as_int (long long d = 0)     const { return type == NUM ? (long long)num : d; }
    bool        as_bool(bool d = false)      const { return type == BOOL ? b : d; }
    std::string as_str (const std::string& d = "") const { return type == STR ? str : d; }

    void set(const std::string& k, Value v) {
        if (type != OBJ) { type = OBJ; obj.clear(); }
        for (auto& kv : obj) if (kv.first == k) { kv.second = std::move(v); return; }
        obj.emplace_back(k, std::move(v));
    }
    void push(Value v) {
        if (type != ARR) { type = ARR; arr.clear(); }
        arr.push_back(std::move(v));
    }

    const Value& get(const std::string& k) const {
        static const Value nil;
        if (type != OBJ) return nil;
        for (auto& kv : obj) if (kv.first == k) return kv.second;
        return nil;
    }
    const Value& at(size_t i) const {
        static const Value nil;
        if (type != ARR || i >= arr.size()) return nil;
        return arr[i];
    }
    bool has(const std::string& k) const {
        if (type != OBJ) return false;
        for (auto& kv : obj) if (kv.first == k) return true;
        return false;
    }
    size_t size() const {
        if (type == ARR) return arr.size();
        if (type == OBJ) return obj.size();
        return 0;
    }

    const Value& operator[](const std::string& k) const { return get(k); }
    const Value& operator[](size_t i) const { return at(i); }

    std::string dump(int indent = -1) const {
        std::string out;
        dump_impl(out, indent, 0);
        return out;
    }

    static Value parse(const std::string& s) {
        const char* p = s.c_str();
        const char* e = p + s.size();
        skip_ws(p, e);
        Value v = parse_value(p, e);
        skip_ws(p, e);
        // [Fix #3] 尾随垃圾报错，防止 "123abc" 静默变成 123
        if (p != e) throw std::runtime_error("json: trailing garbage");
        return v;
    }
    static Value parse_or(const std::string& s, Value fb) {
        try { return parse(s); } catch (...) { return fb; }
    }

private:
    static void skip_ws(const char*& p, const char* e) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    static void dump_str(const std::string& s, std::string& out) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (int)c);
                        out += buf;
                    } else out += (char)c;
            }
        }
        out += '"';
    }

    void dump_impl(std::string& out, int indent, int depth) const {
        auto nl = [&](int d) {
            if (indent < 0) return;
            out += '\n';
            out.append((size_t)(indent * d), ' ');
        };
        switch (type) {
            case NUL:  out += "null"; break;
            case BOOL: out += b ? "true" : "false"; break;
            case NUM: {
                // [Fix #4] NaN / Inf 不是合法 JSON，输出 null
                if (!std::isfinite(num)) { out += "null"; break; }
                if (num == std::floor(num) && std::fabs(num) < 1e15) {
                    out += std::to_string((long long)num);
                } else {
                    char buf[40];
                    std::snprintf(buf, sizeof(buf), "%.17g", num);
                    out += buf;
                }
                break;
            }
            case STR: dump_str(str, out); break;
            case ARR: {
                out += '[';
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (i) out += ',';
                    nl(depth + 1);
                    arr[i].dump_impl(out, indent, depth + 1);
                }
                if (!arr.empty()) nl(depth);
                out += ']';
                break;
            }
            case OBJ: {
                out += '{';
                for (size_t i = 0; i < obj.size(); ++i) {
                    if (i) out += ',';
                    nl(depth + 1);
                    dump_str(obj[i].first, out);
                    out += ':';
                    if (indent >= 0) out += ' ';
                    obj[i].second.dump_impl(out, indent, depth + 1);
                }
                if (!obj.empty()) nl(depth);
                out += '}';
                break;
            }
        }
    }

    static std::string parse_raw_string(const char*& p, const char* e) {
        ++p; // skip "
        std::string out;
        while (p < e) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p >= e) throw std::runtime_error("json: bad escape");
                char d = *p++;
                switch (d) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            if (p >= e) throw std::runtime_error("json: bad \\u");
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else throw std::runtime_error("json: bad hex");
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF && (e - p) >= 6 && p[0] == '\\' && p[1] == 'u') {
                            const char* q = p + 2;
                            unsigned lo = 0; bool ok = true;
                            for (int i = 0; i < 4; ++i) {
                                char h = q[i];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                                else { ok = false; break; }
                            }
                            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                p = q + 4;
                            }
                        }
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xF0 | (cp >> 18));
                            out += (char)(0x80 | ((cp >> 12) & 0x3F));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += d; break;
                }
            } else out += (char)c;
        }
        throw std::runtime_error("json: unterminated string");
    }

    static Value parse_value(const char*& p, const char* e) {
        skip_ws(p, e);
        if (p >= e) throw std::runtime_error("json: unexpected end");
        char c = *p;
        if (c == 'n' && e - p >= 4 && !std::strncmp(p, "null", 4))  { p += 4; return Value(); }
        if (c == 't' && e - p >= 4 && !std::strncmp(p, "true", 4))  { p += 4; return Value(true); }
        if (c == 'f' && e - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; return Value(false); }
        if (c == '"') return Value(parse_raw_string(p, e));

        if (c == '[') {
            ++p;
            Value v = Value::array();
            skip_ws(p, e);
            if (p < e && *p == ']') { ++p; return v; }
            while (true) {
                v.arr.push_back(parse_value(p, e));
                skip_ws(p, e);
                if (p >= e) throw std::runtime_error("json: unterminated array");
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; break; }
                throw std::runtime_error("json: expected , or ]");
            }
            return v;
        }
        if (c == '{') {
            ++p;
            Value v = Value::object();
            skip_ws(p, e);
            if (p < e && *p == '}') { ++p; return v; }
            while (true) {
                skip_ws(p, e);
                if (p >= e || *p != '"') throw std::runtime_error("json: expected key");
                std::string key = parse_raw_string(p, e);
                skip_ws(p, e);
                if (p >= e || *p != ':') throw std::runtime_error("json: expected :");
                ++p;
                v.obj.emplace_back(std::move(key), parse_value(p, e));
                skip_ws(p, e);
                if (p >= e) throw std::runtime_error("json: unterminated object");
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; break; }
                throw std::runtime_error("json: expected , or }");
            }
            return v;
        }
        char* endp = nullptr;
        double d = std::strtod(p, &endp);
        if (endp == p) throw std::runtime_error("json: bad number");
        p = endp;
        return Value(d);
    }
};

// 点路径取值："choices.0.delta.content"
inline const Value* path(const Value& v, const std::string& p) {
    const Value* cur = &v;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('.', i);
        if (j == std::string::npos) j = p.size();
        std::string key = p.substr(i, j - i);
        if (!key.empty()) {
            if (cur->is_obj()) {
                cur = &cur->get(key);
            } else if (cur->is_arr()) {
                char* endp = nullptr;
                long idx = std::strtol(key.c_str(), &endp, 10);
                if (!endp || *endp != '\0') return nullptr;
                if (idx < 0 || (size_t)idx >= cur->arr.size()) return nullptr;
                cur = &cur->arr[(size_t)idx];
            } else return nullptr;
            if (cur->is_null()) return nullptr;
        }
        i = j + 1;
    }
    return cur;
}

inline std::string jpath_str(const Value& v, const std::string& p) {
    const Value* x = path(v, p);
    return x ? x->as_str() : "";
}

} // namespace jsonlite