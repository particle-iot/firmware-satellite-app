#include <algorithm>
#include <cstring>

#include <pb_encode.h>
#include <pb_decode.h>

#include <spark_wiring_stream.h>
#include <spark_wiring_logging.h>
#include <spark_wiring_error.h>

#include <check.h>

#include <cloud/cloud_new.pb.h>

#include "util/protobuf.h"
#include "cloud_protocol.h"

#include "diag_query/diag_query.h"

#include <vector>
#include <map>

#define PB_CLOUD(_name) particle_cloud_##_name

namespace particle::constrained {

namespace {

enum RequestType {
    HELLO = 1,
    EVENT = 2,
    DIAGNOSTICS = 3
};

class InputBufferStream: public Stream {
public:
    explicit InputBufferStream(util::Buffer& buf) :
            buf_(buf),
            offs_(0) {
    }

    int read() override {
        uint8_t b;
        size_t n = readBytes((char*)&b, 1);
        if (n != 1) {
            return -1;
        }
        return b;
    }

    size_t readBytes(char* data, size_t size) override {
        auto n = std::min(size, buf_.size() - offs_);
        std::memcpy(data, buf_.data() + offs_, n);
        offs_ += n;
        return n;
    }

    int available() override {
        return buf_.size() - offs_;
    }

    int peek() override {
        if (offs_ >= buf_.size()) {
            return -1;
        }
        return (uint8_t)buf_.data()[offs_];
    }

    size_t write(uint8_t b) override {
        setWriteError(Error::NOT_SUPPORTED);
        return 0;
    }

    void flush() override {
        setWriteError(Error::NOT_SUPPORTED);
    }

private:
    util::Buffer& buf_;
    size_t offs_;
};

} // namespace

int CloudProtocol::init(CloudProtocolConfig conf) {
    if (state_ != State::NEW) {
        return 0;
    }
    MessageChannelConfig chanConf;
    chanConf.onSend(conf.onSend_);
    chanConf.onRequest([this](auto type, auto data, auto onResp) {
        return receiveRequest(type, std::move(data), std::move(onResp));
    });
    CHECK(channel_.init(std::move(chanConf)));
    state_ = State::DISCONNECTED;
    return 0;
}

int CloudProtocol::connect() {
    if (state_ == State::CONNECTED) {
        return 0;
    }
    if (state_ != State::DISCONNECTED) {
        return Error::INVALID_STATE;
    }
    state_ = State::CONNECTED;
    // TODO: Send a subscription request
    return 0;
}

void CloudProtocol::disconnect() {
    if (state_ == State::NEW || state_ == State::DISCONNECTED) {
        return;
    }
    state_ = State::DISCONNECTED;
    channel_.reset();
}

int CloudProtocol::receive(util::Buffer data, int port) {
    CHECK(channel_.receive(std::move(data), port));
    return 0;
}

int CloudProtocol::run() {
    CHECK(channel_.run());
    return 0;
}

int CloudProtocol::subscribe(int code, OnEvent onEvent) {
    if (!subscrs_.set(code, std::move(onEvent))) {
        return Error::NO_MEMORY;
    }
    // TODO: Send a subscription request
    return 0;
}

int CloudProtocol::publishImpl(int code, std::optional<Variant> data) {
    String eventData;
    PB_CLOUD(EventRequest) reqMsg = {};
    reqMsg.which_type = PB_CLOUD(EventRequest_code_tag);
    reqMsg.type.code = code;
    if (data.has_value()) {
        OutputStringStream s(eventData);
        CHECK(encodeToCBOR(data.value(), s));
        reqMsg.data.arg = &eventData;
        reqMsg.data.funcs.encode = [](auto strm, auto field, auto arg) {
            auto eventData = (const String*)*arg;
            return pb_encode_tag_for_field(strm, field) &&
                    pb_encode_string(strm, (const uint8_t*)eventData->c_str(), eventData->length());
        };
    }
    util::Buffer reqData;
    CHECK(util::encodeProtobuf(reqData, &reqMsg, &PB_CLOUD(EventRequest_msg)));
    Log.trace("Sending Event request");
    CHECK(channel_.sendRequest(RequestType::EVENT, std::move(reqData), [](auto err, auto result, auto /* data */) {
        if (err < 0) {
            Log.error("Failed to send Event request: %d", err);
        } else {
            Log.trace("Received Event response");
            if (result != 0) {
                Log.error("Event request failed: %d", result);
            }
        }
        return 0;
    }));
    return 0;
}

int CloudProtocol::receiveRequest(unsigned type, util::Buffer data, MessageChannel::OnResponse onResp) {
    switch (type) {
    case RequestType::EVENT: {
        CHECK(receiveEventRequest(std::move(data), std::move(onResp)));
        break;
    }
    case RequestType::DIAGNOSTICS: {
        CHECK(receiveDiagnosticsRequest(std::move(data), std::move(onResp)));
        break;
    }
    default:
        Log.error("Received unsupported request, type: %u", type);
    }
    return 0;
}

int CloudProtocol::receiveEventRequest(util::Buffer data, MessageChannel::OnResponse onResp) {
    // Parse the request
    PB_CLOUD(EventRequest) reqMsg = {};
    util::Buffer buf;
    reqMsg.data.arg = &buf;
    reqMsg.data.funcs.decode = [](auto strm, auto field, auto arg) {
        auto buf = (util::Buffer*)*arg;
        if (buf->resize(strm->bytes_left) < 0) {
            return false;
        }
        return pb_read(strm, (pb_byte_t*)buf->data(), strm->bytes_left);
    };
    CHECK(decodeProtobuf(data, &reqMsg, &PB_CLOUD(EventRequest_msg)));
    if (reqMsg.which_type != PB_CLOUD(EventRequest_code_tag)) {
        Log.error("Unsupported event");
        return Error::NOT_SUPPORTED;
    }
    auto code = reqMsg.type.code;
    Variant v;
    InputBufferStream strm(buf);
    CHECK(decodeFromCBOR(v, strm));
    Log.trace("Received event, code: %d", (int)code);
    if (buf.size() > 0) {
        Log.print(LOG_LEVEL_TRACE, v.toJSON().c_str());
        Log.print(LOG_LEVEL_TRACE, "\r\n");
    }
    // Send a response
    onResp(0 /* error */, 0 /* result */, util::Buffer());
    // Invoke the subscription handler
    auto it = subscrs_.find(code);
    if (it == subscrs_.end()) {
        Log.warn("Missing subscription handler");
        return 0;
    }
    it->second(code, std::move(v));
    return 0;
}


struct EncodedUint8Bytes {
    const uint8_t* data;
    size_t size;

    explicit EncodedUint8Bytes(pb_callback_t* cb, const uint8_t* data = nullptr, size_t size = 0) :
            data(data),
            size(size) {
        cb->arg = this;
        cb->funcs.encode = [](pb_ostream_t* strm, const pb_field_iter_t* field, void* const* arg) {
            const auto str = (const EncodedUint8Bytes*)*arg;
            if (str->data && str->size > 0 && (!pb_encode_tag_for_field(strm, field) ||
                    !pb_encode_string(strm, (const uint8_t*)str->data, str->size))) {
                return false;
            }
            return true;
        };
    }
};

bool encodeDiagMap(pb_ostream_t *stream, const pb_field_t *field, void* const* arg) {
    auto map = (const std::map<uint32_t, std::vector<uint8_t>>*)*arg;
    for (const auto& val : *map) {

        const uint32_t id = val.first;
        // Log.printf("Logging values[%d]: ", id);

        // uint8_t bytes[4] = {0};
        // memcpy(bytes, val.second.data(), sizeof(bytes));
        // for (int i=0; i<val.second.size(); i++) {
        //     Log.printf("%02X", bytes[i]);
        // }
        // Log.print("\r\n");
        
        PB_CLOUD(DiagnosticsResponse_Source) s = PB_CLOUD(DiagnosticsResponse_Source_init_default);
        s.id = id;
        EncodedUint8Bytes myDataBytes(&s.data, (uint8_t*)val.second.data(), val.second.size());
        if (!pb_encode_tag_for_field(stream, field)) {
            Log.print(LOG_LEVEL_TRACE, "Tag encoding failed\r\n");
            return false;
        }

        if (!pb_encode_submessage(stream, particle_cloud_DiagnosticsResponse_Source_fields, &s)) {
            Log.print(LOG_LEVEL_TRACE, "Encoding failed\r\n");
            return false;
        }
    }
    return true;
}

bool readIds(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    auto values = (std::vector<uint32_t>*)*arg;
    uint64_t value = 0;
    if (!pb_decode_varint(stream, &value)) {
        return false;
    }
    // Log.printf("Value decoded: %d\r\n", value);
    values->push_back(value);
    return true;
}

int CloudProtocol::receiveDiagnosticsRequest(util::Buffer data, MessageChannel::OnResponse onResp) {
    // TODO: Refactor this function to match the style of this file
    // TODO: Use spark::Vector

    std::vector<uint32_t> diagIds;
    std::map<uint32_t, std::vector<uint8_t>> diagValueMap;

    {
        // Decode the incoming pb request which has the list of diag IDs to query

        pb_istream_t stream = pb_istream_from_buffer((pb_byte_t*)data.data(), data.size());        
        PB_CLOUD(DiagnosticsRequest) request = PB_CLOUD(DiagnosticsRequest_init_zero);

        request.has_categories = false;
        request.ids.arg = (void*)&diagIds;
        request.ids.funcs.decode = &readIds;

        bool decoded = pb_decode(&stream, PB_CLOUD(DiagnosticsRequest_fields), &request);

        if (!decoded) {
            Log.print(LOG_LEVEL_TRACE, "Decoding failed\r\n");
            if (onResp) {
                onResp(!decoded /* error */, 0 /* result */, util::Buffer());
            }
            return Error::ENCODING_FAILED;
        }
    }

    {
        // Get values for the diagnostics IDs
        // Place them in a map

        for (const auto& diagId : diagIds) {
            Log.printf(LOG_LEVEL_TRACE, "Querying diag id: %lu\r\n", diagId);
            std::vector<uint8_t> res;
            if (!getDiagnosticValue(diagId, &res)) {
                diagValueMap[diagId] = res;
            }

            // for (const auto& pair: diagValueMap) {
            //     Log.printf("ID: %d --- ", pair.first);
            //     Log.print("Data: ");
            //     for (int i=0; i<pair.second.size(); i++) {
            //         Log.printf("%02X", pair.second[i]);
            //     }
            //     Log.printf("\r\n");
            // }
        }
    }


    {
        // Encode the response and send it using onResp callback
        auto buffer = util::Buffer(256);

        PB_CLOUD(DiagnosticsResponse) response = PB_CLOUD(DiagnosticsResponse_init_zero);
        pb_ostream_t ostream = pb_ostream_from_buffer((pb_byte_t*) buffer.data(), buffer.size());
        
        response.sources.arg = (void*)&diagValueMap;
        response.sources.funcs.encode = &encodeDiagMap;

        int encodePassed = pb_encode(&ostream, PB_CLOUD(DiagnosticsResponse_fields), &response);
        if (!encodePassed) {
            Log.printf(LOG_LEVEL_TRACE, "Encoding failed: %d \r\n", !encodePassed);
            onResp(1 /* error */, 0 /* result */, util::Buffer());
            return Error::ENCODING_FAILED;
        }

        buffer.resize(ostream.bytes_written);

        Log.print(LOG_LEVEL_TRACE, "Encoded Bytes\r\n");
        for (size_t i = 0; i < buffer.size(); ++i) {
            Log.printf(LOG_LEVEL_TRACE, "%02X", buffer.data()[i]);
        }

        Log.print(LOG_LEVEL_TRACE, "\r\n");

        if (onResp) {
            onResp(0 /* error */, 0 /* result */, std::move(buffer));
        }
    }

    return 0;
}

} // namespace particle::constrained
