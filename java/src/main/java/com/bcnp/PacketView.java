package com.bcnp;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Zero-allocation view into a decoded BCNP packet.
 * 
 * Similar to C++ bcnp::PacketView, this is a flyweight that wraps
 * the underlying buffer without copying data.
 */
public final class PacketView {
    private ByteBuffer buffer;
    private int headerOffset;
    private int payloadOffset;
    private int payloadLength;
    
    // Parsed header values (cached to avoid repeated buffer reads)
    private int messageType;
    private int messageCount;
    private int flags;
    
    public PacketView() {
        reset();
    }
    
    /**
     * Initialize this view with decoded packet data.
     */
    public void wrap(ByteBuffer buffer, int headerOffset, int payloadOffset, int payloadLength,
                     int messageType, int messageCount, int flags) {
        this.buffer = buffer;
        this.headerOffset = headerOffset;
        this.payloadOffset = payloadOffset;
        this.payloadLength = payloadLength;
        this.messageType = messageType;
        this.messageCount = messageCount;
        this.flags = flags;
    }
    
    /**
     * Reset to empty state.
     */
    public void reset() {
        this.buffer = null;
        this.headerOffset = 0;
        this.payloadOffset = 0;
        this.payloadLength = 0;
        this.messageType = 0;
        this.messageCount = 0;
        this.flags = 0;
    }
    
    public boolean isValid() { return buffer != null; }
    public int getMessageType() { return messageType; }
    public int getMessageCount() { return messageCount; }
    public int getFlags() { return flags; }
    public int getPayloadLength() { return payloadLength; }
    
    /**
     * Get the payload as a slice (zero-allocation).
     */
    public void getPayload(BcnpSlice slice) {
        slice.wrap(buffer, payloadOffset, payloadLength);
    }
    
    /**
     * Get a message at the given index.
     * 
     * @param index Message index (0-based)
     * @param wireSize Wire size of each message
     * @param slice Reusable slice to populate
     */
    public void getMessage(int index, int wireSize, BcnpSlice slice) {
        if (index < 0 || index >= messageCount) {
            throw new IndexOutOfBoundsException("Message index " + index + " out of range [0, " + messageCount + ")");
        }
        int msgOffset = payloadOffset + (index * wireSize);
        slice.wrap(buffer, msgOffset, wireSize);
    }
}
