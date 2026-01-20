/**
 * @file bcnp_jni.cpp
 * @brief JNI implementation for BCNP Java bindings.
 * 
 * Bridges the Java BcnpJNI class to the C++ bcnp_core library.
 * Uses DirectByteBuffers for zero-copy data sharing.
 */

#include <jni.h>
#include <cstdint>
#include <cstring>

#include "bcnp/packet.h"

// JNI function naming: Java_com_bcnp_BcnpJNI_<methodName>

extern "C" {

/**
 * Helper to get direct buffer address and validate.
 */
static uint8_t* GetBufferAddress(JNIEnv* env, jobject buffer, jint offset) {
    if (!buffer) return nullptr;
    void* addr = env->GetDirectBufferAddress(buffer);
    if (!addr) return nullptr;
    return static_cast<uint8_t*>(addr) + offset;
}

/**
 * Decode a packet from a DirectByteBuffer.
 */
JNIEXPORT jboolean JNICALL Java_com_bcnp_BcnpJNI_decodePacket(
    JNIEnv* env, jclass cls,
    jobject buffer, jint offset, jint length,
    jobject result, jobject payloadSlice)
{
    uint8_t* data = GetBufferAddress(env, buffer, offset);
    if (!data || length <= 0) {
        return JNI_FALSE;
    }
    
    // Decode using C++ library
    bcnp::DecodeViewResult decodeResult = bcnp::DecodePacketView(data, static_cast<std::size_t>(length));
    
    // Get result class and methods
    jclass resultClass = env->GetObjectClass(result);
    jmethodID setOkMethod = env->GetMethodID(resultClass, "setOk", "(III)V");
    jmethodID setErrorMethod = env->GetMethodID(resultClass, "setError", "(II)V");
    
    if (decodeResult.error != bcnp::PacketError::None || decodeResult.view.is_none()) {
        // Set error in result object
        int errorCode = static_cast<int>(decodeResult.error);
        env->CallVoidMethod(result, setErrorMethod, errorCode, static_cast<jint>(decodeResult.bytesConsumed));
        return JNI_FALSE;
    }
    
    // Success - populate result and payload slice
    const auto& view = decodeResult.view.unwrap();
    env->CallVoidMethod(result, setOkMethod,
        static_cast<jint>(decodeResult.bytesConsumed),
        static_cast<jint>(view.header.messageType),
        static_cast<jint>(view.header.messageCount));
    
    // Populate payload slice
    jclass sliceClass = env->GetObjectClass(payloadSlice);
    jmethodID wrapMethod = env->GetMethodID(sliceClass, "wrap", "(Ljava/nio/ByteBuffer;II)V");
    
    // Calculate payload offset within the original buffer
    jint payloadOffset = offset + bcnp::kHeaderSizeV3;
    jint payloadLength = static_cast<jint>(view.payload.size());
    
    env->CallVoidMethod(payloadSlice, wrapMethod, buffer, payloadOffset, payloadLength);
    
    return JNI_TRUE;
}

/**
 * Encode a packet into a DirectByteBuffer.
 */
JNIEXPORT jint JNICALL Java_com_bcnp_BcnpJNI_encodePacket(
    JNIEnv* env, jclass cls,
    jobject buffer, jint offset, jint maxLength,
    jint messageType, jint flags,
    jobject payload, jint payloadOffset, jint payloadLength,
    jint messageCount)
{
    uint8_t* dest = GetBufferAddress(env, buffer, offset);
    if (!dest || maxLength <= 0) {
        return -1;
    }
    
    const uint8_t* payloadData = GetBufferAddress(env, payload, payloadOffset);
    if (!payloadData && payloadLength > 0) {
        return -1;
    }
    
    // Calculate expected packet size
    std::size_t headerSize = bcnp::kHeaderSizeV3;
    std::size_t crcSize = bcnp::kCrcSize;
    std::size_t totalSize = headerSize + static_cast<std::size_t>(payloadLength) + crcSize;
    
    if (totalSize > static_cast<std::size_t>(maxLength)) {
        return -2; // Buffer too small
    }
    
    // Build header
    bcnp::PacketHeader header{};
    header.messageType = static_cast<bcnp::MessageTypeId>(messageType);
    header.messageCount = static_cast<uint16_t>(messageCount);
    header.flags = static_cast<uint8_t>(flags);
    
    // Write header (V3 format)
    dest[0] = bcnp::kProtocolMajor;
    dest[1] = bcnp::kProtocolMinor;
    dest[2] = header.flags;
    dest[3] = static_cast<uint8_t>((header.messageType >> 8) & 0xFF);
    dest[4] = static_cast<uint8_t>(header.messageType & 0xFF);
    dest[5] = static_cast<uint8_t>((header.messageCount >> 8) & 0xFF);
    dest[6] = static_cast<uint8_t>(header.messageCount & 0xFF);
    
    // Copy payload
    if (payloadLength > 0) {
        std::memcpy(dest + headerSize, payloadData, static_cast<std::size_t>(payloadLength));
    }
    
    // Compute and write CRC
    std::size_t dataLen = headerSize + static_cast<std::size_t>(payloadLength);
    uint32_t crc = bcnp::ComputeCrc32(dest, dataLen);
    dest[dataLen + 0] = static_cast<uint8_t>(crc & 0xFF);
    dest[dataLen + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    dest[dataLen + 2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    dest[dataLen + 3] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    
    return static_cast<jint>(totalSize);
}

/**
 * Compute CRC32 checksum.
 */
JNIEXPORT jint JNICALL Java_com_bcnp_BcnpJNI_computeCrc32(
    JNIEnv* env, jclass cls,
    jobject buffer, jint offset, jint length)
{
    uint8_t* data = GetBufferAddress(env, buffer, offset);
    if (!data || length <= 0) {
        return 0;
    }
    return static_cast<jint>(bcnp::ComputeCrc32(data, static_cast<std::size_t>(length)));
}

/**
 * Get wire size for a message type.
 */
JNIEXPORT jint JNICALL Java_com_bcnp_BcnpJNI_getMessageWireSize(
    JNIEnv* env, jclass cls, jint messageTypeId)
{
    auto info = bcnp::GetMessageInfo(static_cast<bcnp::MessageTypeId>(messageTypeId));
    return info ? static_cast<jint>(info->wireSize) : 0;
}

// StreamParser handle storage (simplified, in production use proper handle management)
#include "bcnp/stream_parser.h"
#include <vector>

struct JavaStreamParser {
    std::vector<bcnp::PacketView> pendingPackets;
    bcnp::StreamParser* parser;
    
    JavaStreamParser(int capacity) {
        parser = new bcnp::StreamParser(
            [this](const bcnp::PacketView& pkt) {
                pendingPackets.push_back(pkt);
            },
            [](const bcnp::StreamParser::ErrorInfo&) {},
            static_cast<std::size_t>(capacity));
    }
    
    ~JavaStreamParser() {
        delete parser;
    }
};

JNIEXPORT jlong JNICALL Java_com_bcnp_BcnpJNI_createStreamParser(
    JNIEnv* env, jclass cls, jint bufferCapacity)
{
    auto* jsp = new JavaStreamParser(bufferCapacity);
    return reinterpret_cast<jlong>(jsp);
}

JNIEXPORT void JNICALL Java_com_bcnp_BcnpJNI_destroyStreamParser(
    JNIEnv* env, jclass cls, jlong handle)
{
    if (handle) {
        delete reinterpret_cast<JavaStreamParser*>(handle);
    }
}

JNIEXPORT void JNICALL Java_com_bcnp_BcnpJNI_streamParserPush(
    JNIEnv* env, jclass cls,
    jlong handle, jobject buffer, jint offset, jint length)
{
    if (!handle) return;
    uint8_t* data = GetBufferAddress(env, buffer, offset);
    if (!data || length <= 0) return;
    
    auto* jsp = reinterpret_cast<JavaStreamParser*>(handle);
    jsp->parser->Push(data, static_cast<std::size_t>(length));
}

JNIEXPORT jboolean JNICALL Java_com_bcnp_BcnpJNI_streamParserPop(
    JNIEnv* env, jclass cls,
    jlong handle, jobject result, jobject payloadSlice)
{
    if (!handle) return JNI_FALSE;
    auto* jsp = reinterpret_cast<JavaStreamParser*>(handle);
    
    if (jsp->pendingPackets.empty()) {
        return JNI_FALSE;
    }
    
    const auto& view = jsp->pendingPackets.front();
    
    // Populate result
    jclass resultClass = env->GetObjectClass(result);
    jmethodID setOkMethod = env->GetMethodID(resultClass, "setOk", "(III)V");
    
    // Note: bytesConsumed is not meaningful here, set to payload size
    env->CallVoidMethod(result, setOkMethod,
        static_cast<jint>(view.payload.size()),
        static_cast<jint>(view.header.messageType),
        static_cast<jint>(view.header.messageCount));
    
    // Note: payload slice cannot point to internal ring buffer (it's not a DirectByteBuffer)
    // For proper implementation, we would need to copy to a DirectByteBuffer
    // This is a limitation of the current design, may need rework
    
    jsp->pendingPackets.erase(jsp->pendingPackets.begin());
    return JNI_TRUE;
}

} // extern "C"
