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

#include "satellite.h"

#include "logging.h"
LOG_SOURCE_CATEGORY("ncp.client");

#include "check.h"
#include "scope_guard.h"
#include "stream_util.h"
#include "hex_to_bytes.h"

#include <str_util.h>

#include <memory>
#include <cstdint>
#include <pb_encode.h>
#include <cloud/cloud_new.pb.h>

/*
// List of all defined system errors
    NONE                        0
    UNKNOWN                  -100
    BUSY                     -110
    NOT_SUPPORTED            -120
    NOT_ALLOWED              -130
    CANCELLED                -140
    ABORTED                  -150
    TIMEOUT                  -160
    NOT_FOUND                -170
    ALREADY_EXISTS           -180
    TOO_LARGE                -190
    NOT_ENOUGH_DATA          -191
    LIMIT_EXCEEDED           -200
    END_OF_STREAM            -201
    INVALID_STATE            -210
    FLASH_IO                 -219
    IO                       -220
    WOULD_BLOCK              -221
    FILE                     -225
    PATH_TOO_LONG            -226
    NETWORK                  -230
    PROTOCOL                 -240
    INTERNAL                 -250
    NO_MEMORY                -260
    INVALID_ARGUMENT         -270
    BAD_DATA                 -280
    OUT_OF_RANGE             -290
    DEPRECATED               -300
    ...
    AT_NOT_OK               -1200
    AT_RESPONSE_UNEXPECTED  -1210
    ...
*/

namespace particle {

using namespace constrained;

namespace {

#define SATELLITE_NCP_RX_DATA_READ_TIMEOUT_MS (3000)
#define SATELLITE_NCP_REGISTRATION_UPDATE_SLOW_MS (60000)
#define SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS (15000)
#define SATELLITE_NCP_RECEIVE_UPDATE_MS (10000)

#define SATELLITE_NCP_NO_REGISTRATION_MS (540000)

#define SATELLITE_NCP_COMM_ERRORS_MAX (3)

#define SATELLITE_NCP_COPS_TIMEOUT_MS (180000)

bool celullarNotReady() {
    return !Cellular.ready();
}

bool wifiNotReady() {
    return !WiFi.ready();
}

} // namespace annonymous

Satellite::Satellite() : begun_(false), registrationUpdateMs_(SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS)
{
    nwConnectionDesired = NW_STATE_IDLE;
}

Satellite::~Satellite() {
    if (begun_) {
        // de-init stuff
    }
}

int Satellite::cbCFUN(int type, const char* buf, int len, int* cfun)
{
    if ((type == TYPE_PLUS) && cfun) {
        if (sscanf(buf, "\r\n+CFUN: %d", cfun) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbICCID(int type, const char* buf, int len, char* iccid)
{
    if ((type == TYPE_PLUS) && iccid) {
        if (sscanf(buf, "\r\n+QCCID: %[^\r]\r\n", iccid) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbCOPS(int type, const char* buf, int len, char* network)
{
    if ((type == TYPE_PLUS) && network) {
        if (sscanf(buf, "\r\n+COPS: 0,0,\"%[^\"]\r\n", network) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbQCFGEXTquery(int type, const char* buf, int len, int* rxlen)
{
    if ((type == TYPE_PLUS) && rxlen) {
        if (sscanf(buf, "\r\n+QCFGEXT: \"nipdr\",%*d,%*d,%d\r\n", rxlen) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbQCFGEXTread(int type, const char* buf, int len, char* rxdata)
{
    if ((type == TYPE_PLUS) && rxdata) {
        if (sscanf(buf, "\r\n+QCFGEXT: \"nipdr\",%*d,%s\r\n", rxdata) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int Satellite::cbQGPSLOC(int type, const char* buf, int len, GnssPositioningInfo* info)
{
    if ((type == TYPE_PLUS) && info) {
        if (sscanf(buf, "\r\n+QGPSLOC: %02d%02d%02d.%*03d,%lf,%lf,%f,%f,%d,%f,%f,%f,%02d%02d%02d,%d\r\n",
                        &info->utcTime.tm_hour, &info->utcTime.tm_min, &info->utcTime.tm_sec,
                        &info->latitude, &info->longitude, &info->accuracy, &info->altitude,
                        &info->posMode, &info->cog, &info->speedKmph, &info->speedKnots,
                        &info->utcTime.tm_mday, &info->utcTime.tm_mon, &info->utcTime.tm_year,
                        &info->satsInView) == 15) {
            info->valid = 1;
        }
    }
    return WAIT;
}

int Satellite::getICCID(char* i, bool log) {
    char iccid[30] = {0};

    int ret = Cellular.command(cbICCID, iccid, 10000, "AT+QCCID\r\n");
    if ((ret == RESP_OK) && (strcmp(iccid, "") != 0)) {
        // Log.info("SIM ICCID = %s", iccid);
    } else {
        Log.info("SIM ICCID NOT FOUND!");
        return -1;
    }

    strcpy(i, iccid);
    return 0;
}

int Satellite::isRegistered() {
    bool reg = false;
    char network[32] = "";
    if ((RESP_OK == Cellular.command(cbCOPS, network, 10000, "AT+COPS?\r\n"))
            && (strcmp(network,"") != 0))
    {
        Log.info("SATELLITE NETWORK REGISTERED = %s\r\n", network);
        reg = true;
        noRegistrationTimer_ = 0;
    } else {
        if (!noRegistrationTimer_) {
            noRegistrationTimer_ = millis();
        }
    }

    return reg;
}

int Satellite::waitAtResponse(unsigned int tries, unsigned int timeout) {
    unsigned int attempt = 0;
    for (;;) {
        const int r = Cellular.command(timeout, "AT\r\n");
        if (r < 0 && r != SYSTEM_ERROR_TIMEOUT) {
            return r;
        }
        if (r == RESP_OK) {
            return SYSTEM_ERROR_NONE;
        }
        if (++attempt >= tries) {
            break;
        }
    }
    return SYSTEM_ERROR_TIMEOUT;
}

int Satellite::begin() { // (const SatelliteConfig& conf) {
    begun_ = true;
    errorCount_ = 0;
    // conf_ = conf;

    ntnConnected = 0; // assume we need to reconnect

    if (!Cellular.isOn() || Cellular.isOff()) {
        // Turn on the modem
        Cellular.on();
        if (!waitFor(Cellular.isOn, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }
    }

    if (Cellular.ready()) {
        if (Particle.connected()) {
            return SYSTEM_ERROR_INVALID_STATE;
        }

        // If disconnected from the cloud but cellular still connected, disconnect.
        Cellular.disconnect();
        if (!waitFor(celullarNotReady, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }
    }

    waitAtResponse(10); // Check if the module is alive

    Cellular.command(2000, "AT+QGMR\r\n");

    char iccid[32] = "";
    getICCID(iccid, /* log results */ true);

    Cellular.command(2000, "AT+QCFG=\"band\"\r\n");
    Cellular.command(2000, "AT+CEREG=2\r\n");
    Cellular.command(2000, "AT+CEREG?\r\n");
    Cellular.command(2000, "AT+COPS=3,0\r\n");
    if (isRegistered()) {
        registered_ = true;
        Log.info("SKIPPING THE FOLLOWING COMMANDS:\n"
            "\"AT+CFUN=0\"\n"
            "\"AT+CGDCONT=1,\"Non-IP\",\"particle.io\"\n"
            "\"AT+QCFG=\"nwscanmode\",3,1\n"
            "\"AT+QCFG=\"iotopmode\",3,1\n"
            "\"AT+CFUN=1\n");
    } else {
        Cellular.command(180000, "AT+CFUN=0\r\n");
        Cellular.command(2000, "AT+CGDCONT=1,\"Non-IP\",\"particle.io\"");
        Cellular.command(2000, "AT+QCFG=\"nwscanmode\",3,1\r\n"); // LTE (includes NTN)
        Cellular.command(2000, "AT+QCFG=\"iotopmode\",3,1\r\n");  // NTN only
        Cellular.command(180000, "AT+CFUN=1\r\n");
    }

    Log.trace("Initializing protocol handler");
    CloudProtocolConfig protoConf;
    protoConf.onSend([this](auto data, auto port, auto /* onAck */) {
        return tx((const uint8_t*)data.data(), data.size(), port);
    });
    int r = proto_.init(protoConf);
    if (r < 0) {
        Log.error("CloudProtocol::init() failed: %d", r);
        return r;
    }

    return 0;
}

int Satellite::connect() {
    nwConnectionDesired = NW_STATE_CONNECT;
    nwConnected = NW_CONNECTED_INIT;

    return 0;
}

int Satellite::connectImpl() {
    if (nwConnectionDesired != NW_STATE_CONNECT || connected()) {
        return 0;
    }

    auto r = 0;
    static uint32_t lastConnectAttempt;

    if (millis() - lastConnectAttempt > 5000) {
        if (!ntnConnected) {
            if (isRegistered()) {
                Cellular.command(2000, "AT+CEREG?\r\n");
                int r = 0;
                r = Cellular.command(2000, "AT+QCFGEXT=\"nipdcfg\",0,\"particle.io\"\r\n");
                if (r == RESP_OK) {
                    r = Cellular.command(2000, "AT+QCFGEXT=\"nipdcfg\"\r\n");
                }
                if (r == RESP_OK) {
                    r = Cellular.command(2000, "AT+QCFGEXT=\"nipd\",1,30\r\n");
                    ntnConnected = 1;
                } else {
                    ntnConnected = 0;
                    nwConnected = NW_CONNECTED_FAILED;
                }
            } else {
                Log.info("NOT REGISTERED YET");
                nwConnected = NW_CONNECTED_INIT;
                // Toggle CFUN if no registration for a long time
                if (millis() - noRegistrationTimer_ > SATELLITE_NCP_NO_REGISTRATION_MS) {
                    Log.info("No registration for %d minutes, toggling CFUN.", SATELLITE_NCP_NO_REGISTRATION_MS/60000);
                    Cellular.command(20000, "AT+CFUN=0\r\n");
                    Cellular.command(20000, "AT+CFUN=1\r\n");
                    noRegistrationTimer_ = millis();
                }
            }
            Cellular.command(2000, "AT+QENG=\"servingcell\"");
        } else {
            r = proto_.connect();
            if (r < 0) {
                Log.error("CloudProtocol::connect() failed: %d", r);
                nwConnected = NW_CONNECTED_FAILED;
                return r;
            }
            Log.trace("Connected to the Cloud");
            nwConnected = NW_CONNECTED_SUCCESS;
        }
        lastConnectAttempt = millis();
    }

    return 0;
}

int Satellite::disconnect() {
    nwConnectionDesired = NW_STATE_DISCONNECT;
    nwConnected = NW_CONNECTED_INIT;
    ntnConnected = 0;

    return 0;
}

bool Satellite::connected(void) {
    return nwConnected == NW_CONNECTED_SUCCESS && nwConnectionDesired == NW_STATE_CONNECT;
}

void Satellite::updateRegistration(bool force) {
    // periodically check for registration
    if (force || millis() - lastRegistrationCheck_ >= registrationUpdateMs_) {
        // Log.info("registered_:%d, connected():%d", registered_, connected());
        bool r = isRegistered();
        if (r && !registered_) {
            // we just reattached, reconnect to NTN
            nwConnected = NW_CONNECTED_INIT;
            registered_ = r;
            connect();
        } else if (!r) {
            // detached, reset NTN connection
            nwConnected = NW_CONNECTED_INIT;
        }
        registered_ = r;
        lastRegistrationCheck_ = millis();
        registrationUpdateMs_ = connected() ? SATELLITE_NCP_REGISTRATION_UPDATE_SLOW_MS : SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
    }
}

void Satellite::receiveData(void) {
    // check for incoming data and update cloud protocol
    if (registered_ && connected() && millis() - lastReceivedCheck_ >= SATELLITE_NCP_RECEIVE_UPDATE_MS) {
        lastReceivedCheck_ = millis();
        int recv = 0;
        if ((RESP_OK == Cellular.command(cbQCFGEXTquery, &recv, 10000, "AT+QCFGEXT=\"nipdr\",0\r\n"))
                && (recv > 0))
        {
            // Receive hex data
            char rxData[320] = "";
            if ((RESP_OK == Cellular.command(cbQCFGEXTread, rxData, 10000, "AT+QCFGEXT=\"nipdr\",%d,1\r\n", recv))
                && (strcmp(rxData,"") != 0))
            {

                Log.info("%d BYTES RECEIVED!", recv);
                // General counter response - 806006
                // Diagnostics request - 830000120306071A
                auto dataBuf = util::Buffer(recv);
                hexToBytes(rxData, dataBuf.data(), recv);
                LOG_DUMP(TRACE, dataBuf.data(), recv);
                LOG_PRINTF(TRACE, "\r\n");
                proto_.receive(dataBuf, 223);
            } else {
                Log.error("ERROR READING DATA!");
            }
        }
    }
}

int Satellite::tx(const uint8_t* buf, size_t len, int port) {
    if (!registered_ || !connected()) {
        return SYSTEM_ERROR_INVALID_STATE;
    }

    auto hexBufSize = len * 2 + 1; // Includes term. null
    std::unique_ptr<char[]> hexBuf(new(std::nothrow) char[hexBufSize]);
    if (!hexBuf) {
        return SYSTEM_ERROR_NO_MEMORY;
    }
    toHex(buf, len, hexBuf.get(), hexBufSize);
    // Send hex data
    if (RESP_OK == Cellular.command(2000, "AT+QCFGEXT=\"nipds\",1,\"%s\",%d\r\n", hexBuf.get(), len)) {
        Log.info("%d BYTES SENT!\r\n", len);
    } else {
        Log.error("ERROR SENDING DATA!");
        errorCount_++;
        return -1;
    }

    return 0;
}

int Satellite::getGNSSLocation(unsigned int maxFixWaitTimeMs) {
    GnssPositioningInfo info = {};
    auto s = millis();
    Cellular.command(2000, "AT+QGPS=1\r\n");
    delay(5000);

    do {
        Cellular.command(cbQGPSLOC, &info, 2000, "AT+QGPSLOC=2\r\n");

        if (info.valid) {
            Log.info("GPS TIME: %02d/%02d/%02d %02d:%02d:%02d", info.utcTime.tm_year, info.utcTime.tm_mon,
                    info.utcTime.tm_mday, info.utcTime.tm_hour, info.utcTime.tm_min, info.utcTime.tm_sec);
            Log.info("LOCATION: %.5lf, %.5lf, ALT:%.1f SATS:%d\r\n", info.latitude, info.longitude,
                    info.altitude, info.satsInView);
        } else {
            delay(5000);
        }
    } while (!info.valid && millis() - s < maxFixWaitTimeMs);

    Cellular.command(2000, "AT+QGPSEND\r\n");

    if (info.valid) {
        lastPositionInfo_ = info;
    }
    return info.valid == 1 ? 0 : -1;
}

int Satellite::publishLocation() {
    if (!lastPositionInfo_.valid) {
        return -1;
    }

    memset(publishBuffer, 0, sizeof(publishBuffer));
    SpecialJSONWriter writer(publishBuffer, sizeof(publishBuffer));
    auto now = (unsigned int)Time.now();
    writer.beginObject();
        writer.name("cmd").value("loc");
        writer.name("time").value(now);
        writer.name("loc").beginObject();
            writer.name("lck").value(1);
            writer.name("time").value(now);
            writer.name("lat").value(lastPositionInfo_.latitude);
            writer.name("lon").value(lastPositionInfo_.longitude);
            writer.name("alt").value(lastPositionInfo_.altitude);
        writer.endObject();
    writer.endObject();

    WiFi.on();
    waitUntil(WiFi.isOn);
    WiFi.connect();
    if (waitFor(WiFi.ready, 30000)) {
        Particle.connect();
        waitUntil(Particle.connected);

        Particle.publish("loc", publishBuffer);

        Particle.disconnect();
        waitUntil(Particle.disconnected);
        WiFi.disconnect();
        waitUntil(wifiNotReady);
        WiFi.off();
        waitUntil(WiFi.isOff);
    }

    return 0;
}

int Satellite::processErrors() {
    if (errorCount_ >= SATELLITE_NCP_COMM_ERRORS_MAX) {
        // reset modem and re-init
        Cellular.command(20000, "AT+CFUN=0\r\n");
        Cellular.command(20000, "AT+CFUN=1\r\n");
        errorCount_ = 0;
        registrationUpdateMs_ = SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
        nwConnected = NW_CONNECTED_INIT;
        ntnConnected = 0;
    }
    // TODO: Check for uncommanded band change
    // 0000001817 [ncp.at] TRACE: > AT+QCFG="band"
    // 0000001831 [ncp.at] TRACE: < +QCFG: "band",0xf,0x100002000000000f0e189f,0x10004200000000090e189f,0x7
    // 0000001859 [ncp.at] TRACE: < OK
    return 0;
}

int Satellite::process(bool force) {
    updateRegistration(force);
    connectImpl();
    receiveData();
    processErrors();
    proto_.run();

    return 0;
}

} // namespace particle


