package com.bcnp;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.DisplayName;
import static org.junit.jupiter.api.Assertions.*;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Unit tests for BCNP Java core classes.
 * 
 * Note: JNI-dependent tests are skipped if native library isn't loaded.
 */
class BcnpCoreTest {
    
    // ========================================================================
    // BcnpSlice Tests
    // ========================================================================
    
    @Test
    @DisplayName("BcnpSlice: Basic wrap and access")
    void testSliceBasicAccess() {
        ByteBuffer buffer = ByteBuffer.allocateDirect(100);
        buffer.order(ByteOrder.LITTLE_ENDIAN);
        
        // Write test data
        buffer.put(10, (byte) 0x42);
        buffer.putInt(20, 12345);
        buffer.putFloat(30, 1.5f);
        
        BcnpSlice slice = new BcnpSlice();
        assertFalse(slice.isValid());
        
        slice.wrap(buffer, 0, 100);
        assertTrue(slice.isValid());
        assertEquals(100, slice.length());
        
        assertEquals(0x42, slice.get(10));
    }
    
    @Test
    @DisplayName("BcnpSlice: Bounds checking")
    void testSliceBoundsChecking() {
        ByteBuffer buffer = ByteBuffer.allocateDirect(50);
        BcnpSlice slice = new BcnpSlice();
        
        // Valid wrap
        slice.wrap(buffer, 10, 30);
        assertEquals(30, slice.length());
        
        // Access within bounds should work
        assertDoesNotThrow(() -> slice.get(0));
        assertDoesNotThrow(() -> slice.get(29));
        
        // Access out of bounds should throw
        assertThrows(IndexOutOfBoundsException.class, () -> slice.get(-1));
        assertThrows(IndexOutOfBoundsException.class, () -> slice.get(30));
        
        // Invalid wrap should throw
        assertThrows(IndexOutOfBoundsException.class, () -> slice.wrap(buffer, 40, 20)); // Exceeds capacity
    }
    
    @Test
    @DisplayName("BcnpSlice: Subslice (zero-alloc)")
    void testSliceSubslice() {
        ByteBuffer buffer = ByteBuffer.allocateDirect(100);
        buffer.put(50, (byte) 0xAB);
        
        BcnpSlice slice = new BcnpSlice();
        slice.wrap(buffer, 0, 100);
        
        // Take subslice from [40, 60)
        slice.subslice(40, 20);
        assertEquals(20, slice.length());
        
        // Original byte at buffer[50] is now at slice[10]
        assertEquals((byte) 0xAB, slice.get(10));
    }
    
    // ========================================================================
    // BcnpResult Tests
    // ========================================================================
    
    @Test
    @DisplayName("BcnpResult: Success state")
    void testResultSuccess() {
        BcnpResult result = new BcnpResult();
        assertTrue(result.isOk()); // Default is OK
        
        result.setOk(128, 1, 5);
        assertTrue(result.isOk());
        assertFalse(result.isError());
        assertEquals(128, result.getBytesConsumed());
        assertEquals(1, result.getMessageType());
        assertEquals(5, result.getMessageCount());
        assertEquals("OK", result.getErrorString());
    }
    
    @Test
    @DisplayName("BcnpResult: Error state")
    void testResultError() {
        BcnpResult result = new BcnpResult();
        
        result.setError(BcnpResult.CHECKSUM_MISMATCH, 7);
        assertTrue(result.isError());
        assertFalse(result.isOk());
        assertEquals(BcnpResult.CHECKSUM_MISMATCH, result.getErrorCode());
        assertEquals(7, result.getBytesConsumed());
        assertEquals("Checksum mismatch", result.getErrorString());
    }
    
    @Test
    @DisplayName("BcnpResult: Reset")
    void testResultReset() {
        BcnpResult result = new BcnpResult();
        result.setError(BcnpResult.UNSUPPORTED_VERSION, 3);
        assertTrue(result.isError());
        
        result.reset();
        assertTrue(result.isOk());
        assertEquals(0, result.getBytesConsumed());
    }
    
    // ========================================================================
    // PacketView Tests
    // ========================================================================
    
    @Test
    @DisplayName("PacketView: Basic wrap and access")
    void testPacketViewBasic() {
        ByteBuffer buffer = ByteBuffer.allocateDirect(200);
        
        PacketView view = new PacketView();
        assertFalse(view.isValid());
        
        view.wrap(buffer, 0, 7, 100, 1, 10, 0);
        assertTrue(view.isValid());
        assertEquals(1, view.getMessageType());
        assertEquals(10, view.getMessageCount());
        assertEquals(100, view.getPayloadLength());
    }
    
    @Test
    @DisplayName("PacketView: Get message slice")
    void testPacketViewGetMessage() {
        ByteBuffer buffer = ByteBuffer.allocateDirect(200);
        
        // Write some test data in payload area (starting at offset 7)
        buffer.order(ByteOrder.BIG_ENDIAN);
        buffer.putInt(7, 0x12345678); // First message
        buffer.putInt(11, 0xABCDEF01); // Second message
        
        PacketView view = new PacketView();
        view.wrap(buffer, 0, 7, 8, 1, 2, 0); // 2 messages, 4 bytes each
        
        BcnpSlice msgSlice = new BcnpSlice();
        view.getMessage(0, 4, msgSlice);
        assertEquals(4, msgSlice.length());
        assertEquals(0x12345678, msgSlice.getInt(0));
        
        view.getMessage(1, 4, msgSlice);
        assertEquals(0xABCDEF01 & 0xFFFFFFFFL, msgSlice.getInt(0) & 0xFFFFFFFFL);
    }
}
