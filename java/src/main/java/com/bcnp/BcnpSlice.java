package com.bcnp;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Zero-allocation slice view over a ByteBuffer.
 * 
 * Similar to crab::Slice in the C++ library, this provides bounds-checked
 * access to a region of a ByteBuffer without allocating memory.
 * 
 * Usage:
 * 
 * <pre>
 * BcnpSlice slice = new BcnpSlice();
 * slice.wrap(buffer, 10, 100); // View bytes [10, 110)
 * byte b = slice.get(0); // Access first byte of slice
 * </pre>
 */
public final class BcnpSlice {
    private ByteBuffer buffer;
    private int offset;
    private int length;

    /**
     * Create an empty slice. Call wrap() to initialize.
     */
    public BcnpSlice() {
        this.buffer = null;
        this.offset = 0;
        this.length = 0;
    }

    /**
     * Wrap a region of a ByteBuffer.
     * 
     * @param buffer The source buffer (must be direct for JNI compatibility)
     * @param offset Starting offset within the buffer
     * @param length Number of bytes in this slice
     * @throws IndexOutOfBoundsException if offset + length exceeds buffer capacity
     */
    public void wrap(ByteBuffer buffer, int offset, int length) {
        if (offset < 0 || length < 0 || offset + length > buffer.capacity()) {
            throw new IndexOutOfBoundsException(
                    "Slice bounds [" + offset + ", " + (offset + length) +
                            ") exceed buffer capacity " + buffer.capacity());
        }
        this.buffer = buffer;
        this.offset = offset;
        this.length = length;
    }

    /**
     * Wrap the entire remaining portion of a ByteBuffer.
     */
    public void wrap(ByteBuffer buffer) {
        wrap(buffer, buffer.position(), buffer.remaining());
    }

    /**
     * Reset to empty state.
     */
    public void reset() {
        this.buffer = null;
        this.offset = 0;
        this.length = 0;
    }

    /**
     * @return true if this slice has been initialized with a buffer
     */
    public boolean isValid() {
        return buffer != null;
    }

    /**
     * @return Number of bytes in this slice
     */
    public int length() {
        return length;
    }

    /**
     * @return true if length is 0
     */
    public boolean isEmpty() {
        return length == 0;
    }

    /**
     * Get a byte at the given index within the slice.
     * 
     * @param index Index relative to slice start (0-based)
     * @return The byte value
     * @throws IndexOutOfBoundsException if index is out of range
     */
    public byte get(int index) {
        if (index < 0 || index >= length) {
            throw new IndexOutOfBoundsException("Index " + index + " out of range [0, " + length + ")");
        }
        return buffer.get(offset + index);
    }

    /**
     * Put a byte at the given index within the slice.
     */
    public void put(int index, byte value) {
        if (index < 0 || index >= length) {
            throw new IndexOutOfBoundsException("Index " + index + " out of range [0, " + length + ")");
        }
        buffer.put(offset + index, value);
    }

    /**
     * Get a 32-bit float at the given byte offset (little-endian).
     */
    public float getFloat(int byteOffset) {
        if (byteOffset < 0 || byteOffset + 4 > length) {
            throw new IndexOutOfBoundsException();
        }
        return buffer.order(ByteOrder.LITTLE_ENDIAN).getFloat(offset + byteOffset);
    }

    /**
     * Get a 32-bit signed integer at the given byte offset (little-endian).
     */
    public int getInt(int byteOffset) {
        if (byteOffset < 0 || byteOffset + 4 > length) {
            throw new IndexOutOfBoundsException();
        }
        return buffer.order(ByteOrder.LITTLE_ENDIAN).getInt(offset + byteOffset);
    }

    /**
     * Get a 16-bit unsigned integer at the given byte offset (little-endian).
     */
    public int getUInt16(int byteOffset) {
        if (byteOffset < 0 || byteOffset + 2 > length) {
            throw new IndexOutOfBoundsException();
        }
        return buffer.order(ByteOrder.LITTLE_ENDIAN).getShort(offset + byteOffset) & 0xFFFF;
    }

    /**
     * Get an 8-bit unsigned integer at the given byte offset.
     */
    public int getUInt8(int byteOffset) {
        return get(byteOffset) & 0xFF;
    }

    /**
     * Create a sub-slice (zero-allocation, reuses this object).
     * 
     * @param start Start offset within this slice
     * @param len   Length of the sub-slice
     */
    public void subslice(int start, int len) {
        if (start < 0 || len < 0 || start + len > length) {
            throw new IndexOutOfBoundsException();
        }
        this.offset = this.offset + start;
        this.length = len;
    }

    /**
     * @return The underlying ByteBuffer (for JNI)
     */
    public ByteBuffer buffer() {
        return buffer;
    }

    /**
     * @return The absolute offset within the buffer
     */
    public int offset() {
        return offset;
    }

    /**
     * Copy bytes from this slice to a destination array.
     */
    public void copyTo(byte[] dest, int destOffset, int srcOffset, int len) {
        if (srcOffset < 0 || len < 0 || srcOffset + len > length) {
            throw new IndexOutOfBoundsException();
        }
        for (int i = 0; i < len; i++) {
            dest[destOffset + i] = buffer.get(offset + srcOffset + i);
        }
    }
}
