#include "sync_semaphore.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <time.h>
#include <unistd.h>

static void addMsToAbsTime(int timeout_ms, struct timespec &out_abs) {
    ::clock_gettime(CLOCK_REALTIME, &out_abs);

    const long addSec = timeout_ms / 1000;
    const long addNs = (timeout_ms % 1000) * 1000000L;

    out_abs.tv_sec += addSec;
    out_abs.tv_nsec += addNs;
    if (out_abs.tv_nsec >= 1000000000L) {
        out_abs.tv_sec += 1;
        out_abs.tv_nsec -= 1000000000L;
    }
}

NamedSemaphore::~NamedSemaphore() {
    close();
}

NamedSemaphore::NamedSemaphore(NamedSemaphore &&other) noexcept {
    sem_ = other.sem_;
    name_ = other.name_;
    owner_ = other.owner_;
    other.sem_ = nullptr;
    other.owner_ = false;
    other.name_.clear();
}

NamedSemaphore &NamedSemaphore::operator=(NamedSemaphore &&other) noexcept {
    if (this == &other) return *this;
    close();
    sem_ = other.sem_;
    name_ = other.name_;
    owner_ = other.owner_;
    other.sem_ = nullptr;
    other.owner_ = false;
    other.name_.clear();
    return *this;
}

NamedSemaphore
NamedSemaphore::create(const std::string &name, unsigned int initialValue, bool owner, std::string *err) {
    NamedSemaphore s;
    s.name_ = name;
    s.owner_ = owner;

    errno = 0;
    sem_t *sem = ::sem_open(name.c_str(), O_CREAT, 0666, initialValue);
    if (sem == SEM_FAILED) {
        if (err) {
            std::ostringstream oss;
            oss << "sem_open(create) failed name='" << name << "': errno=" << errno << " (" << std::strerror(errno)
                << ")";
            *err = oss.str();
        }
        s.sem_ = nullptr;
        return s;
    }

    s.sem_ = sem;
    return s;
}

NamedSemaphore NamedSemaphore::open(const std::string &name, bool owner, std::string *err) {
    NamedSemaphore s;
    s.name_ = name;
    s.owner_ = owner;

    errno = 0;
    sem_t *sem = ::sem_open(name.c_str(), 0);
    if (sem == SEM_FAILED) {
        if (err) {
            std::ostringstream oss;
            oss << "sem_open(open) failed name='" << name << "': errno=" << errno << " (" << std::strerror(errno)
                << ")";
            *err = oss.str();
        }
        s.sem_ = nullptr;
        return s;
    }

    s.sem_ = sem;
    return s;
}

bool NamedSemaphore::waitMs(int timeoutMs, std::string *err) {
    if (!isValid()) {
        if (err) *err = "waitMs called on invalid semaphore";
        return false;
    }

    struct timespec abs;
    addMsToAbsTime(timeoutMs, abs);

    while (true) {
        if (::sem_timedwait(sem_, &abs) == 0) return true;

        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) {
            if (err) *err = "sem_timedwait timeout";
            return false;
        }

        if (err) {
            std::ostringstream oss;
            oss << "sem_timedwait failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = oss.str();
        }
        return false;
    }
}

bool NamedSemaphore::post(std::string *err) {
    if (!isValid()) {
        if (err) *err = "post called on invalid semaphore";
        return false;
    }

    while (true) {
        if (::sem_post(sem_) == 0) return true;
        if (errno == EINTR) continue;
        if (err) {
            std::ostringstream oss;
            oss << "sem_post failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = oss.str();
        }
        return false;
    }
}

void NamedSemaphore::close() {
    if (isValid()) {
        ::sem_close(sem_);
        sem_ = nullptr;
    }
    if (owner_ && !name_.empty()) {
        ::sem_unlink(name_.c_str());
    }
    owner_ = false;
    name_.clear();
}

std::string makeSemName(const std::string &prefix, int hostPid, int clientId, const std::string &suffix) {
    std::ostringstream oss;
    oss << "/";
    oss << prefix;
    oss << "_";
    oss << hostPid;
    oss << "_";
    oss << clientId;
    if (!suffix.empty()) {
        oss << "_";
        oss << suffix;
    }
    return oss.str();
}
