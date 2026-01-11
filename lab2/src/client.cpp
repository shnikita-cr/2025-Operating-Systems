#include "transport/transport_factory.h"
#include "core/game_engine.h"
#include "common/common.h"

#include <cstdio>

int main(int argc, char **argv) {
    ClientRuntime rt = createClientRuntime(argc, argv);
    if (!rt.session || !rt.log) {
        std::fprintf(stderr, "ClientRuntime is not initialized (session/log is null)\n");
        return 2;
    }

    installTerminationSignalHandlers();

    return runClient(*rt.session, rt.args, *rt.log);
}
