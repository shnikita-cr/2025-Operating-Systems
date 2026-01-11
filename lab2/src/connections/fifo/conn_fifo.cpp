#include "conn_fifo.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>
#include <thread>

static long monoMsFifo() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static std::string errnoStrFifo(const char *what) {
    std::ostringstream oss;
    oss << what << ": errno=" << errno << " (" << std::strerror(errno) << ")";
    return oss.str();
}

ConnFifo::ConnFifo() = default;

ConnFifo::ConnFifo(int fdRead, int fdWrite) : fd_read_(fdRead), fd_write_(fdWrite) {}

ConnFifo::~ConnFifo() {
    close();
}

ConnFifo::ConnFifo(ConnFifo &&other) noexcept {
    fd_read_ = other.fd_read_;
    fd_write_ = other.fd_write_;
    last_error_ = other.last_error_;
    other.fd_read_ = -1;
    other.fd_write_ = -1;
    other.last_error_.clear();
}

ConnFifo &ConnFifo::operator=(ConnFifo &&other) noexcept {
    if (this == &other) return *this;
    close();
    fd_read_ = other.fd_read_;
    fd_write_ = other.fd_write_;
    last_error_ = other.last_error_;
    other.fd_read_ = -1;
    other.fd_write_ = -1;
    other.last_error_.clear();
    return *this;
}

void ConnFifo::close() {
    if (fd_read_ >= 0) {
        ::close(fd_read_);
        fd_read_ = -1;
    }
    if (fd_write_ >= 0) {
        ::close(fd_write_);
        fd_write_ = -1;
    }
}

bool ConnFifo::pollFd(int fd, short events, int timeoutMs, std::string &err) const {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;

    while (true) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            err = errnoStrFifo("poll");
            return false;
        }
        if (rc == 0) {
            err = "poll timeout";
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            err = "poll error/hup/nval";
            return false;
        }
        if (pfd.revents & events) return true;
    }
}

bool ConnFifo::readExact(void *buf, std::size_t count, int timeoutMs) {
    if (fd_read_ < 0) {
        setError("readExact: invalid read fd");
        return false;
    }
    if (count == 0) return true;

    char *p = static_cast<char *>(buf);
    std::size_t left = count;

    const long deadline = monoMsFifo() + timeoutMs;
    while (left > 0) {
        const long now = monoMsFifo();
        const long remain = deadline - now;
        if (remain <= 0) {
            setError("readExact timeout");
            return false;
        }

        std::string e;
        if (!pollFd(fd_read_, POLLIN, static_cast<int>(remain), e)) {
            setError("readExact poll: " + e);
            return false;
        }

        const ssize_t r = ::read(fd_read_, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            setError(errnoStrFifo("read"));
            return false;
        }
        if (r == 0) {
            setError("read EOF");
            return false;
        }

        p += static_cast<std::size_t>(r);
        left -= static_cast<std::size_t>(r);
    }

    return true;
}

bool ConnFifo::writeExact(const void *buf, std::size_t count, int timeoutMs) {
    if (fd_write_ < 0) {
        setError("writeExact: invalid write fd");
        return false;
    }
    if (count == 0) return true;

    const char *p = static_cast<const char *>(buf);
    std::size_t left = count;

    const long deadline = monoMsFifo() + timeoutMs;
    while (left > 0) {
        const long now = monoMsFifo();
        const long remain = deadline - now;
        if (remain <= 0) {
            setError("writeExact timeout");
            return false;
        }

        std::string e;
        if (!pollFd(fd_write_, POLLOUT, static_cast<int>(remain), e)) {
            setError("writeExact poll: " + e);
            return false;
        }

        const ssize_t w = ::write(fd_write_, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            setError(errnoStrFifo("write"));
            return false;
        }
        if (w == 0) continue;

        p += static_cast<std::size_t>(w);
        left -= static_cast<std::size_t>(w);
    }

    return true;
}

std::string ConnFifo::makePathC2H(int hostPid, int clientId) {
    return "/tmp/lab2_v10_fifo_" + std::to_string(hostPid) + "_" + std::to_string(clientId) + "_c2h";
}

std::string ConnFifo::makePathH2C(int hostPid, int clientId) {
    return "/tmp/lab2_v10_fifo_" + std::to_string(hostPid) + "_" + std::to_string(clientId) + "_h2c";
}

bool ConnFifo::ensureFifo(const std::string &path, std::string &err) {
    if (::mkfifo(path.c_str(), 0666) != 0) {
        if (errno == EEXIST) return true;
        err = errnoStrFifo(("mkfifo " + path).c_str());
        return false;
    }
    return true;
}

bool ConnFifo::openRdwrNonBlock(const std::string &path, int &out_fd, std::string &err) {
    out_fd = -1;
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        err = errnoStrFifo(("open " + path).c_str());
        return false;
    }
    out_fd = fd;
    return true;
}

bool ConnFifo::hostCreateAndOpen(int hostPid, int clientId,
                                 ConnFifo &out,
                                 std::string &outPathC2H,
                                 std::string &outPathH2C,
                                 std::string &err) {
    outPathC2H = makePathC2H(hostPid, clientId);
    outPathH2C = makePathH2C(hostPid, clientId);

    if (!ensureFifo(outPathC2H, err)) return false;
    if (!ensureFifo(outPathH2C, err)) return false;

    int fd_c2h = -1;
    int fd_h2c = -1;

    std::string e1, e2;
    if (!openRdwrNonBlock(outPathC2H, fd_c2h, e1)) {
        err = e1;
        return false;
    }
    if (!openRdwrNonBlock(outPathH2C, fd_h2c, e2)) {
        ::close(fd_c2h);
        err = e2;
        return false;
    }


    out = ConnFifo(fd_c2h, fd_h2c);
    return true;
}

bool ConnFifo::clientOpenWithRetry(int hostPid, int clientId, int timeoutMs,
                                   ConnFifo &out,
                                   std::string &outPathC2H,
                                   std::string &outPathH2C,
                                   std::string &err) {
    outPathC2H = makePathC2H(hostPid, clientId);
    outPathH2C = makePathH2C(hostPid, clientId);

    const long deadline = monoMsFifo() + timeoutMs;

    int fd_c2h = -1;
    int fd_h2c = -1;

    while (monoMsFifo() < deadline) {
        std::string e;
        if (fd_c2h < 0) {
            int fd = ::open(outPathC2H.c_str(), O_RDWR | O_NONBLOCK);
            if (fd >= 0) fd_c2h = fd;
        }
        if (fd_h2c < 0) {
            int fd = ::open(outPathH2C.c_str(), O_RDWR | O_NONBLOCK);
            if (fd >= 0) fd_h2c = fd;
        }

        if (fd_c2h >= 0 && fd_h2c >= 0) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (fd_c2h < 0 || fd_h2c < 0) {
        if (fd_c2h >= 0) ::close(fd_c2h);
        if (fd_h2c >= 0) ::close(fd_h2c);
        err = "clientOpenWithRetry timeout opening fifo files";
        return false;
    }

    out = ConnFifo(fd_h2c, fd_c2h);
    return true;
}
