#pragma once

#include <string>
#include <syslog.h>

class Logger {
public:
    static Logger &instance();

    void init(bool foreground);

    void info(const std::string &m) const;

    void warn(const std::string &m) const;

    void error(const std::string &m) const;

private:
    bool _foreground = true;
};