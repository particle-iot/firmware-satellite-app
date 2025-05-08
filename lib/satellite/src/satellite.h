/*
 * Copyright (c) 2024 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "Particle.h"

#include "system_error.h"
#include "cloud_protocol.h"

#include <optional>

const auto NW_CONNECTED_INIT = 0;
const auto NW_CONNECTED_SUCCESS = 1;
const auto NW_CONNECTED_FAILED = 2;

namespace particle {

struct GnssPositioningInfo {
    uint16_t version;
    uint16_t size;
    double latitude;
    double longitude;
    float accuracy;
    float altitude;
    float cog;
    float speedKmph;
    float speedKnots;
    struct tm utcTime;
    int satsInView;
    bool locked;
    int posMode;
    int valid;
};

class SpecialJSONWriter : public spark::JSONBufferWriter {

  public:
    SpecialJSONWriter(char *buf, size_t size) : spark::JSONBufferWriter(buf, size) {

    }
    using spark::JSONBufferWriter::write;
};

class Satellite {

public:

    Satellite();
    ~Satellite();

    int begin(void);
    int connect(void);
    int disconnect(void);
    bool connected(void);
    void updateRegistration(bool force = false);
    int tx(const uint8_t* buf, size_t len, int port);

    int publish(int code) {
        return proto_.publish(code);
    }

    int publish(int code, const Variant& data) {
        return proto_.publish(code, data);
    }

    int subscribe(int code, constrained::CloudProtocol::OnEvent onEvent) {
        return proto_.subscribe(code, std::move(onEvent));
    }

    int getGNSSLocation(unsigned int maxFixWaitTimeMs = 30000);
    int publishLocation();

    int process();

    GnssPositioningInfo lastPositionInfo(void) {
        return lastPositionInfo_;
    };

private:

    bool begun_; // true if begin() previously called

    bool registered_ = false;
    uint8_t nwConnected = NW_CONNECTED_INIT;
    uint32_t lastReceivedCheck_ = 0;
    uint32_t lastRegistrationCheck_ = 0;
    uint32_t registrationUpdateMs_ = 0;
    int errorCount_ = 0;
    GnssPositioningInfo lastPositionInfo_;
    constrained::CloudProtocol proto_;

    char publishBuffer[1024] = {};

    static int cbICCID(int type, const char* buf, int len, char* iccid);
    static int cbCOPS(int type, const char* buf, int len, char* network);
    static int cbQCFGEXTquery(int type, const char* buf, int len, int* rxlen);
    static int cbQCFGEXTread(int type, const char* buf, int len, char* rxdata);
    static int cbQGPSLOC(int type, const char* buf, int len, GnssPositioningInfo* info);

    int isRegistered(void);
    int waitAtResponse(unsigned int tries, unsigned int timeout = 1000);
    int publishImpl(int code, const std::optional<Variant>& data = std::nullopt);
    
    void receiveData(void);
    int processErrors(void);
};

} // particle