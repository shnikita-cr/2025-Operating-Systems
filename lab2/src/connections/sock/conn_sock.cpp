#include "conn_sock.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static long monoMs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static std::string errnoStr(const char *what) {
    std::ostringstream oss;
    oss << what << ": errno=" << errno << " (" << std::strerror(errno) << ")";
    return oss.str();
}

ConnSock::ConnSock() = default;

ConnSock::ConnSock(int fd) : fd_(fd) {
    std::string e;
    (void) setNonBlocking(e);
}

ConnSock::~ConnSock() {
    close();
}

ConnSock::ConnSock(ConnSock &&other) noexcept {
    fd_ = other.fd_;
    last_error_ = other.last_error_;
    other.fd_ = -1;
    other.last_error_.clear();
}

ConnSock &ConnSock::operator=(ConnSock &&other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = other.fd_;
    last_error_ = other.last_error_;
    other.fd_ = -1;
    other.last_error_.clear();
    return *this;
}

bool ConnSock::setNonBlocking(std::string &err) {
    if (fd_ < 0) {
        err = "setNonBlocking: invalid fd";
        return false;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        err = errnoStr("fcntl(F_GETFL)");
        return false;
    }
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
        err = errnoStr("fcntl(F_SETFL,O_NONBLOCK)");
        return false;
    }
    return true;
}

bool ConnSock::pollRw(short events, int timeoutMs, std::string &err) const {
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = events;
    pfd.revents = 0;

    while (true) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            err = errnoStr("poll");
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

bool ConnSock::readExact(void *buf, std::size_t count, int timeoutMs) {
    if (fd_ < 0) {
        setError("readExact: invalid fd");
        return false;
    }
    if (count == 0) return true;

    char *p = static_cast<char *>(buf);
    std::size_t left = count;

    const long deadline = monoMs() + timeoutMs;
    while (left > 0) {
        const long now = monoMs();
        const long remain = deadline - now;
        if (remain <= 0) {
            setError("readExact timeout");
            return false;
        }

        std::string e;
        if (!pollRw(POLLIN, static_cast<int>(remain), e)) {
            setError("readExact poll: " + e);
            return false;
        }

        const ssize_t r = ::read(fd_, p, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            setError(errnoStr("read"));
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

bool ConnSock::writeExact(const void *buf, std::size_t count, int timeoutMs) {
    if (fd_ < 0) {
        setError("writeExact: invalid fd");
        return false;
    }
    if (count == 0) return true;

    const char *p = static_cast<const char *>(buf);
    std::size_t left = count;

    const long deadline = monoMs() + timeoutMs;
    while (left > 0) {
        const long now = monoMs();
        const long remain = deadline - now;
        if (remain <= 0) {
            setError("writeExact timeout");
            return false;
        }

        std::string e;
        if (!pollRw(POLLOUT, static_cast<int>(remain), e)) {
            setError("writeExact poll: " + e);
            return false;
        }

        const ssize_t w = ::write(fd_, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            setError(errnoStr("write"));
            return false;
        }
        if (w == 0) continue;

        p += static_cast<std::size_t>(w);
        left -= static_cast<std::size_t>(w);
    }

    return true;
}

void ConnSock::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool ConnSock::createListen(const std::string &addr, int port, int &outFd, std::string &err) {
    outFd = -1;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = errnoStr("socket");
        return false;
    }

    int yes = 1;
    (void) ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        err = "inet_pton failed for addr: " + addr;
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
        err = errnoStr("bind");
        ::close(fd);
        return false;
    }

    if (::listen(fd, 64) != 0) {
        err = errnoStr("listen");
        ::close(fd);
        return false;
    }


    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    outFd = fd;
    return true;
}

bool ConnSock::acceptOne(int listenFd, int timeoutMs, int &outFd, std::string &err) {
    outFd = -1;

    struct pollfd pfd;
    pfd.fd = listenFd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (true) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc < 0) {
            if (errno == EINTR) continue;
            err = errnoStr("poll(listen)");
            return false;
        }
        if (rc == 0) {
            err = "accept timeout";
            return false;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            err = "listen poll error/hup/nval";
            return false;
        }
        if (!(pfd.revents & POLLIN)) continue;

        sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int fd = ::accept(listenFd, reinterpret_cast<sockaddr *>(&peer), &plen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            err = errnoStr("accept");
            return false;
        }


        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) (void) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        outFd = fd;
        return true;
    }
}

bool ConnSock::connectTo(const std::string &host, int port, int timeoutMs, int &outFd, std::string &err) {
    outFd = -1;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = errnoStr("socket");
        return false;
    }


    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        err = "inet_pton failed for host: " + host;
        return false;
    }

    const int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
    if (rc == 0) {
        outFd = fd;
        return true;
    }
    if (rc < 0 && errno != EINPROGRESS) {
        err = errnoStr("connect");
        ::close(fd);
        return false;
    }


    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    while (true) {
        const int prc = ::poll(&pfd, 1, timeoutMs);
        if (prc < 0) {
            if (errno == EINTR) continue;
            err = errnoStr("poll(connect)");
            ::close(fd);
            return false;
        }
        if (prc == 0) {
            err = "connect timeout";
            ::close(fd);
            return false;
        }
        break;
    }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0) {
        err = errnoStr("getsockopt(SO_ERROR)");
        ::close(fd);
        return false;
    }
    if (soerr != 0) {
        errno = soerr;
        err = errnoStr("connect(SO_ERROR)");
        ::close(fd);
        return false;
    }

    outFd = fd;
    return true;
}