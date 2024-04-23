#include <cstring>
#include <cassert>

#include <pb_encode.h>
#include <pb_decode.h>

#include <spark_wiring_error.h>

#include "protobuf.h"

namespace particle::util {

int encodeProtobuf(Buffer& buf, const void* msg, const pb_msgdesc_t* desc) {
    pb_ostream_t strm = {};
    strm.state = &buf;
    strm.max_size = SIZE_MAX;
    strm.callback = [](pb_ostream_t* strm, const uint8_t* data, size_t size) {
        auto buf = static_cast<Buffer*>(strm->state);
        if (buf->resize(buf->size() + size) < 0) {
            return false;
        }
        std::memcpy(buf->data() + buf->size() - size, data, size);
        return true;
    };
    if (!pb_encode(&strm, desc, msg)) {
        return Error::ENCODING_FAILED;
    }
    return strm.bytes_written;
}

int decodeProtobuf(const Buffer& buf, void* msg, const pb_msgdesc_t* desc) {
    auto strm = pb_istream_from_buffer((const pb_byte_t*)buf.data(), buf.size());
    if (!pb_decode(&strm, desc, msg)) {
        return Error::BAD_DATA;
    }
    assert(strm.bytes_left <= buf.size());
    return buf.size() - strm.bytes_left;
}

} // namespace particle::util
