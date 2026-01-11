#include "connections/mq/conn_mq.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <fstream>
#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <thread>

static std::string errnoStrMq(const char *what) {
    std::ostringstream oss;
    oss << what << ": errno=" << errno << " (" << std::strerror(errno) << ")";
    return oss.str();
}

static long monoMsMq() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long readSysctlLong(const char *path, long fallback) {
    std::ifstream f(path);
    long v = fallback;
    if (f) f >> v;
    if (v <= 0) return fallback;
    return v;
}

static void getMqLimits(long &outMsgMax, long &outMsgsizeMax) {
    outMsgMax = readSysctlLong("/proc/sys/fs/mqueue/msg_max", 10);
    outMsgsizeMax = readSysctlLong("/proc/sys/fs/mqueue/msgsize_max", 8192);
    if (outMsgMax <= 0) outMsgMax = 10;
    if (outMsgsizeMax <= 0) outMsgsizeMax = 8192;
}

ConnMq::ConnMq() = default;

ConnMq::ConnMq(mqd_t mq_read, mqd_t mq_write, std::size_t msg_size)
        : mq_read_(mq_read), mq_write_(mq_write), msg_size_(msg_size) {}

ConnMq::~ConnMq() {
    close();
}

ConnMq::ConnMq(ConnMq &&other) noexcept {
    mq_read_ = other.mq_read_;
    mq_write_ = other.mq_write_;
    msg_size_ = other.msg_size_;
    in_buf_ = std::move(other.in_buf_);
    in_off_ = other.in_off_;
    last_error_ = other.last_error_;

    other.mq_read_ = (mqd_t) - 1;
    other.mq_write_ = (mqd_t) - 1;
    other.msg_size_ = 0;
    other.in_off_ = 0;
    other.last_error_.clear();
}

ConnMq &ConnMq::operator=(ConnMq &&other) noexcept {
    if (this == &other) return *this;
    close();

    mq_read_ = other.mq_read_;
    mq_write_ = other.mq_write_;
    msg_size_ = other.msg_size_;
    in_buf_ = std::move(other.in_buf_);
    in_off_ = other.in_off_;
    last_error_ = other.last_error_;

    other.mq_read_ = (mqd_t) - 1;
    other.mq_write_ = (mqd_t) - 1;
    other.msg_size_ = 0;
    other.in_off_ = 0;
    other.last_error_.clear();
    return *this;
}

void ConnMq::close() {
    if (mq_read_ != (mqd_t) - 1) {
        ::mq_close(mq_read_);
        mq_read_ = (mqd_t) - 1;
    }
    if (mq_write_ != (mqd_t) - 1) {
        ::mq_close(mq_write_);
        mq_write_ = (mqd_t) - 1;
    }
    msg_size_ = 0;
    in_buf_.clear();
    in_off_ = 0;
}

void ConnMq::unlinkQuiet(const std::string &name) {
    ::mq_unlink(name.c_str());
}

bool ConnMq::makeAbsRealtimeDeadline(int timeoutMs, struct timespec &outTs, std::string &err) {
    struct timespec now;
    if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
        err = errnoStrMq("clock_gettime(CLOCK_REALTIME)");
        return false;
    }

    long sec = now.tv_sec + timeoutMs / 1000;
    long nsec = now.tv_nsec + (timeoutMs % 1000) * 1000000L;
    if (nsec >= 1000000000L) {
        sec += 1;
        nsec -= 1000000000L;
    }

    outTs.tv_sec = sec;
    outTs.tv_nsec = nsec;
    return true;
}

bool ConnMq::timedReceiveOne(std::vector<unsigned char> &outMsg, int timeoutMs) {
    if (mq_read_ == (mqd_t) - 1) {
        setError("timedReceiveOne: invalid mq_read");
        return false;
    }
    if (msg_size_ == 0) {
        setError("timedReceiveOne: invalid msg_size");
        return false;
    }

    outMsg.assign(msg_size_, 0);

    struct timespec abs_ts;
    std::string e;
    if (!makeAbsRealtimeDeadline(timeoutMs, abs_ts, e)) {
        setError(e);
        return false;
    }

    unsigned int prio = 0;
    while (true) {
        const ssize_t r = ::mq_timedreceive(mq_read_,
                                            reinterpret_cast<char *>(outMsg.data()),
                                            outMsg.size(),
                                            &prio,
                                            &abs_ts);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == ETIMEDOUT) {
                setError("mq_timedreceive timeout");
                return false;
            }
            setError(errnoStrMq("mq_timedreceive"));
            return false;
        }
        outMsg.resize(static_cast<std::size_t>(r));
        return true;
    }
}

bool ConnMq::timedSendChunk(const unsigned char *data, std::size_t len, int timeoutMs) {
    if (mq_write_ == (mqd_t) - 1) {
        setError("timedSendChunk: invalid mq_write");
        return false;
    }
    if (msg_size_ == 0) {
        setError("timedSendChunk: invalid msg_size");
        return false;
    }
    if (len > msg_size_) {
        setError("timedSendChunk: len > msg_size");
        return false;
    }

    struct timespec abs_ts;
    std::string e;
    if (!makeAbsRealtimeDeadline(timeoutMs, abs_ts, e)) {
        setError(e);
        return false;
    }

    while (true) {
        const int rc = ::mq_timedsend(mq_write_,
                                      reinterpret_cast<const char *>(data),
                                      len,
                                      0,
                                      &abs_ts);
        if (rc == 0) return true;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) {
            setError("mq_timedsend timeout");
            return false;
        }
        setError(errnoStrMq("mq_timedsend"));
        return false;
    }
}

bool ConnMq::readExact(void *buf, std::size_t count, int timeoutMs) {
    if (mq_read_ == (mqd_t) - 1) {
        setError("readExact: invalid mq_read");
        return false;
    }
    if (count == 0) return true;

    unsigned char *out = static_cast<unsigned char *>(buf);
    std::size_t left = count;

    const long deadline = monoMsMq() + timeoutMs;

    while (left > 0) {
        if (in_off_ < in_buf_.size()) {
            const std::size_t avail = in_buf_.size() - in_off_;
            const std::size_t take = (avail < left) ? avail : left;
            std::memcpy(out, in_buf_.data() + in_off_, take);
            in_off_ += take;
            out += take;
            left -= take;

            if (in_off_ >= in_buf_.size()) {
                in_buf_.clear();
                in_off_ = 0;
            }
            continue;
        }

        const long now = monoMsMq();
        const long remain = deadline - now;
        if (remain <= 0) {
            setError("readExact timeout");
            return false;
        }

        std::vector<unsigned char> msg;
        if (!timedReceiveOne(msg, static_cast<int>(remain))) {
            return false;
        }

        in_buf_ = std::move(msg);
        in_off_ = 0;
    }

    return true;
}

bool ConnMq::writeExact(const void *buf, std::size_t count, int timeoutMs) {
    if (mq_write_ == (mqd_t) - 1) {
        setError("writeExact: invalid mq_write");
        return false;
    }
    if (count == 0) return true;
    if (msg_size_ == 0) {
        setError("writeExact: invalid msg_size");
        return false;
    }

    const unsigned char *p = static_cast<const unsigned char *>(buf);
    std::size_t left = count;

    const long deadline = monoMsMq() + timeoutMs;
    while (left > 0) {
        const long now = monoMsMq();
        const long remain = deadline - now;
        if (remain <= 0) {
            setError("writeExact timeout");
            return false;
        }

        const std::size_t chunk = (left > msg_size_) ? msg_size_ : left;
        if (!timedSendChunk(p, chunk, static_cast<int>(remain))) {
            return false;
        }
        p += chunk;
        left -= chunk;
    }

    return true;
}

std::string ConnMq::makeNameC2H(int hostPid, int clientId) {
    return "/lab2_v10_mq_" + std::to_string(hostPid) + "_" + std::to_string(clientId) + "_c2h";
}

std::string ConnMq::makeNameH2C(int hostPid, int clientId) {
    return "/lab2_v10_mq_" + std::to_string(hostPid) + "_" + std::to_string(clientId) + "_h2c";
}

static bool mqOpenHostSafe(const std::string &name, mqd_t &out, std::size_t &out_msgsize, std::string &err) {

    long msg_max = 0;
    long msgsize_max = 0;
    getMqLimits(msg_max, msgsize_max);

    long want_maxmsg = 10;
    long want_msgsize = 1024;

    want_maxmsg = std::min(want_maxmsg, msg_max);
    want_msgsize = std::min(want_msgsize, msgsize_max);

    if (want_maxmsg <= 0) want_maxmsg = std::max(1L, msg_max);
    if (want_msgsize <= 0) want_msgsize = std::max(256L, msgsize_max);

    struct mq_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.mq_flags = 0;
    attr.mq_maxmsg = want_maxmsg;
    attr.mq_msgsize = want_msgsize;

    const mqd_t q = ::mq_open(name.c_str(), O_CREAT | O_RDWR, 0666, &attr);
    if (q == (mqd_t) - 1) {
        std::ostringstream oss;
        oss << "mq_open(host) " << name << ": errno=" << errno << " (" << std::strerror(errno) << ")"
            << "; limits: msg_max=" << msg_max << " msgsize_max=" << msgsize_max
            << "; chosen: mq_maxmsg=" << want_maxmsg << " mq_msgsize=" << want_msgsize;

        if (errno == ENOENT) {
            oss << "; hint: /dev/mqueue might be missing inside container";
        }
        err = oss.str();
        return false;
    }

    struct mq_attr real_attr;
    std::memset(&real_attr, 0, sizeof(real_attr));
    if (::mq_getattr(q, &real_attr) != 0) {
        ::mq_close(q);
        err = errnoStrMq(("mq_getattr " + name).c_str());
        return false;
    }

    out = q;
    out_msgsize = static_cast<std::size_t>(real_attr.mq_msgsize);
    return true;
}

static bool
mqOpenClientRetry(const std::string &name, int timeout_ms, mqd_t &out, std::size_t &out_msgsize, std::string &err) {
    const long deadline = monoMsMq() + timeout_ms;

    while (monoMsMq() < deadline) {
        const mqd_t q = ::mq_open(name.c_str(), O_RDWR);
        if (q != (mqd_t) - 1) {
            struct mq_attr real_attr;
            std::memset(&real_attr, 0, sizeof(real_attr));
            if (::mq_getattr(q, &real_attr) != 0) {
                ::mq_close(q);
                err = errnoStrMq(("mq_getattr " + name).c_str());
                return false;
            }
            out = q;
            out_msgsize = static_cast<std::size_t>(real_attr.mq_msgsize);
            return true;
        }

        if (errno != ENOENT) {
            err = errnoStrMq(("mq_open(client) " + name).c_str());
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    err = "mq_open retry timeout for " + name;
    return false;
}

bool ConnMq::hostCreateAndOpen(int hostPid, int clientId,
                               ConnMq &out,
                               std::string &outNameC2H,
                               std::string &outNameH2C,
                               std::string &err) {
    outNameC2H = makeNameC2H(hostPid, clientId);
    outNameH2C = makeNameH2C(hostPid, clientId);

    unlinkQuiet(outNameC2H);
    unlinkQuiet(outNameH2C);

    mqd_t q_c2h = (mqd_t) - 1;
    mqd_t q_h2c = (mqd_t) - 1;
    std::size_t ms1 = 0, ms2 = 0;

    if (!mqOpenHostSafe(outNameC2H, q_c2h, ms1, err)) return false;
    if (!mqOpenHostSafe(outNameH2C, q_h2c, ms2, err)) {
        ::mq_close(q_c2h);
        unlinkQuiet(outNameC2H);
        return false;
    }

    const std::size_t msg_size = (ms1 < ms2) ? ms1 : ms2;
    out = ConnMq(q_c2h, q_h2c, msg_size);
    return true;
}

bool ConnMq::clientOpenWithRetry(int hostPid, int clientId, int timeoutMs,
                                 ConnMq &out,
                                 std::string &outNameC2H,
                                 std::string &outNameH2C,
                                 std::string &err) {
    outNameC2H = makeNameC2H(hostPid, clientId);
    outNameH2C = makeNameH2C(hostPid, clientId);

    mqd_t q_c2h = (mqd_t) - 1;
    mqd_t q_h2c = (mqd_t) - 1;
    std::size_t ms1 = 0, ms2 = 0;

    if (!mqOpenClientRetry(outNameC2H, timeoutMs, q_c2h, ms1, err)) return false;
    if (!mqOpenClientRetry(outNameH2C, timeoutMs, q_h2c, ms2, err)) {
        ::mq_close(q_c2h);
        return false;
    }

    const std::size_t msg_size = (ms1 < ms2) ? ms1 : ms2;
    out = ConnMq(q_h2c, q_c2h, msg_size);
    return true;
}
