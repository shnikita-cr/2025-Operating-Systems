#include "logger.hpp"
#include <iostream>

Logger &Logger::instance() {
    static Logger L;
    return L;
}

void Logger::init(bool foreground) {
    _foreground = foreground;
    if (!_foreground) openlog("lab1", LOG_PID | LOG_CONS, LOG_DAEMON);
}

void Logger::info(const std::string &m) const {
    if (_foreground) std::cerr << "[INFO]  " << m << "\n";
    else
        syslog(LOG_INFO, "%s", m.c_str());
}

void Logger::warn(const std::string &m) const {
    if (_foreground) std::cerr << "[WARN]  " << m << "\n";
    else
        syslog(LOG_WARNING, "%s", m.c_str());
}

void Logger::error(const std::string &m) const {
    if (_foreground) std::cerr << "[ERROR] " << m << "\n";
    else
        syslog(LOG_ERR, "%s", m.c_str());
}