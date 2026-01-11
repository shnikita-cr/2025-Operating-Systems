#pragma once

#include "connections/base/conn_iface.h"

#include <string>


class ConnFifo final : public ConnIface {
public:
    ConnFifo();

    ConnFifo(int fdRead, int fdWrite);

    ~ConnFifo() override;

    ConnFifo(const ConnFifo &) = delete;

    ConnFifo &operator=(const ConnFifo &) = delete;

    ConnFifo(ConnFifo &&other) noexcept;

    ConnFifo &operator=(ConnFifo &&other) noexcept;

    bool isValid() const { return fd_read_ >= 0 && fd_write_ >= 0; }

    bool readExact(void *buf, std::size_t count, int timeoutMs) override;

    bool writeExact(const void *buf, std::size_t count, int timeoutMs) override;

    void close() override;

    std::string lastError() const override { return last_error_; }

    static std::string makePathC2H(int hostPid, int clientId);

    static std::string makePathH2C(int hostPid, int clientId);

    static bool hostCreateAndOpen(int hostPid, int clientId,
                                  ConnFifo &out,
                                  std::string &outPathC2H,
                                  std::string &outPathH2C,
                                  std::string &err);


    static bool clientOpenWithRetry(int hostPid, int clientId, int timeoutMs,
                                    ConnFifo &out,
                                    std::string &outPathC2H,
                                    std::string &outPathH2C,
                                    std::string &err);

private:
    int fd_read_ = -1;
    int fd_write_ = -1;
    std::string last_error_;

    bool pollFd(int fd, short events, int timeoutMs, std::string &err) const;

    void setError(const std::string &e) { last_error_ = e; }

    static bool ensureFifo(const std::string &path, std::string &err);

    static bool openRdwrNonBlock(const std::string &path, int &out_fd, std::string &err);
};
