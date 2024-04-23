#include <utility>
#include <cstring>

#include <spark_wiring_logging.h>
#include <spark_wiring_error.h>

#include <scope_guard.h>
#include <check.h>

#include "message_channel.h"
#include "frame_codec.h"

namespace particle::constrained {

namespace {

const unsigned MIN_LORAWAN_APP_PORT = 1;
const unsigned MAX_LORAWAN_APP_PORT = 223;

} // namespace

struct MessageChannel::InRequest: RefCount {
    unsigned id;
    unsigned sessionId;

    InRequest() :
            id(0),
            sessionId(0) {
    }
};

struct MessageChannel::OutRequest: RefCount {
    RequestOptions options;
    OnResponse onResponse;
    unsigned id;

    OutRequest() :
            id(0) {
    }
};

MessageChannel::MessageChannel() :
        maxPayloadSize_(100), // TODO
        nextOutReqId_(0),
        sessId_(0),
        inited_(false) {
}

MessageChannel::~MessageChannel() {
}

int MessageChannel::init(MessageChannelConfig conf) {
    if (inited_) {
        return 0;
    }
    if (!conf.onSend_ || conf.port_ < MIN_LORAWAN_APP_PORT || conf.port_ > MAX_LORAWAN_APP_PORT) {
        return Error::INVALID_ARGUMENT;
    }
    conf_ = std::move(conf);
    inited_ = true;
    return 0;
}

int MessageChannel::receive(util::Buffer data, int port) {
    if (!inited_) {
        return Error::INVALID_STATE;
    }

    FrameHeader h;
    size_t headerSize = CHECK(decodeFrameHeader(data.data(), data.size(), h));

    // Resize the buffer so that it only contains the payload data
    std::memmove(data.data(), data.data() + headerSize, data.size() - headerSize);
    CHECK(data.resize(data.size() - headerSize));

    if (!h.hasFrameType() || h.frameType() == FrameType::REQUEST || h.frameType() == FrameType::REQUEST_NO_RESPONSE) {
        // Handle a request
        if (!conf_.onReq_) {
            return 0; // Ignore
        }
        OnResponse onResp;
        bool noResp = !h.hasFrameType() || h.frameType() == FrameType::REQUEST_NO_RESPONSE;
        if (!noResp) {
            RefCountPtr<InRequest> req = makeRefCountPtr<InRequest>();
            if (!req) {
                return Error::NO_MEMORY;
            }
            req->id = h.requestId();
            req->sessionId = sessId_;
            onResp = [this, req = std::move(req)](int error, int result, util::Buffer data) {
                if (error < 0) {
                    Log.error("Request error: %d", error);
                    return 0;
                }
                return sendResponse(result, std::move(data), std::move(req));
            };
        } else {
            // No response needed
            onResp = [this, sessId = sessId_](int error, int result, util::Buffer data) -> int {
                if (sessId != sessId_) {
                    return Error::CANCELLED;
                }
                return 0;
            };
        }
        int r = conf_.onReq_(h.requestTypeOrResultCode(), std::move(data), std::move(onResp));
        if (r < 0) {
            Log.error("Request handler failed: %d", r);
        }
    } else if (h.hasFrameType() && h.frameType() == FrameType::RESPONSE) {
        // Handle a response
        auto it = outReqs_.find(h.requestId());
        if (it == outReqs_.end()) {
            return 0;
        }
        auto req = std::move(it->second);
        outReqs_.erase(it);
        if (req->onResponse) {
            int r = req->onResponse(0 /* error */, h.requestTypeOrResultCode(), std::move(data));
            if (r < 0) {
                Log.error("Response handler failed: %d", r);
            }
        }
    }
    return 0;
}

int MessageChannel::changeMaxPayloadSize(size_t size) {
    if (!inited_) {
        return Error::INVALID_STATE;
    }
    return Error::NOT_SUPPORTED; // TODO
}

int MessageChannel::run() {
    if (!inited_) {
        return 0;
    }
    // TODO
    return 0;
}

int MessageChannel::sendRequest(unsigned type, util::Buffer data, OnResponse onResp, RequestOptions opts) {
    if (!inited_) {
        return Error::INVALID_STATE;
    }

    auto id = nextOutReqId_;
    if (++nextOutReqId_ > MAX_REQUEST_ID) {
        nextOutReqId_ = 0;
    }

    bool noResp = opts.noResponse();
    if (!noResp) {
        RefCountPtr<OutRequest> req = makeRefCountPtr<OutRequest>();
        if (!req) {
            return Error::NO_MEMORY;
        }
        req->id = id;
        req->onResponse = std::move(onResp);
        req->options = std::move(opts);
        if (!outReqs_.set(id, std::move(req))) {
            return Error::NO_MEMORY;
        }
    }
    NAMED_SCOPE_GUARD(removeReqGuard, {
        if (!noResp) {
            outReqs_.remove(id);
        }
    });

    FrameHeader h;
    h.requestTypeOrResultCode(type);
    if (!noResp) {
        h.frameType(FrameType::REQUEST);
        h.requestId(id);
    }
    char headerData[MAX_FRAME_HEADER_SIZE] = {};
    size_t headerSize = CHECK(encodeFrameHeader(headerData, sizeof(headerData), h));

    util::Buffer buf;
    CHECK(buf.resize(headerSize + data.size()));
    std::memcpy(buf.data(), headerData, headerSize);
    std::memcpy(buf.data() + headerSize, data.data(), data.size());

    assert(conf_.onSend_);
    CHECK(conf_.onSend_(std::move(buf), conf_.port_, nullptr /* TODO: onAck */));

    removeReqGuard.dismiss();

    return 0;
}

void MessageChannel::reset() {
    if (!inited_) {
        return;
    }

    decltype(outReqs_) outReqs;
    using std::swap;
    swap(outReqs, outReqs_);

    ++sessId_;

    // Cancel outgoing requests
    for (auto& [id, req]: outReqs) {
        if (req->onResponse && !req->options.noResponse()) {
            req->onResponse(Error::CANCELLED, 0, util::Buffer());
        }
    }
}

int MessageChannel::sendResponse(int result, util::Buffer data, RefCountPtr<InRequest> req) {
    assert(req);
    if (req->sessionId != sessId_) {
        return Error::CANCELLED;
    }

    FrameHeader h;
    h.requestTypeOrResultCode(result);
    h.frameType(FrameType::RESPONSE);
    h.requestId(req->id);

    char headerData[MAX_FRAME_HEADER_SIZE] = {};
    size_t headerSize = CHECK(encodeFrameHeader(headerData, sizeof(headerData), h));

    util::Buffer buf;
    CHECK(buf.resize(headerSize + data.size()));
    std::memcpy(buf.data(), headerData, headerSize);
    std::memcpy(buf.data() + headerSize, data.data(), data.size());

    assert(conf_.onSend_);
    CHECK(conf_.onSend_(std::move(buf), conf_.port_, nullptr /* TODO: onAck */));

    return 0;
}

} // namespace particle::constrained
