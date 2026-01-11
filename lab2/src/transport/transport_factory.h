#pragma once

#include "transport/transport_api.h"
#include "common/common.h"

#include <memory>
#include <string>


struct HostArgs {
    int rounds = 20;
    int timeoutMs = 5000;
    std::string logDir = "/etc/logs";
    std::string nick = "wolf";


    std::string addr = "0.0.0.0";
    int port = 5555;
};

struct ClientArgs {
    int hostPid = 0;
    int timeoutMs = 5000;
    std::string logDir = "/etc/logs";
    std::string nick = "kid";


    std::string hostAddr = "127.0.0.1";
    int hostPort = 5555;
};

struct HostRuntime {
    HostArgs args;
    std::string transportCode;
    std::unique_ptr<IHostSession> session;
    std::shared_ptr<Logger> log;
};

struct ClientRuntime {
    ClientArgs args;
    std::string transportCode;
    std::unique_ptr<IClientSession> session;
    std::shared_ptr<Logger> log;
};

HostRuntime createHostRuntime(int argc, char **argv);

ClientRuntime createClientRuntime(int argc, char **argv);
