#include "daemon.hpp"
#include "../logger/logger.hpp"
#include "../util/util.hpp"

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fstream>

using namespace std;
namespace fs = std::filesystem;

static bool file_exists(const fs::path &p) {
    error_code ec;
    return fs::exists(p, ec);
}

Daemon &Daemon::instance() {
    static Daemon D;
    return D;
}

void Daemon::set_paths(fs::path config, fs::path pidfile) {
    if (!config.empty()) _config_path = config;
    if (!pidfile.empty()) _pidfile_path = pidfile;
}

void Daemon::set_foreground(bool fg) { _foreground = fg; }

void Daemon::set_interval(unsigned s) { _interval_sec = s ? s : 60; }

bool Daemon::acquire_pidfile(std::string &err) {
    if (_pidfile_path.empty()) _pidfile_path = _foreground ? fs::path("./lab1.pid") : fs::path("/var/run/lab1.pid");
    int fd = ::open(_pidfile_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        err = string("open pidfile failed: ") + strerror(errno);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        err = string("another instance running (lock): ") + _pidfile_path.string();
        ::close(fd);
        return false;
    }
    // truncate and write pid
    if (ftruncate(fd, 0) != 0) {
        err = string("ftruncate pidfile: ") + strerror(errno);
        ::close(fd);
        return false;
    }
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%ld\n", (long) getpid());
    if (write(fd, buf, len) != len) {
        err = string("write pidfile: ") + strerror(errno);
        ::close(fd);
        return false;
    }
    _pid_fd = fd; // keep fd open to hold the lock
    return true;
}

void Daemon::release_pidfile() {
    if (_pid_fd >= 0) {
        flock(_pid_fd, LOCK_UN);
        ::close(_pid_fd);
        _pid_fd = -1;
    }
    // best-effort remove
    if (!_pidfile_path.empty()) {
        std::error_code ec;
        fs::remove(_pidfile_path, ec);
    }
}

bool Daemon::daemonize(std::string &err) {
    if (_foreground) return true; // no-op in Docker/foreground
    if (setsid() < 0) {
        err = string("setsid failed: ") + strerror(errno);
        return false;
    }
    // detach stdio → /dev/null
    int fd0 = open("/dev/null", O_RDWR);
    if (fd0 >= 0) {
        dup2(fd0, STDIN_FILENO);
        dup2(fd0, STDOUT_FILENO);
        dup2(fd0, STDERR_FILENO);
        if (fd0 > 2) close(fd0);
    }
    chdir("/");
    umask(0);
    return true;
}

bool Daemon::load_config(std::string &err) {
    Config tmp;
    if (!tmp.load_config(_config_path, tmp, err)) return false;
    _cfg = std::move(tmp);
    return true;
}

void Daemon::run_loop() {
    auto &log = Logger::instance();
    log.info("Started. Config: " + _config_path.string() + ", interval=" + to_string(_interval_sec) + "s");

    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            std::string e;
            if (load_config(e))
                log.info("Config reloaded");
            else
                log.error(string("Config reload failed: ") + e);
        }

        for (const auto &e: _cfg.entries) {
            const fs::path folder = e.folder;
            const fs::path marker = folder / e.ignfile;

            if (!fs::exists(folder)) {
                log.warn("Folder not found: " + folder.string());
                continue;
            }
            if (!fs::is_directory(folder)) {
                log.warn("Not a directory: " + folder.string());
                continue;
            }

            if (file_exists(marker)) {
                log.info("IGN present, skip: " + marker.string());
            } else {
                std::string err;
                if (remove_contents(folder, err))
                    log.info("Purged contents of: " + folder.string());
                else
                    log.error("Purge failed for " + folder.string() + ": " + err);
            }
        }

        sleep_interruptible(_interval_sec);
    }
    log.info("Stopping main loop");
}
