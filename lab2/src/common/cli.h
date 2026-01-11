#pragma once

#include "transport/transport_factory.h"

#include <string>

bool parseHostArgs(int argc, char **argv, HostArgs &out, std::string *err);

bool parseClientArgs(int argc, char **argv, ClientArgs &out, std::string *err);
