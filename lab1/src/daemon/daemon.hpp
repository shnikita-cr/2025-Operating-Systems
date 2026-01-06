#pragma once

#include "../config/config.hpp"
#include <string>
#include <filesystem>

class Daemon {
public:
    static Daemon &instance();

    // Configure runtime
    void set_paths(std::filesystem::path config, std::filesystem::path pidfile);

    void set_foreground(bool fg);

    void set_interval(unsigned sec);

    // Lifecycle
    bool acquire_pidfile(std::string &err); // exclusive lock+write pid
    void release_pidfile();

    bool daemonize(std::string &err); // setsid + detach stdio (when !foreground)

    bool load_config(std::string &err);

    void run_loop();

private:
    std::filesystem::path _config_path = "./config.txt";
    std::filesystem::path _pidfile_path;
    bool _foreground = true;
    unsigned _interval_sec = 60;
    int _pid_fd = -1;
    Config _cfg;
};