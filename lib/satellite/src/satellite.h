/*
 * Copyright (c) 2026 Particle Industries, Inc.  All rights reserved.
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

const uint8_t NW_CONNECTED_INIT = 0;
const uint8_t NW_CONNECTED_SUCCESS = 1;
const uint8_t NW_CONNECTED_FAILED = 2;

const uint8_t NW_STATE_IDLE = 0;
const uint8_t NW_STATE_CONNECT = 1;
const uint8_t NW_STATE_DISCONNECT = 2;

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

// Parsed AT+QENG="servingcell" response. Field order follows the Quectel
// servingcell report:
//   +QENG: "servingcell",<state>,<rat>,<duplex>,<mcc>,<mnc>,<cellId(hex)>,
//          <pcid>,<earfcn>,<band>,<ulBw>,<dlBw>,<tac(hex)>,<rsrp>,<rsrq>,
//          <rssi>,<sinr>,<srxlev>
// When the modem is still searching it reports only the state field, so
// `valid` distinguishes "full signal metrics present" from a bare state.
struct NtnServingCellInfo {
    bool valid = false;          // true when the full signal metrics parsed
    char state[12] = {};         // SEARCH / LIMSRV / CONNECT / NOCONN ...
    char rat[16] = {};           // e.g. "NTN NBIoT"
    char duplex[8] = {};         // FDD / TDD
    int mcc = 0;                 // mobile country code (901 = Skylo NTN shared)
    int mnc = 0;                 // mobile network code
    unsigned int cellId = 0;     // cell identity (hex on the wire)
    int pcid = 0;                // physical cell ID
    int earfcn = 0;              // channel number
    int band = 0;                // frequency band
    int ulBandwidth = 0;
    int dlBandwidth = 0;
    unsigned int tac = 0;        // tracking area code (hex on the wire)
    int rsrp = 0;                // dBm
    int rsrq = 0;                // dB
    int rssi = 0;                // dBm
    int sinr = 0;                // Quectel index 0-250 -> -20..+30 dB
    int srxlev = 0;              // cell-selection RX level
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

    int getGNSSLocation(unsigned int maxFixWaitTimeMs = 120000);
    int publishLocation();

    void setMaxPayloadSize(size_t size) {
        maxPayloadSize_ = size;
    }

    // Provide the location used for the NTN location fix (AT+QNWCFG="ntn_locfix").
    // Stores the coordinates so begin() can program them into the modem before
    // NTN registration. Must be called before begin() to take effect on the
    // next registration.
    //
    // forceFixed: when true, getGNSSLocation() short-circuits to these stored
    // coordinates and never queries the modem's GNSS engine. Use this when the
    // device has no GNSS antenna and the app is supplying a static location.
    int setLocationFix(double lat, double lon, double alt, bool forceFixed = false);

    int process(bool force = false);

    GnssPositioningInfo lastPositionInfo(void) {
        return lastPositionInfo_;
    };

    NtnServingCellInfo servingCellInfo(void) {
        return servingCell_;
    };

private:

    bool begun_; // true if begin() previously called

    uint8_t registered_ = 0;
    volatile uint8_t ntnConnected_ = 0;
    volatile uint8_t nwConnected_ = NW_CONNECTED_INIT;
    volatile uint8_t nwConnectionDesired_ = NW_STATE_IDLE;
    uint32_t lastReceivedCheck_ = 0;
    uint32_t lastRegistrationCheck_ = 0;
    uint32_t lastServingCellCheck_ = 0;
    uint32_t registrationUpdateMs_ = 0;
    uint32_t noRegistrationTimer_ = 0;
    int errorCount_ = 0;
    GnssPositioningInfo lastPositionInfo_;
    NtnServingCellInfo servingCell_;

    // NTN location fix coordinates programmed via AT+QNWCFG="ntn_locfix".
    double locLat_ = 0;
    double locLon_ = 0;
    double locAlt_ = 0;
    bool locFixValid_ = false;
    // When true, getGNSSLocation() returns the stored loc{Lat,Lon,Alt}_ without
    // querying the GNSS engine. Set via setLocationFix(forceFixed=true).
    bool locForceFixed_ = false;

    size_t maxPayloadSize_ = 0;
    constrained::CloudProtocol proto_;

    char publishBuffer[1024] = {};

    static int cbCFUN(int type, const char* buf, int len, int* cfun);
    static int cbCOPS(int type, const char* buf, int len, char* network);
    static int cbQCFGEXTquery(int type, const char* buf, int len, int* rxlen);
    static int cbQIRDquery(int type, const char* buf, int len, int* rxlen);
    static int cbQIRD(int type, const char* buf, int len, char* outBuf);
    static int cbQISENDEX(int type, const char* buf, int len, int* param);
    static int cbQCFGEXTread(int type, const char* buf, int len, char* rxdata);
    static int cbQGPSLOC(int type, const char* buf, int len, GnssPositioningInfo* info);
    static int cbQENG(int type, const char* buf, int len, NtnServingCellInfo* info);

    int isRegistered(void);
    int queryServingCell(void);
    int waitAtResponse(unsigned int tries, unsigned int timeout = 1000);
    int publishImpl(int code, const std::optional<Variant>& data = std::nullopt);
    void updateRegistration(bool force = false);

    void receiveData(void);
    int processErrors(void);
    int connectImpl(void);
};

} // particle