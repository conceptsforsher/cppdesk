// main.cpp — cppdesk step 15 (part 12 of upstream d1013487 "source code").
//
// src/main.rs argv dispatch (--server, --cm, --service, ...) is deferred
// until the ui and the rest of the platform layer land, so the binary stays a
// step placeholder. It links the rustdesk library: the server connection
// loop and entry points (start_server, RendezvousMediator::start_all) on top
// of ipc, client, mediator and hbb_common.
//
// Build + test happen in GitHub Actions (.github/workflows/ci.yml); see
// README.md for the equivalent local commands.

#include <cppdesk/version.hpp>

#include <iostream>

int main() {
    std::cout << "cppdesk " << cppdesk::kVersion
              << " step 15 (d1013487 part 12): + server connection — ui/platform pending.\n";
    return 0;
}
