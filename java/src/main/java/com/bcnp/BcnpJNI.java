package com.bcnp;

import java.nio.ByteBuffer;

/**
 * JNI bridge to the native BCNP C++ library.
 * 
 * All native methods operate on DirectByteBuffers to enable zero-copy
 * data sharing between Java and C++.
 */
public final class BcnpJNI {
    
    static {
        System.loadLibrary("bcnp_jni");
    }
    
    // ========================================================================
    // Protocol Constants (matching C++ values)
    // ========================================================================
    
    public static final int PROTOCOL_MAJOR = 3;
    public static final int PROTOCOL_MINOR = 2;
    public static final int HEADER_SIZE = 7;
    public static final int CRC_SIZE = 4;
    
    // ========================================================================
    // Native Methods
    // ========================================================================
    
    /**
     * Decode a packet from a DirectByteBuffer.
     * 
     * @param buffer Direct ByteBuffer containing raw packet bytes
     * @param offset Starting offset within the buffer
     * @param length Available bytes to read
     * @param result Reusable result container (populated by native code)
     * @param payloadSlice Reusable slice for payload access (populated if successful)
     * @return true if a complete packet was decoded
     */
    public static native boolean decodePacket(
        ByteBuffer buffer, int offset, int length,
        BcnpResult result, BcnpSlice payloadSlice);
    
    /**
     * Encode a packet into a DirectByteBuffer.
     * 
     * @param buffer Direct ByteBuffer to write encoded bytes
     * @param offset Starting offset within the buffer
     * @param maxLength Maximum bytes available for writing
     * @param messageType Message type ID
     * @param flags Packet flags
     * @param payload Slice containing message payloads
     * @param messageCount Number of messages
     * @return Number of bytes written, or negative value on error
     */
    public static native int encodePacket(
        ByteBuffer buffer, int offset, int maxLength,
        int messageType, int flags,
        ByteBuffer payload, int payloadOffset, int payloadLength,
        int messageCount);
    
    /**
     * Compute CRC32 checksum for a buffer region.
     * 
     * @param buffer Direct ByteBuffer
     * @param offset Starting offset
     * @param length Number of bytes to checksum
     * @return CRC32 value
     */
    public static native int computeCrc32(ByteBuffer buffer, int offset, int length);
    
    /**
     * Get the wire size for a message type ID.
     * 
     * @param messageTypeId The message type to query
     * @return Wire size in bytes, or 0 if unknown
     */
    public static native int getMessageWireSize(int messageTypeId);
    
    /**
     * Create a native StreamParser instance.
     * 
     * @param bufferCapacity Size of internal ring buffer
     * @return Native handle (pointer), or 0 on failure
     */
    public static native long createStreamParser(int bufferCapacity);
    
    /**
     * Destroy a native StreamParser instance.
     * 
     * @param handle Native handle from createStreamParser
     */
    public static native void destroyStreamParser(long handle);
    
    /**
     * Push bytes to a StreamParser.
     * 
     * @param handle Native StreamParser handle
     * @param buffer Direct ByteBuffer containing data
     * @param offset Starting offset
     * @param length Number of bytes to push
     */
    public static native void streamParserPush(
        long handle, ByteBuffer buffer, int offset, int length);
    
    /**
     * Pop a decoded packet from the StreamParser.
     * 
     * @param handle Native StreamParser handle
     * @param result Reusable result container
     * @param payloadSlice Reusable slice for payload
     * @return true if a packet was available
     */
    public static native boolean streamParserPop(
        long handle, BcnpResult result, BcnpSlice payloadSlice);
    
    // Private constructor - utility class
    private BcnpJNI() {}
}
