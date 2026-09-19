#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

namespace util {

inline long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    return "/tmp";
}

// [Rename] 数据目录改为 ~/.mxy-harness
inline std::string data_dir() {
    const char* e = std::getenv("MXY_HARNESS_HOME");
    if (e && *e) return e;
    return home_dir() + "/.mxy-harness";
}

inline void mkdir_p(const std::string& path) {
    if (path.empty()) return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/" || cur.empty()) continue;
            ::mkdir(cur.c_str(), 0755);
        }
    }
}

// [Fix #5] 清空目录里所有 .tmp 后缀的文件（daemon 启动时调一次）
inline void clean_tmp_files(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = ::readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) {
            ::unlink((dir + "/" + n).c_str());
        }
    }
    ::closedir(d);
}

inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

inline std::string gen_id(const std::string& prefix = "") {
    static std::mt19937_64 rng(
        (unsigned long long)std::random_device{}() ^ (unsigned long long)now_ms());
    static const char* hex = "0123456789abcdef";
    std::string s = prefix;
    for (int i = 0; i < 16; ++i) s += hex[rng() & 0xF];
    return s;
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::string json_escape_inner(const std::string& s) {
    std::string out;
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
    return out;
}

inline void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace util