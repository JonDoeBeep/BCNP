package com.bcnp;

import java.nio.ByteBuffer;

/**
 * Java wrapper around the native BCNP StreamParser.
 * 
 * Provides incremental packet parsing with zero-copy buffer handling.
 * Allocates the native StreamParser on construction and must be
 * explicitly closed to free native resources.
 */
public final class StreamParser implements AutoCloseable {
    
    private long nativeHandle;
    private final BcnpResult result;
    private final BcnpSlice payloadSlice;
    
    /**
     * Callback interface for received packets.
     */
    @FunctionalInterface
    public interface PacketHandler {
        void onPacket(BcnpResult result, BcnpSlice payload);
    }
    
    /**
     * Create a new StreamParser with default buffer capacity (4096 bytes).
     */
    public StreamParser() {
        this(4096);
    }
    
    /**
     * Create a new StreamParser with specified buffer capacity.
     */
    public StreamParser(int bufferCapacity) {
        this.nativeHandle = BcnpJNI.createStreamParser(bufferCapacity);
        if (this.nativeHandle == 0) {
            throw new RuntimeException("Failed to create native StreamParser");
        }
        this.result = new BcnpResult();
        this.payloadSlice = new BcnpSlice();
    }
    
    /**
     * Push bytes into the parser.
     * 
     * @param buffer Direct ByteBuffer containing data
     * @param offset Offset within the buffer
     * @param length Number of bytes to push
     */
    public void push(ByteBuffer buffer, int offset, int length) {
        if (nativeHandle == 0) {
            throw new IllegalStateException("StreamParser has been closed");
        }
        BcnpJNI.streamParserPush(nativeHandle, buffer, offset, length);
    }
    
    /**
     * Push all remaining bytes from a ByteBuffer.
     */
    public void push(ByteBuffer buffer) {
        push(buffer, buffer.position(), buffer.remaining());
    }
    
    /**
     * Pop the next available packet.
     * 
     * @param result Reusable result container (will be populated)
     * @param payloadSlice Reusable slice for payload (will be populated)
     * @return true if a packet was available
     */
    public boolean pop(BcnpResult result, BcnpSlice payloadSlice) {
        if (nativeHandle == 0) {
            return false;
        }
        return BcnpJNI.streamParserPop(nativeHandle, result, payloadSlice);
    }
    
    /**
     * Pop all available packets and call handler for each.
     */
    public void popAll(PacketHandler handler) {
        while (pop(result, payloadSlice)) {
            handler.onPacket(result, payloadSlice);
        }
    }
    
    /**
     * Release native resources.
     */
    @Override
    public void close() {
        if (nativeHandle != 0) {
            BcnpJNI.destroyStreamParser(nativeHandle);
            nativeHandle = 0;
        }
    }
    
    /**
     * @return true if this parser is still valid (not closed)
     */
    public boolean isValid() {
        return nativeHandle != 0;
    }
}
