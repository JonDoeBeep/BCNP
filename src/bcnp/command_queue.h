#pragma once

// Legacy header - use message_queue.h for new code

#include "bcnp/message_queue.h"
#include <bcnp/message_types.h>

namespace bcnp {

// Legacy type aliases
using QueueConfig = MessageQueueConfig;
using QueueMetrics = MessageQueueMetrics;
using Command = DriveCmd;

/// Legacy CommandQueue - use MessageQueue<DriveCmd> for new code
class CommandQueue : public MessageQueue<DriveCmd> {
public:
    explicit CommandQueue(MessageQueueConfig config = {})
        : MessageQueue<DriveCmd>(config) {}
    
    // Legacy API methods
    void NotifyPacketReceived(Clock::time_point now) { NotifyReceived(now); }
    std::optional<DriveCmd> ActiveCommand() const { return ActiveMessage(); }
    void IncrementParseErrors() {} // No-op, handled by dispatcher now
};

} // namespace bcnp
