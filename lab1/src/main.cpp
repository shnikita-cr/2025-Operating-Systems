#include "daemon/daemon.hpp"
#include "logger/logger.hpp"
#include "util/util.hpp"

#include <cstdlib>
#include <string>
#include <csignal>

static unsigned env_uint(const char *name, unsigned defv) {
    if (const char *v = std::getenv(name)) {
        try { return std::stoul(v); } catch (...) {}
    }
    return defv;
}

static std::string env_str(const char *name, const char *defv) {
    if (const char *v = std::getenv(name)) return std::string(v);
    return std::string(defv);
}

static void setup_signals() {
    struct sigaction sa{};
    sa.sa_handler = [](int s) {
        if (s == SIGHUP) g_reload.store(true);
        else if (s == SIGTERM || s == SIGINT)
            g_stop.store(true);
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

int main(int argc, char **argv) {
    const bool foreground = std::getenv("RUN_FOREGROUND") ? true : false;
    const std::string cfgPath = (argc > 1) ? std::string(argv[1]) : env_str("CONFIG_PATH", "./config.txt");
    const std::string pidPath = env_str("PID_FILE", foreground ? "./lab1.pid" : "/var/run/lab1.pid");
    const unsigned interval = env_uint("INTERVAL_SEC", 20);

    Logger::instance().init(foreground);
    setup_signals();

    auto &D = Daemon::instance();
    D.set_paths(cfgPath, pidPath);
    D.set_foreground(foreground);
    D.set_interval(interval);

    std::string err;
    if (!D.load_config(err)) {
        Logger::instance().error("Config load failed: " + err);
        return 1;
    }
    if (!D.daemonize(err)) {
        Logger::instance().error("Daemonize failed: " + err);
        return 1;
    }
    if (!D.acquire_pidfile(err)) {
        Logger::instance().error("PID file failed: " + err);
        return 1;
    }

    D.run_loop();
    D.release_pidfile();
    return 0;
}