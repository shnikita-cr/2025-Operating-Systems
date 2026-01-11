#pragma once

#include "core/protocol.h"

#include <memory>
#include <string>
#include <vector>

class IClientLink {
public:
    virtual ~IClientLink() = default;

    virtual int clientId() const = 0;

    virtual std::string nick() const = 0;

    virtual bool requestTurn(const TurnRequest &req, int timeoutMs, TurnResponse &out) = 0;

    virtual bool sendResult(const ResultMessage &msg, int timeoutMs) = 0;

    virtual std::string lastError() const = 0;

    virtual void disconnect() = 0;
};

class IHostSession {
public:
    virtual ~IHostSession() = default;

    virtual void startAccepting() = 0;

    virtual std::vector<std::shared_ptr<IClientLink>> clientsSnapshot() = 0;

    virtual std::string lastError() const = 0;

    virtual void shutdown() = 0;
};

class IClientSession {
public:
    virtual ~IClientSession() = default;

    virtual bool connect() = 0;

    virtual bool waitTurnRequest(int timeoutMs, TurnRequest &outReq) = 0;

    virtual bool sendTurn(const TurnResponse &resp, int timeoutMs) = 0;

    virtual bool waitResult(int timeout_ms, ResultMessage &outMsg) = 0;

    virtual std::string lastError() const = 0;

    virtual void shutdown() = 0;
};
