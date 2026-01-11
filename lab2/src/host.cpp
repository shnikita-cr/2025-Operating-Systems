#include "transport/transport_factory.h"
#include "core/game_engine.h"
#include "common/common.h"

#include <cstdio>

int main(int argc, char **argv) {
    HostRuntime rt = createHostRuntime(argc, argv);
    if (!rt.session || !rt.log) {
        std::fprintf(stderr, "HostRuntime is not initialized (session/log is null)\n");
        return 2;
    }

    installTerminationSignalHandlers();

    std::string err;
    if (!writeTextFileAtomic("/app/run/host.pid", std::to_string(getPid()) + "\n", &err)) {
        rt.log->warn("Failed to write /app/run/host.pid: " + err);
    } else {
        rt.log->info("Wrote host pid to /app/run/host.pid: " + std::to_string(getPid()));
    }

    return runHost(*rt.session, rt.args, *rt.log);
}
