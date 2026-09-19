#pragma once
// HTTPS 通过 fork/exec 系统 curl 实现（不引入任何 C++ 第三方库）
//
// [Fix #1] 读取管道改用 poll()，每 200ms 检查一次 cancel 标志，
//          客户端断开时能立刻 kill 掉 curl，不再阻塞 15 分钟。
// [Fix #5] 临时文件放到 httpc::tmp_dir()（默认 /tmp，daemon 启动时改成
//          ~/.mxy-harness/tmp/ 并清空）。
// [小-1]   --max-time 从参数传入（来源 ModelConfig.max_time）。
// [小-2]   curl stderr 落到临时文件，出错时附加到 Result::stderr_text。
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include "json.hpp"
#include "util.hpp"

namespace httpc {

// ---------------- 临时目录管理 ----------------
inline std::string& tmp_dir_ref() {
    static std::string d = "/tmp";
    return d;
}
inline void set_tmp_dir(const std::string& d) { tmp_dir_ref() = d; }

inline std::string tmp_path(const std::string& tag) {
    static std::atomic<unsigned long long> ctr{0};
    unsigned long long n = ctr.fetch_add(1);
    return tmp_dir_ref() + "/mxyh_" + tag + "_" + std::to_string((long)::getpid())
         + "_" + std::to_string(n) + ".tmp";
}

struct Result {
    long status = 0;
    std::string body;
    bool ok = false;
    bool aborted = false;
    std::string error;
    std::string stderr_text;   // [Fix 小-2]
};

inline Result request(const std::string& method,
                      const std::string& url,
                      const std::vector<std::pair<std::string,std::string>>& headers,
                      const std::string& body,
                      bool stream,
                      std::function<bool(const std::string&)> on_line,
                      std::atomic<bool>* cancel,
                      int max_time_sec = 900)
{
    Result res;
    std::string body_file = tmp_path("body");
    std::string hdr_file  = tmp_path("hdr");
    std::string err_file  = tmp_path("err");

    {
        std::ofstream f(body_file, std::ios::binary);
        if (!f) { res.error = "temp file error"; return res; }
        f.write(body.data(), (std::streamsize)body.size());
    }

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        ::unlink(body_file.c_str());
        res.error = "pipe() failed";
        return res;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        ::unlink(body_file.c_str());
        res.error = "fork() failed";
        return res;
    }

    if (pid == 0) {
        // ---- 子进程：执行 curl ----
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        if (pipefd[1] != STDOUT_FILENO) ::close(pipefd[1]);

        // [Fix 小-2] stderr 落盘而不是 /dev/null
        int errfd = ::open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (errfd >= 0) {
            ::dup2(errfd, STDERR_FILENO);
            ::close(errfd);
        } else {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
        }

        std::vector<std::string> a;
        a.push_back("curl");
        a.push_back("-sS");
        a.push_back("-N");
        a.push_back("--max-time");
        a.push_back(std::to_string(max_time_sec > 0 ? max_time_sec : 900));
        a.push_back("-X");
        a.push_back(method.empty() ? "POST" : method);
        a.push_back(url);
        for (auto& h : headers) {
            a.push_back("-H");
            a.push_back(h.first + ": " + h.second);
        }
        a.push_back("--data-binary");
        a.push_back("@" + body_file);
        a.push_back("--dump-header");
        a.push_back(hdr_file);

        std::vector<char*> argv;
        argv.reserve(a.size() + 1);
        for (auto& s : a) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        ::execvp("curl", argv.data());
        ::_exit(127);
    }

    // ---- 父进程 ----
    ::close(pipefd[1]);
    int fd = pipefd[0];
    char buf[16384];
    std::string pending;
    bool aborted = false;

    // [Fix #1] poll + cancel 检查
    while (true) {
        if (cancel && cancel->load()) { aborted = true; break; }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;  // 超时，回到顶部重新检查 cancel

        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        if (!stream) {
            res.body.append(buf, (size_t)n);
            continue;
        }

        pending.append(buf, (size_t)n);
        size_t pos;
        bool stop = false;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (on_line && !on_line(line)) { stop = true; break; }
        }
        if (stop) { aborted = true; break; }
    }
    if (stream && !aborted && !pending.empty() && on_line) {
        on_line(pending);
    }

    if (aborted) {
        ::kill(pid, SIGTERM);
        int st = 0;
        for (int i = 0; i < 50; ++i) {
            pid_t r = ::waitpid(pid, &st, WNOHANG);
            if (r == pid) { pid = -1; break; }
            ::usleep(10000);
        }
        if (pid > 0) { ::kill(pid, SIGKILL); ::waitpid(pid, &st, 0); }
    } else {
        int st = 0;
        ::waitpid(pid, &st, 0);
    }
    ::close(fd);
    res.aborted = aborted;

    // 解析状态行
    {
        std::ifstream hf(hdr_file);
        std::string first;
        if (std::getline(hf, first)) {
            size_t sp = first.find(' ');
            if (sp != std::string::npos) {
                const char* q = first.c_str() + sp + 1;
                while (*q == ' ') ++q;
                res.status = std::strtol(q, nullptr, 10);
            }
        }
    }
    res.ok = (res.status >= 200 && res.status < 300);

    // [Fix 小-2] 读取 stderr
    {
        std::string e = util::read_file(err_file);
        if (!e.empty() && e.size() > 4000) e = e.substr(0, 4000);
        res.stderr_text = e;
    }

    ::unlink(body_file.c_str());
    ::unlink(hdr_file.c_str());
    ::unlink(err_file.c_str());
    return res;
}

} // namespace httpc