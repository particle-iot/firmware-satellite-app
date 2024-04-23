#pragma once

#include <optional>

#include <spark_wiring_variant.h>
#include <spark_wiring_map.h>

#include "message_channel.h"

namespace particle::constrained {

class CloudProtocol;

class CloudProtocolConfig {
public:
    CloudProtocolConfig() = default;

    CloudProtocolConfig& onSend(MessageChannel::OnSend fn) {
        onSend_ = std::move(fn);
        return *this;
    }

private:
    MessageChannel::OnSend onSend_;

    friend class CloudProtocol;
};

class CloudProtocol {
public:
    typedef std::function<void(int code, Variant data)> OnEvent;

    CloudProtocol() :
            state_(State::NEW) {
    }

    int init(CloudProtocolConfig conf);

    int connect();
    void disconnect();
    int receive(util::Buffer data, int port);
    int run();

    int publish(int code) {
        return publishImpl(code, std::nullopt);
    }

    int publish(int code, Variant data) {
        return publishImpl(code, std::move(data));
    }

    int subscribe(int code, OnEvent onEvent);

private:
    enum class State {
        NEW,
        DISCONNECTED,
        CONNECTED
    };

    MessageChannel channel_;
    CloudProtocolConfig conf_;
    Map<int, OnEvent> subscrs_;
    State state_;

    int publishImpl(int code, std::optional<Variant> data);

    int receiveRequest(unsigned type, util::Buffer data, MessageChannel::OnResponse onResp);

    int receiveEventRequest(util::Buffer data, MessageChannel::OnResponse onResp);
    int receiveDiagnosticsRequest(util::Buffer data, MessageChannel::OnResponse onResp);
};

} // namespace particle::constrained
