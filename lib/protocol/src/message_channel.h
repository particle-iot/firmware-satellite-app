#pragma once

#include <functional>

#include <spark_wiring_ticks.h>
#include <spark_wiring_map.h>

#include <ref_count.h>

#include "util/buffer.h"

namespace particle::constrained {

class MessageChannel;

class MessageChannelBase {
public:
    typedef std::function<void(int error)> OnAck;
    typedef std::function<int(int error, int result, util::Buffer data)> OnResponse;
    typedef std::function<int(int type, util::Buffer data, OnResponse onResp)> OnRequest;
    typedef std::function<int(util::Buffer data, int port, OnAck onAck)> OnSend;

    static const system_tick_t DEFAULT_REQUEST_TIMEOUT = 60000;
    static const unsigned DEFAULT_PORT = 223;
};

class MessageChannelConfig {
public:
    MessageChannelConfig() :
            port_(MessageChannelBase::DEFAULT_PORT) {
    }

    MessageChannelConfig& onRequest(MessageChannelBase::OnRequest fn) {
        onReq_ = std::move(fn);
        return *this;
    }

    MessageChannelConfig& onSend(MessageChannelBase::OnSend fn) {
        onSend_ = std::move(fn);
        return *this;
    }

    MessageChannelConfig& port(unsigned port) {
        port_ = port;
        return *this;
    }

private:
    MessageChannelBase::OnRequest onReq_;
    MessageChannelBase::OnSend onSend_;
    unsigned port_;

    friend class MessageChannel;
};

class RequestOptions {
public:
    RequestOptions() :
            timeout_(MessageChannelBase::DEFAULT_REQUEST_TIMEOUT),
            noResp_(false) {
    }

    RequestOptions& timeout(system_tick_t timeout) {
        timeout_ = timeout;
        return *this;
    }

    system_tick_t timeout() const {
        return timeout_;
    }

    RequestOptions& noResponse(bool enabled) {
        noResp_ = enabled;
        return *this;
    }

    bool noResponse() const {
        return noResp_;
    }

private:
    system_tick_t timeout_;
    bool noResp_;
};

class MessageChannel: public MessageChannelBase {
public:
    MessageChannel();
    ~MessageChannel();

    int init(MessageChannelConfig conf);

    int receive(util::Buffer data, int port);
    int changeMaxPayloadSize(size_t size);
    int run();

    int sendRequest(unsigned type, util::Buffer data, OnResponse onResp = nullptr, RequestOptions opts = RequestOptions());

    int sendRequest(unsigned type, OnResponse onResp = nullptr, RequestOptions opts = RequestOptions()) {
        return sendRequest(type, util::Buffer(), std::move(onResp), std::move(opts));
    }

    void reset();

private:
    struct InRequest;
    struct OutRequest;

    Map<unsigned, RefCountPtr<OutRequest>> outReqs_;
    MessageChannelConfig conf_;
    size_t maxPayloadSize_;
    unsigned nextOutReqId_;
    unsigned sessId_;
    bool inited_;

    int sendResponse(int result, util::Buffer data, RefCountPtr<InRequest> req);
};

} // namespace particle::constrained
