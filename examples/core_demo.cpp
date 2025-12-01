#include <bcnp/message_types.h>
#include "bcnp/controller.h"
#include "bcnp/transport/tcp_posix.h"
#include "bcnp/transport/controller_driver.h"
#include "bcnp/packet.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>
#include <csignal>
#include <atomic>
#include <iomanip>

using namespace std::chrono_literals;

std::atomic<bool> g_running{true};

void SignalHandler(int) {
    g_running = false;
}

// Basic demo config, replace with your own.
bcnp::ControllerConfig MakeDemoConfig() {
    bcnp::ControllerConfig config;
    config.limits.vxMax = 1.5f;
    config.limits.vxMin = -1.5f;
    config.limits.omegaMax = 2.5f;
    config.limits.omegaMin = -2.5f;
    config.limits.durationMin = 0;
    config.limits.durationMax = 65535;
    
    config.queue.connectionTimeout = 200ms;
    config.queue.maxCommandLag = 5000ms;
    config.queue.capacity = bcnp::kMaxCommandsPerPacket;
    config.parserBufferSize = bcnp::kMaxPacketSize;
    
    return config;
}

int main() {
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    constexpr uint16_t kPort = 5800;
    
    std::cout << "[Server] BCNP v" << int(bcnp::kProtocolMajorV3) << "." 
              << int(bcnp::kProtocolMinorV3) << " TCP Demo\n";
    std::cout << "[Server] Schema hash: 0x" << std::hex << std::setfill('0') 
              << std::setw(8) << bcnp::kSchemaHash << std::dec << "\n";
    std::cout << "[Server] Message types: DriveCmd(id=" << int(bcnp::MessageTypeId::DriveCmd)
              << ", " << bcnp::kDriveCmdSize << "B)\n";
    std::cout << "[Server] Listening on port " << kPort << "...\n";
    std::cout << "[Server] Press Ctrl+C to stop.\n\n";

    // 1. Setup Controller
    bcnp::Controller controller(MakeDemoConfig());

    // 2. Setup Transport (Server) - handshake enabled by default
    bcnp::TcpPosixAdapter serverAdapter(kPort);
    
    // 3. Setup Driver (Connects Transport -> Controller)
    bcnp::ControllerDriver driver(controller, serverAdapter);

    // 4. Main Loop
    while (g_running) {
        // Poll network
        driver.PollOnce();

        // Update controller time
        auto now = std::chrono::steady_clock::now();
        controller.Queue().Update(now);

        // Check status
        if (controller.IsConnected(now)) {
            auto cmd = controller.CurrentCommand(now);
            if (cmd) {
                std::cout << "[Server] Executing: vx=" << cmd->vx 
                          << " omega=" << cmd->omega 
                          << " duration=" << cmd->durationMs << "ms"
                          << " QueueSize=" << controller.Queue().Size() << "\n";
            } else {
                std::cout << "[Server] Connected, Idle. QueueSize=" << controller.Queue().Size() << "\n";
            }
        } else {
            // Only print occasionally to avoid spamming
            static int counter = 0;
            if (counter++ % 10 == 0) {
                std::cout << "[Server] Waiting for connection (schema 0x" 
                          << std::hex << bcnp::kSchemaHash << std::dec << ")...\n";
            }
        }

        std::this_thread::sleep_for(100ms);
    }

    std::cout << "[Server] Demo finished.\n";
    return 0;
}
