#pragma once

#include <cstdint>
#include <memory>
#include <string>

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

int getPid();

std::int64_t nowEpochSeconds();

std::string nowLocalMs();

int randIntInclusive(int min, int max);


bool readIntWithinMs(int timeoutMs, int min, int max, int *outValue);

bool ensureDirRecursive(const std::string &path, std::string *err);

std::string joinPath(const std::string &a, const std::string &b);

bool writeTextFileAtomic(const std::string &path, const std::string &content, std::string *err);

void installTerminationSignalHandlers();

bool stopRequested();

class Logger {
public:
    Logger() = default;

    Logger(const std::string &baseLogDir,
           const std::string &transportCode,
           const std::string &role,
           const std::string &nick,
           const std::string &fileName);

    void log(LogLevel lvl, const std::string &msg);

    void debug(const std::string &msg) { log(LogLevel::DEBUG, msg); }

    void info(const std::string &msg) { log(LogLevel::INFO, msg); }

    void warn(const std::string &msg) { log(LogLevel::WARN, msg); }

    void error(const std::string &msg) { log(LogLevel::ERROR, msg); }

private:
    int fd_ = -1;
    bool ok_ = false;

    std::string transport_code_;
    std::string role_;
    std::string nick_;

    void writeLine(const std::string &line);

    static const char *levelToStr(LogLevel lvl);
};
