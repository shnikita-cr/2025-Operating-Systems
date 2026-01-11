#pragma once

#include <string>

struct HandshakeEvent {
    int clientPid = 0;
    int clientId = 0;
};

class HandshakeHost {
public:
    HandshakeHost();

    bool init(std::string *err);

    bool waitNextClient(int timeoutMs, HandshakeEvent &out_ev, std::string *err);

private:
    int next_id_ = 1;
    bool inited_ = false;
};

bool handshakeClient(int hostPid, int timeoutMs, int &outClientId, std::string *err);
