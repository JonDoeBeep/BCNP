#include <bcnp/message_types.h>
#include "bcnp/dispatcher.h"
#include "bcnp/message_queue.h"
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

int main() {
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    constexpr uint16_t kPort = 5800;
    
    std::cout << "[Server] BCNP v" << int(bcnp::kProtocolMajorV3) << "." 
              << int(bcnp::kProtocolMinorV3) << " TCP Demo\n";
    std::cout << "[Server] Schema hash: 0x" << std::hex << std::setfill('0') 
              << std::setw(8) << bcnp::kSchemaHash << std::dec << "\n";
    std::cout << "[Server] Define your message types in schema/messages.json\n";
    std::cout << "[Server] Listening on port " << kPort << "...\n";
    std::cout << "[Server] Press Ctrl+C to stop.\n\n";

    // 1. Setup Dispatcher
    bcnp::DispatcherConfig config;
    config.connectionTimeout = 200ms;
    bcnp::PacketDispatcher dispatcher(config);

    // 2. Setup Transport (Server) - handshake enabled by default
    bcnp::TcpPosixAdapter serverAdapter(kPort);
    
    // 3. Setup Driver (Connects Transport -> Dispatcher)
    bcnp::DispatcherDriver driver(dispatcher, serverAdapter);

    // Example: If you have defined a message type in your schema, you can register
    // handlers like this:
    //
    // bcnp::MessageQueue<MyMotorCmd> motorQueue;
    // dispatcher.RegisterHandler<MyMotorCmd>([&](const bcnp::PacketView& pkt) {
    //     for (auto it = pkt.begin_as<MyMotorCmd>(); it != pkt.end_as<MyMotorCmd>(); ++it) {
    //         motorQueue.Push(*it);
    //     }
    //     motorQueue.NotifyReceived(bcnp::MessageQueue<MyMotorCmd>::Clock::now());
    // });

    // 4. Main Loop
    while (g_running) {
        // Poll network
        driver.PollOnce();

        auto now = std::chrono::steady_clock::now();
        
        // Check connection status
        if (dispatcher.IsConnected(now)) {
            std::cout << "[Server] Connected, Idle. Waiting for messages...\n";
            // Example: If you have a queue, you would update it:
            // motorQueue.Update(now);
            // if (auto cmd = motorQueue.ActiveMessage()) {
            //     std::cout << "[Server] Executing motor command...\n";
            // }
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
