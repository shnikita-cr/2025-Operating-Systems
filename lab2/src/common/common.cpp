#include "common.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <csignal>

static std::atomic<bool> gStop(false);

static void onTerm(int) { gStop.store(true); }

int getPid() { return static_cast<int>(::getpid()); }

bool stopRequested() { return gStop.load(); }

void installTerminationSignalHandlers() {
    std::signal(SIGINT, onTerm);
    std::signal(SIGTERM, onTerm);
}

std::int64_t nowEpochSeconds() {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec);
}

std::string nowLocalMs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);

    time_t t = static_cast<time_t>(ts.tv_sec);
    struct tm tmv;
    ::localtime_r(&t, &tmv);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  ts.tv_nsec / 1000000L);
    return std::string(buf);
}

int randIntInclusive(int min, int max) {
    static thread_local std::mt19937 rng([] {
        std::random_device rd;
        std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32)
                             ^ static_cast<std::uint64_t>(rd())
                             ^ static_cast<std::uint64_t>(::getpid())
                             ^ static_cast<std::uint64_t>(::time(nullptr));
        return static_cast<std::mt19937::result_type>(seed);
    }());
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

bool readIntWithinMs(int timeoutMs, int min, int max, int *outValue) {
    if (!outValue) return false;

    struct timespec startTs;
    ::clock_gettime(CLOCK_MONOTONIC, &startTs);
    const long startMs = startTs.tv_sec * 1000L + startTs.tv_nsec / 1000000L;
    const long deadlineMs = startMs + timeoutMs;

    std::string line;
    line.reserve(64);

    while (true) {
        struct timespec now_ts;
        ::clock_gettime(CLOCK_MONOTONIC, &now_ts);
        const long now_ms = now_ts.tv_sec * 1000L + now_ts.tv_nsec / 1000000L;

        long remain = deadlineMs - now_ms;
        if (remain <= 0) break;

        const int slice = static_cast<int>(remain > 50 ? 50 : remain);

        struct pollfd pfd;
        pfd.fd = 0;
        pfd.events = POLLIN;
        pfd.revents = 0;

        const int rc = ::poll(&pfd, 1, slice);
        if (rc < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        if (rc == 0) continue;

        if (pfd.revents & POLLIN) {
            line.clear();
            char ch = 0;

            while (true) {
                const ssize_t r = ::read(0, &ch, 1);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) {

                    break;
                }
                if (ch == '\n') break;
                if (ch == '\r') continue;
                if (line.size() < 63) line.push_back(ch);
            }

            try {
                const int v = std::stoi(line);
                if (v >= min && v <= max) {
                    *outValue = v;
                    return true;
                }
            } catch (...) {

            }
        }
    }

    return false;
}

static bool mkdirOne(const std::string &path, std::string *err) {
    if (path.empty()) return true;
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    if (errno == EEXIST) return true;
    if (err) {
        std::ostringstream oss;
        oss << "mkdir('" << path << "') failed: errno=" << errno << " (" << std::strerror(errno) << ")";
        *err = oss.str();
    }
    return false;
}

bool ensureDirRecursive(const std::string &path, std::string *err) {
    if (path.empty()) return true;

    std::string cur;
    size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        cur = "/";
        i = 1;
    }

    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') i++;
        if (i >= path.size()) break;
        size_t j = i;
        while (j < path.size() && path[j] != '/') j++;

        const std::string part = path.substr(i, j - i);
        if (!part.empty()) {
            if (cur.size() > 1 && cur.back() != '/') cur.push_back('/');
            cur += part;
            if (!mkdirOne(cur, err)) return false;
        }
        i = j;
    }
    return true;
}

std::string joinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool writeTextFileAtomic(const std::string &path, const std::string &content, std::string *err) {
    std::ostringstream oss;
    oss << path << ".tmp." << ::getpid() << "." << randIntInclusive(1000, 999999);
    const std::string tmp = oss.str();

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (err) {
            std::ostringstream e;
            e << "open(tmp) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = e.str();
        }
        return false;
    }

    const char *p = content.c_str();
    size_t left = content.size();
    while (left > 0) {
        const ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            if (err) {
                std::ostringstream e;
                e << "write(tmp) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
                *err = e.str();
            }
            return false;
        }
        p += static_cast<size_t>(w);
        left -= static_cast<size_t>(w);
    }

    ::fsync(fd);
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        if (err) {
            std::ostringstream e;
            e << "rename(tmp->path) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = e.str();
        }
        return false;
    }

    return true;
}

const char *Logger::levelToStr(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
        default:
            return "INFO";
    }
}

Logger::Logger(const std::string &baseLogDir,
               const std::string &transportCode,
               const std::string &role,
               const std::string &nick,
               const std::string &fileName)
        : transport_code_(transportCode),
          role_(role),
          nick_(nick) {

    std::string dir = joinPath(baseLogDir, transport_code_);
    std::string e;
    ensureDirRecursive(dir, &e);

    const std::string filePath = joinPath(dir, fileName);

    fd_ = ::open(filePath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ >= 0) ok_ = true;
}

void Logger::writeLine(const std::string &line) {
    if (!ok_) {
        (void) ::write(2, line.c_str(), line.size());
        return;
    }
    (void) ::write(fd_, line.c_str(), line.size());
}

void Logger::log(LogLevel lvl, const std::string &msg) {
    std::ostringstream oss;
    oss << nowEpochSeconds() << " " << nowLocalMs()
        << " [" << transport_code_ << "]"
        << " [" << role_ << "]"
        << " [" << nick_ << "] "
        << levelToStr(lvl) << " "
        << msg << "\n";

    const std::string line = oss.str();
    writeLine(line);
    (void) ::write(1, line.c_str(), line.size());
}
