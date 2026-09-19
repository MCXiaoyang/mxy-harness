#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include "store.hpp"
#include "server.hpp"
#include "http.hpp"
#include "util.hpp"

static void usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "  --sock <path>   Unix socket path (default: $XDG_RUNTIME_DIR/mxy-harness.sock\n"
        "                                     or ~/.mxy-harness/daemon.sock)\n"
        "  --home <dir>    Data dir (default: $HOME/.mxy-harness)\n"
        "  --version       Print version\n"
        "  -h, --help      Show this help\n",
        prog);
}

// [Fix #10] 优先 XDG_RUNTIME_DIR，退回到数据目录
static std::string default_sock_path() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return std::string(xdg) + "/mxy-harness.sock";
    return util::data_dir() + "/daemon.sock";
}

int main(int argc, char** argv) {
    std::string sock_path = default_sock_path();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sock" && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (a == "--home" && i + 1 < argc) {
            ::setenv("MXY_HARNESS_HOME", argv[++i], 1);
            if (sock_path == default_sock_path() || sock_path.empty())
                sock_path = default_sock_path();
        } else if (a == "--version") {
            std::printf("mxy-harnessd 1.0.0\n");
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);

    // [Fix #5] 准备并清空临时目录
    {
        std::string base = util::data_dir();
        util::mkdir_p(base);
        std::string tmp = base + "/tmp";
        util::mkdir_p(tmp);
        util::clean_tmp_files(tmp);
        httpc::set_tmp_dir(tmp);
    }

    Store store;
    store.ensure_default_model();

    Server server(store, sock_path);
    server.run();
    return 0;
}