// main.cpp — cppdesk step 14 (part 11 of upstream d1013487 "source code").
//
// src/main.rs argv dispatch is deferred until connection/platform/ui land,
// so the binary stays a step placeholder. It now links the IPC layer
// (src/ipc.rs + libs/parity-tokio-ipc) alongside the server foundation,
// client, mediator and hbb_common.
//
// Build + test happen in GitHub Actions (.github/workflows/ci.yml); see
// README.md for the equivalent local commands.

#include <cppdesk/version.hpp>

#include <iostream>

int main() {
    std::cout << "cppdesk " << cppdesk::kVersion
              << " step 14 (d1013487 part 11): + ipc — connection/ui/platform pending.\n";
    return 0;
}
