#pragma once

#include "connections/base/conn_iface.h"

#include <string>


class ConnSock final : public ConnIface {
public:
    ConnSock();

    explicit ConnSock(int fd);

    ~ConnSock() override;

    ConnSock(const ConnSock &) = delete;

    ConnSock &operator=(const ConnSock &) = delete;

    ConnSock(ConnSock &&other) noexcept;

    ConnSock &operator=(ConnSock &&other) noexcept;

    bool isValid() const { return fd_ >= 0; }

    int Fd() const { return fd_; }

    bool readExact(void *buf, std::size_t count, int timeoutMs) override;

    bool writeExact(const void *buf, std::size_t count, int timeoutMs) override;

    void close() override;

    std::string lastError() const override { return last_error_; }


    static bool createListen(const std::string &addr, int port, int &outFd, std::string &err);

    static bool acceptOne(int listenFd, int timeoutMs, int &outFd, std::string &err);

    static bool connectTo(const std::string &host, int port, int timeoutMs, int &outFd, std::string &err);

private:
    int fd_ = -1;
    std::string last_error_;

    bool pollRw(short events, int timeoutMs, std::string &err) const;

    bool setNonBlocking(std::string &err);

    void setError(const std::string &e) { last_error_ = e; }
};
