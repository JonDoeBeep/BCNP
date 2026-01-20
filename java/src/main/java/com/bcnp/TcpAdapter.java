package com.bcnp;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;

/**
 * Non-blocking TCP adapter for BCNP using Java NIO.
 * 
 * Provides zero-copy buffer handling for RT-safe operation.
 * 
 * Usage:
 * <pre>
 *   TcpAdapter tcp = new TcpAdapter();
 *   tcp.connect("192.168.1.100", 5800);
 *   
 *   // In periodic loop:
 *   int received = tcp.receive(buffer);
 *   if (received > 0) {
 *       parser.push(buffer, 0, received);
 *   }
 *   
 *   tcp.send(encodedPacket);
 * </pre>
 */
public final class TcpAdapter implements AutoCloseable {
    
    private SocketChannel channel;
    private Selector selector;
    private boolean connected;
    
    // Pre-allocated for zero-alloc sends
    private final ByteBuffer sendBuffer;
    
    public TcpAdapter() {
        this(8192); // 8KB default buffer
    }
    
    public TcpAdapter(int sendBufferSize) {
        this.sendBuffer = ByteBuffer.allocateDirect(sendBufferSize);
        this.connected = false;
    }
    
    /**
     * Connect to a remote server.
     * 
     * @param host Remote host address
     * @param port Remote port
     * @return true if connection initiated (may not be complete yet)
     */
    public boolean connect(String host, int port) throws IOException {
        close(); // Close any existing connection
        
        channel = SocketChannel.open();
        channel.configureBlocking(false);
        channel.socket().setTcpNoDelay(true);
        
        selector = Selector.open();
        channel.register(selector, SelectionKey.OP_CONNECT | SelectionKey.OP_READ);
        
        connected = channel.connect(new InetSocketAddress(host, port));
        return true;
    }
    
    /**
     * Complete pending connection (call in loop until returns true).
     */
    public boolean finishConnect() throws IOException {
        if (connected) return true;
        if (channel == null) return false;
        
        if (channel.finishConnect()) {
            connected = true;
            channel.register(selector, SelectionKey.OP_READ);
            return true;
        }
        return false;
    }
    
    /**
     * Receive bytes into a DirectByteBuffer (zero-copy).
     * 
     * @param buffer Direct ByteBuffer to receive into
     * @return Number of bytes received, 0 if none available, -1 on disconnect
     */
    public int receive(ByteBuffer buffer) throws IOException {
        if (channel == null || !connected) return 0;
        
        buffer.clear();
        int read = channel.read(buffer);
        buffer.flip();
        
        return read;
    }
    
    /**
     * Send bytes from a ByteBuffer.
     * 
     * @param buffer Buffer containing data to send
     * @return Number of bytes sent
     */
    public int send(ByteBuffer buffer) throws IOException {
        if (channel == null || !connected) return 0;
        return channel.write(buffer);
    }
    
    /**
     * Send bytes from a slice.
     */
    public int send(BcnpSlice slice) throws IOException {
        if (channel == null || !connected) return 0;
        
        ByteBuffer buf = slice.buffer();
        int originalPos = buf.position();
        int originalLimit = buf.limit();
        
        buf.position(slice.offset());
        buf.limit(slice.offset() + slice.length());
        
        int written = channel.write(buf);
        
        buf.position(originalPos);
        buf.limit(originalLimit);
        
        return written;
    }
    
    /**
     * @return true if connected to remote host
     */
    public boolean isConnected() {
        return connected && channel != null && channel.isConnected();
    }
    
    /**
     * Close the connection and release resources.
     */
    @Override
    public void close() {
        connected = false;
        try {
            if (selector != null) {
                selector.close();
                selector = null;
            }
            if (channel != null) {
                channel.close();
                channel = null;
            }
        } catch (IOException e) {
            // Ignore close errors
        }
    }
    
    /**
     * @return The internal send buffer for zero-alloc encoding
     */
    public ByteBuffer getSendBuffer() {
        sendBuffer.clear();
        return sendBuffer;
    }
}
