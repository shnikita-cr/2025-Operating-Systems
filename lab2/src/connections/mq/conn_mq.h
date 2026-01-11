#pragma once

#include "connections/base/conn_iface.h"

#include <string>
#include <vector>

#include <mqueue.h>

class ConnMq final : public ConnIface {
public:
    ConnMq();

    ConnMq(mqd_t mq_read, mqd_t mq_write, std::size_t msg_size);

    ~ConnMq() override;

    ConnMq(const ConnMq &) = delete;

    ConnMq &operator=(const ConnMq &) = delete;

    ConnMq(ConnMq &&other) noexcept;

    ConnMq &operator=(ConnMq &&other) noexcept;

    bool IsValid() const { return mq_read_ != (mqd_t) - 1 && mq_write_ != (mqd_t) - 1; }

    bool readExact(void *buf, std::size_t count, int timeoutMs) override;

    bool writeExact(const void *buf, std::size_t count, int timeoutMs) override;

    void close() override;

    std::string lastError() const override { return last_error_; }

    static std::string makeNameC2H(int hostPid, int clientId);

    static std::string makeNameH2C(int hostPid, int clientId);

    static bool hostCreateAndOpen(int hostPid, int clientId,
                                  ConnMq &out,
                                  std::string &outNameC2H,
                                  std::string &outNameH2C,
                                  std::string &err);

    static bool clientOpenWithRetry(int hostPid, int clientId, int timeoutMs,
                                    ConnMq &out,
                                    std::string &outNameC2H,
                                    std::string &outNameH2C,
                                    std::string &err);

    static void unlinkQuiet(const std::string &name);

private:
    mqd_t mq_read_ = (mqd_t) - 1;
    mqd_t mq_write_ = (mqd_t) - 1;
    std::size_t msg_size_ = 0;

    std::vector<unsigned char> in_buf_;
    std::size_t in_off_ = 0;

    std::string last_error_;

    void setError(const std::string &e) { last_error_ = e; }

    static bool makeAbsRealtimeDeadline(int timeoutMs, struct timespec &outTs, std::string &err);

    bool timedReceiveOne(std::vector<unsigned char> &outMsg, int timeoutMs);

    bool timedSendChunk(const unsigned char *data, std::size_t len, int timeoutMs);
};
