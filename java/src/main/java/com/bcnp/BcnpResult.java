package com.bcnp;

/**
 * GC-free result container for BCNP operations.
 * 
 * Instead of allocating Result objects on the heap, this class can be
 * reused across calls. Similar to crab::Result but optimized for Java's
 * real-time constraints.
 * 
 * Usage:
 * <pre>
 *   BcnpResult result = new BcnpResult();
 *   parser.decode(buffer, result);
 *   if (result.isOk()) {
 *       // Use result.getBytesConsumed(), etc.
 *   } else {
 *       // Handle result.getError()
 *   }
 * </pre>
 */
public final class BcnpResult {
    
    /** Error codes matching C++ PacketError enum */
    public static final int OK = 0;
    public static final int INCOMPLETE = 1;
    public static final int UNSUPPORTED_VERSION = 2;
    public static final int CHECKSUM_MISMATCH = 3;
    public static final int UNKNOWN_MESSAGE_TYPE = 4;
    public static final int PAYLOAD_TOO_LARGE = 5;
    public static final int BUFFER_TOO_SMALL = 6;
    
    private int errorCode;
    private int bytesConsumed;
    private int messageCount;
    private int messageType;
    
    public BcnpResult() {
        reset();
    }
    
    /**
     * Reset to initial state (OK, 0 bytes consumed).
     */
    public void reset() {
        this.errorCode = OK;
        this.bytesConsumed = 0;
        this.messageCount = 0;
        this.messageType = 0;
    }
    
    /**
     * Set success state with metadata.
     */
    public void setOk(int bytesConsumed, int messageType, int messageCount) {
        this.errorCode = OK;
        this.bytesConsumed = bytesConsumed;
        this.messageType = messageType;
        this.messageCount = messageCount;
    }
    
    /**
     * Set error state.
     */
    public void setError(int errorCode, int bytesConsumed) {
        this.errorCode = errorCode;
        this.bytesConsumed = bytesConsumed;
        this.messageCount = 0;
        this.messageType = 0;
    }
    
    /**
     * @return true if operation succeeded
     */
    public boolean isOk() {
        return errorCode == OK;
    }
    
    /**
     * @return true if operation failed
     */
    public boolean isError() {
        return errorCode != OK;
    }
    
    /**
     * @return The error code (OK if success)
     */
    public int getErrorCode() {
        return errorCode;
    }
    
    /**
     * @return Number of bytes consumed from the input buffer
     */
    public int getBytesConsumed() {
        return bytesConsumed;
    }
    
    /**
     * @return Message type ID (valid only if isOk())
     */
    public int getMessageType() {
        return messageType;
    }
    
    /**
     * @return Number of messages in packet (valid only if isOk())
     */
    public int getMessageCount() {
        return messageCount;
    }
    
    /**
     * @return Human-readable error description
     */
    public String getErrorString() {
        switch (errorCode) {
            case OK: return "OK";
            case INCOMPLETE: return "Incomplete packet";
            case UNSUPPORTED_VERSION: return "Unsupported protocol version";
            case CHECKSUM_MISMATCH: return "Checksum mismatch";
            case UNKNOWN_MESSAGE_TYPE: return "Unknown message type";
            case PAYLOAD_TOO_LARGE: return "Payload too large";
            case BUFFER_TOO_SMALL: return "Buffer too small";
            default: return "Unknown error (" + errorCode + ")";
        }
    }
}
