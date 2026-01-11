#pragma once

#include "transport/transport_api.h"
#include "transport/transport_factory.h"
#include "common/common.h"


int runHost(IHostSession &session, const HostArgs &args, Logger &log);

int runClient(IClientSession &session, const ClientArgs &args, Logger &log);
