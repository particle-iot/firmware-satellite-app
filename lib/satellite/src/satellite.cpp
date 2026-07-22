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

#define USE_NON_IP 0
#define UDP_CONNECT_ID 0

// Single source of truth for the UDP endpoint, shared by BOTH transports: the
// Device OS UDP API takes it as an IPAddress, and the AT path (QIOPEN) formats
// its host argument from the same octets in place - so the two can never drift
// apart. Numeric IPs only; a DNS hostname endpoint would need its own
// resolution step first. kUdpPort is the port the server listens on, paired
// with its IP - switch both by swapping one comment block. (Phase 1 secure
// ingress uses its own port, 9932, for port-based versioning, spec §9;
// TODO: confirm the deployed secure ingress host:port.)
// static const IPAddress kUdpEndpointIp(13, 219, 177, 65); // debug echo server "publish-receiver-udp.particle.io"
// static const uint16_t kUdpPort = 40000;                  // debug echo server
static const IPAddress kUdpEndpointIp(52, 5, 13, 97);       // secure ingress
static const uint16_t kUdpPort = 9932;                      // secure ingress

// The Device OS UDP socket also binds kUdpPort as its device-side source port
// (src and dst ports are independent namespaces, so this is safe). A fixed
// local port keeps the device's NAT mapping stable so cloud downlinks (sent to
// the source addr:port of the last verified uplink) stay routable between
// polls, and reusing kUdpPort means it tracks the endpoint block above
// automatically. The NTN AT path does not bind it: QIOPEN without a local port
// lets the modem pick an ephemeral one.
static const size_t kUdpRxBufferSize = 320; // matches the NTN path's rxData[320]

// Canonical on-wire datagram cap for outbound frames on both transports: the
// modem's AT-command body limit (256 raw bytes = 512 hex chars on the QISENDEX
// line). The Device OS UDP path could carry more, but both transports share one
// secure session and one cap so frame admission never depends on the active
// transport. The configured max payload size is clamped to this, the protocol
// layer's frame limit and tx()'s secure scratch buffer are both derived from
// it, and tx() re-checks the wrapped frame against it before sending.
static const size_t kMaxWireDatagramBytes = 256;

namespace particle {

using namespace constrained;

namespace {

#define SATELLITE_NCP_RX_DATA_READ_TIMEOUT_MS (3000)
#define SATELLITE_NCP_REGISTRATION_UPDATE_SLOW_MS (60000)
#define SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS (5000)
#define SATELLITE_NCP_RECEIVE_UPDATE_MS (10000)

#define SATELLITE_NCP_SERVINGCELL_UPDATE_MS (5000)

#define SATELLITE_NCP_NO_REGISTRATION_MS (300000)

#define SATELLITE_NCP_COMM_ERRORS_MAX (3)

#define SATELLITE_NCP_COPS_TIMEOUT_MS (180000)

} // namespace annonymous

Satellite::Satellite() : 
    begun_(false), 
    nwConnectionDesired_(NW_STATE_IDLE),
    registrationUpdateMs_(SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS) 
{

}

Satellite::~Satellite() {
}

int Satellite::cbCFUN(int type, const char* buf, int len, int* cfun)
{
    if ((type == TYPE_PLUS) && cfun) {
        if (sscanf(buf, "\r\n+CFUN: %d", cfun) == 1)
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

int Satellite::cbQIRDquery(int type, const char* buf, int len, int* rxlen)
{
    //+QIRD: <total_receive_length>,<have_read_length>,<unread_length>
    if (rxlen) {
        sscanf(buf, "\r\n+QIRD: %*d,%*d,%d\r\n", rxlen);
    }
    return WAIT;
}

int Satellite::cbQIRD(int type, const char* buf, int len, char* outBuf) {
  static int incomingPacketLength = 0;
  if (incomingPacketLength == 0) {
    sscanf(buf, "\r\n+QIRD: %d\r\n", &incomingPacketLength);
  } else if(outBuf) {
    // skip the leading "\r\n" in the response and copy the hex data to outBuf for processing
    memcpy(outBuf, &buf[2], incomingPacketLength);
    incomingPacketLength = 0;
  }

  return WAIT;
}

int Satellite::cbQISENDEX(int type, const char* buf, int len, int* param)
{
    if (strstr(buf, "SEND OK")) {
        return RESP_OK;
    }
    // QISENDEX reports a failed send as "SEND FAIL"; the modem may also emit a
    // bare "ERROR". Return RESP_ERROR so the command fails fast and the caller
    // can retry instead of waiting out the full timeout.
    if (strstr(buf, "SEND FAIL") || (type & TYPE_ERROR)) {
        return RESP_ERROR;
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

int Satellite::cbQENG(int type, const char* buf, int len, NtnServingCellInfo* info)
{
    if ((type != TYPE_PLUS) || !info) {
        return WAIT;
    }
    NtnServingCellInfo p;
    // state, rat, duplex are quoted; cellId and tac are hex; the rest decimal.
    // Example responses:
    // +QENG: "servingcell","SEARCH"
    // +QENG: "servingcell","LIMSRV","NTN NBIoT","FDD",901,98,DC379,9,7685,23,0,0,7D9,-123,-14,-108,85,17
    // +QENG: "servingcell","CONNECT","NTN NBIoT","FDD",901,98,DC379,9,7685,23,0,0,7D9,-126,-18,-107,75,
    // +QENG: "servingcell","NOCONN","NTN NBIoT","FDD",901,98,2C480D,29,7689,23,0,0,7ED,-117,-13,-103,102,23
    int n = sscanf(buf,
            "\r\n+QENG: \"servingcell\",\"%11[^\"]\",\"%15[^\"]\",\"%7[^\"]\",%d,%d,%x,%d,%d,%d,%d,%d,%x,%d,%d,%d,%d,%d",
            p.state, p.rat, p.duplex, &p.mcc, &p.mnc, &p.cellId, &p.pcid, &p.earfcn,
            &p.band, &p.ulBandwidth, &p.dlBandwidth, &p.tac, &p.rsrp, &p.rsrq,
            &p.rssi, &p.sinr, &p.srxlev);
    if (n >= 1) {
        // Full signal metrics require everything up to SINR (16 fields);
        // srxlev (the 17th) is often empty. A bare state (e.g. SEARCH) gives n==1.
        p.valid = (n >= 16);
        *info = p;
    }
    return WAIT;
}

// 0000307909 [ncp.at] TRACE: > AT+QNWCFG="ntn_locfix"
// 0000307925 [ncp.at] TRACE: < +QNWCFG: "ntn_locfix",1,38.073146,-122.165430,111
// 0000307946 [ncp.at] TRACE: < OK
int Satellite::cbQNWCFGNTNLOCFIX(int type, const char* buf, int len, GnssPositioningInfo* info)
{
    if ((type == TYPE_PLUS) && info) {
        if (sscanf(buf, "\r\n+QNWCFG: \"ntn_locfix\",%d,%lf,%lf,%f\r\n",
                        &info->posMode,
                        &info->latitude, &info->longitude, &info->altitude) == 4) {
            info->valid = 1;
            Log.info("Current NTN location fix: mode=%d, lat=%f, lon=%f, alt=%f",
                info->posMode, info->latitude, info->longitude, info->altitude);
        }
    }
    return WAIT;
}

// True if the modem's current ntn_locfix already matches our desired fixed fix within reason.
bool Satellite::locFixMatches(const GnssPositioningInfo& cur) const {
    constexpr double kCoordEps = 1e-4;
    return cur.valid && cur.posMode == 1
        && fabs(cur.latitude  - locLat_) < kCoordEps
        && fabs(cur.longitude - locLon_) < kCoordEps
        && lround(cur.altitude) == lround(locAlt_);
}

// Pure query: returns 1 if registered on a network, 0 otherwise.
int Satellite::isRegistered() {
    char network[32] = "";
    Cellular.command(2000, "AT+CEREG?");
    if ((RESP_OK == Cellular.command(cbCOPS, network, 10000, "AT+COPS?"))
            && (strcmp(network, "") != 0)) {
        // Log.trace("SATELLITE NETWORK REGISTERED = %s", network);
        return 1;
    }
    return 0;
}

// AT+QENG="servingcell" <state> values:
//   SEARCH  - no cell found yet, not on the network
//   LIMSRV  - camped on a cell, not yet registered
//   NOCONN  - camped AND registered, idle mode (no active bearer) - still attached
//   CONNECT - camped AND registered, active call/data in progress
static bool ntnRegistered(const char* state) {
    return strcmp(state, "CONNECT") == 0 || strcmp(state, "NOCONN") == 0;
}

// Query and parse the serving-cell report into servingCell_.
int Satellite::queryServingCell() {
    servingCell_ = NtnServingCellInfo{};
    Cellular.command(cbQENG, &servingCell_, 2000, "AT+QENG=\"servingcell\"");

    if (nwConnected_ == NW_CONNECTED_SUCCESS && !ntnRegistered(servingCell_.state)) {
        proto_.disconnect();
        ntnConnected_ = 0;
        nwConnected_ = NW_CONNECTED_INIT;
        ntnInit_ = 0; // in case we de-registered, make sure NTN is re-initialized
        // do not change state of nwConnectionDesired_, connection should come back on its own
    }

    return servingCell_.state[0] ? 0 : -1;
}

int Satellite::waitAtResponse(unsigned int tries, unsigned int timeout) {
    unsigned int attempt = 0;
    for (;;) {
        const int r = Cellular.command(timeout, "AT");
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

int Satellite::begin() {
    begun_ = true;
    errorCount_ = 0;

    // assume we need to reconnect
    ntnInit_ = 0;
    ntnConnected_ = 0;

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
        if (waitForNot(Cellular.ready, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }
    }

    waitAtResponse(10); // Check if the module is alive

    Cellular.command(2000, "AT+QGMR");

    // Check if ntn_locfix needs to be set or unset
    auto resetModem = false;
    GnssPositioningInfo locFixSetting = {};
    Cellular.command(cbQNWCFGNTNLOCFIX, &locFixSetting, 2000, "AT+QNWCFG=\"ntn_locfix\"");

    if (locForceFixed_) {
        if (!locFixMatches(locFixSetting)) {
            Log.info("Programming NTN location fix: %f,%f,%d", locLat_, locLon_,  (int)lround(locAlt_));
            Cellular.command(2000, "AT+QNWCFG=\"ntn_locfix\",1,%f,%f,%d", locLat_, locLon_,  (int)lround(locAlt_));
            resetModem = true;
        }
    } else {
        // If ntn is set and we need to unset it, do that and reset
        if (locFixSetting.valid && (locFixSetting.posMode == 1)) {
            Log.warn("Clearing NTN location fix");
            Cellular.command(2000, "AT+QNWCFG=\"ntn_locfix\",0");
            resetModem = true;
        } else if (!locFixSetting.valid) {
            Log.warn("No NTN location fix programmed; NTN registration may fail");
        }
    }

    if (resetModem) {
        Cellular.off();
        Cellular.on();
        if (!waitFor(Cellular.isOn, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }

        waitAtResponse(10);
        // Read back settings after reset
        Cellular.command(2000, "AT+QNWCFG=\"ntn_locfix\"");
    }

    Cellular.command(2000, "AT+QCFG=\"band\"");
    Cellular.command(2000, "AT+CEREG=2");
    Cellular.command(2000, "AT+CEREG?");
    Cellular.command(2000, "AT+COPS=3,0");

    if (isRegistered()) {
        registered_ = 1;
        Log.info("SKIPPING THE FOLLOWING COMMANDS:\n"
            "\"AT+CFUN=0\"\n"
            "\"AT+CGDCONT=1,\"IP\",\"360Connect\"\n"
            "\"AT+QCFG=\"nwscanmode\",3,1\n"
            "\"AT+QCFG=\"iotopmode\",3,1\n"
            "\"AT+CFUN=1\n");
    } else {
        Cellular.command(180000, "AT+CFUN=0");
        Cellular.command(2000, "AT+CGDCONT=1,\"IP\",\"360Connect\"");
        Cellular.command(2000, "AT+QCFG=\"nwscanmode\",3,1"); // LTE (includes NTN)
        Cellular.command(2000, "AT+QCFG=\"iotopmode\",3,1");  // NTN only
        Cellular.command(180000, "AT+CFUN=1");
    }

    return initProtocolStack();
}

// Protocol stack + secure-UDP session init shared by both transports (NTN
// begin() and beginCellularTransport()). Safe to call repeatedly:
// CloudProtocol::init() is a no-op once initialized, and the secure session is
// only (re)derived when not ready - each re-derivation burns up to a counter
// stride, so it must not run again on every radio switch.
int Satellite::initProtocolStack() {
    Log.trace("Initializing protocol handler");
    CloudProtocolConfig protoConf;
    protoConf.onSend([this](auto data, auto port, auto /* onAck */) {
        return tx((const uint8_t*)data.data(), data.size(), port);
    });

    // The configured cap is the ON-WIRE datagram limit. Clamp out-of-range
    // values to the transport maximum rather than admitting frames the modem
    // cannot carry (too high) or that no frame can ever fit (too low).
    size_t minWireCap = 1;
#if SECURE_UDP_ENABLED
    static_assert(secure_udp::kUplinkOverheadBytes < kMaxWireDatagramBytes,
            "secure overhead must leave room for a payload");
    minWireCap = secure_udp::kUplinkOverheadBytes + 1;
#endif
    if (maxPayloadSize_ < minWireCap || maxPayloadSize_ > kMaxWireDatagramBytes) {
        Log.warn("Max payload size %u out of range; clamping to %u",
            (unsigned)maxPayloadSize_, (unsigned)kMaxWireDatagramBytes);
        maxPayloadSize_ = kMaxWireDatagramBytes;
    }
    size_t maxProtoFrame = maxPayloadSize_;
#if SECURE_UDP_ENABLED
    // The secure frame wraps the protocol frame in kUplinkOverheadBytes of
    // KeyId/CounterLow/Tag, so the protocol layer only gets the remainder —
    // otherwise a cap-sized frame would leave tx() as cap + overhead bytes.
    maxProtoFrame -= secure_udp::kUplinkOverheadBytes;
#endif
    protoConf.maxPayloadSize(maxProtoFrame);
    int r = proto_.init(protoConf);
    if (r < 0) {
        Log.error("CloudProtocol::init() failed: %d", r);
        return r;
    }

#if SECURE_UDP_ENABLED
    // Derive the per-device keys (DCT private key + pinned cloud key) and load
    // counter watermarks. On failure (e.g. Device Protection on, §7.1) there is
    // no fallback to unauthenticated frames — fail here so begin() /
    // beginCellularTransport() report the fault instead of coming up "online"
    // with an uplink that can never send.
    if (!secureUdp_.ready() && !secureUdp_.init(secureUdpStore_)) {
        Log.error("Secure UDP init failed (device key unreadable? Device Protection on?)");
        return SYSTEM_ERROR_INVALID_STATE;
    }
    Log.info("Secure UDP session ready");
#endif

    return 0;
}

int Satellite::beginCellularTransport() {
    if (cellularTransportActive()) {
        return 0;
    }

    // No modem AT work here: on the cellular/WiFi profile Device OS owns the
    // modem, so the datagram transport is a Device OS UDP socket riding the
    // active network interface. The caller must have the network up
    // (Particle.connected()) before starting.
    // initProtocolStack() fails when the secure session cannot initialize —
    // there is no fallback to unauthenticated frames or Particle.publish; the
    // publisher stats surface the dropped publishes.
    int r = initProtocolStack();
    if (r < 0) {
        return r;
    }

    udp_.setBuffer(kUdpRxBufferSize);
    if (!udp_.begin(kUdpPort)) {
        Log.error("UDP begin on port %u failed", (unsigned)kUdpPort);
        return SYSTEM_ERROR_NETWORK;
    }
    transportMode_ = TransportMode::DEVICEOS_UDP;
    udpStarted_ = true;
    lastUdpReceiveCheck_ = 0;
    proto_.connect();
    Log.info("Constrained protocol over Device OS UDP started (dst %u.%u.%u.%u:%u, local port %u)",
        (unsigned)kUdpEndpointIp[0], (unsigned)kUdpEndpointIp[1],
        (unsigned)kUdpEndpointIp[2], (unsigned)kUdpEndpointIp[3],
        (unsigned)kUdpPort, (unsigned)kUdpPort);
    return 0;
}

void Satellite::endCellularTransport() {
    if (!udpStarted_) {
        return;
    }
    udp_.stop();
    udpStarted_ = false;
    transportMode_ = TransportMode::NTN_AT_SOCKET;
    // Reset the channel so pending out-requests whose ACKs can no longer be
    // routed don't linger; the NTN path re-connects the protocol later in
    // connectImpl().
    proto_.disconnect();
    Log.info("Constrained protocol over Device OS UDP stopped");
}

int Satellite::processCellularTransport() {
    if (!cellularTransportActive()) {
        return SYSTEM_ERROR_INVALID_STATE;
    }
    // Same receive poll cadence as the NTN path - timing parity is the point
    // of running the constrained protocol over this transport.
    if (millis() - lastUdpReceiveCheck_ >= SATELLITE_NCP_RECEIVE_UPDATE_MS) {
        lastUdpReceiveCheck_ = millis();
        // Drain everything waiting: the cloud can release several queued
        // downlinks between polls (one per verified uplink).
        int n = 0;
        while ((n = udp_.parsePacket()) > 0) {
            char rxData[320] = "";
            int len = udp_.read((unsigned char*)rxData, sizeof(rxData));
            if (len > 0) {
                Log.info("%d Bytes Read", len);
                handleInboundDatagram(rxData, (size_t)len);
            }
        }
    }
    proto_.run();
    return 0;
}

int Satellite::connect() {
    nwConnectionDesired_ = NW_STATE_CONNECT;
    nwConnected_ = NW_CONNECTED_INIT;

    int cfunVal = -1;
    if ( RESP_OK == Cellular.command(cbCFUN, &cfunVal, 180000, "AT+CFUN?") && cfunVal != 1 ) {
        Cellular.command(180000, "AT+CFUN=1");
    }

    return 0;
}

int Satellite::connectImpl() {
    if (nwConnectionDesired_ != NW_STATE_CONNECT || connected()) {
        return 0;
    }
    if (!registered_) {
        return 0;
    }

    static uint32_t lastConnectAttempt;
    if (millis() - lastConnectAttempt <= 5000) {
        return 0;
    }
    lastConnectAttempt = millis();

    if (!ntnInit_) {
        int r = 0;
#if USE_NON_IP
        r = Cellular.command(2000, "AT+QCFGEXT=\"nipdcfg\",0,\"particle.io\"");
        if (r == RESP_OK) {
            r = Cellular.command(2000, "AT+QCFGEXT=\"nipdcfg\"");
        }
        if (r == RESP_OK) {
            r = Cellular.command(2000, "AT+QCFGEXT=\"nipd\",1,30");
            ntnInit_ = 1;
        } else {
            ntnInit_ = 0;
        }
#else
        Cellular.command(2000, "AT+QICSGP=1");
        Cellular.command(2000, "AT+QIACT?");

        Cellular.command(2000, "AT+QICSGP=1,1,\"360Connect\"");
        r = Cellular.command(150 * 1000, "AT+QIACT=1");
        Cellular.command(2000, "AT+QIACT?");

        r = Cellular.command(150 * 1000, "AT+QIOPEN=1,%d,\"UDP\",\"%u.%u.%u.%u\",%u", UDP_CONNECT_ID,
                (unsigned)kUdpEndpointIp[0], (unsigned)kUdpEndpointIp[1],
                (unsigned)kUdpEndpointIp[2], (unsigned)kUdpEndpointIp[3],
                (unsigned)kUdpPort);

        if (r == RESP_OK) {
            ntnInit_ = 1;
        } else {
            Cellular.command(2000, "AT+QISTATE?");
            ntnInit_ = 0;
        }
#endif
    }

    if (ntnInit_) {
        queryServingCell();

        if (ntnRegistered(servingCell_.state)) {
            ntnConnected_ = 1;
        } else {
            ntnConnected_ = 0;
        }
    }

    if (ntnConnected_) {
        int r = proto_.connect();
        if (r < 0) {
            Log.error("CloudProtocol::connect() failed: %d", r);
            Log.warn("Ensure Satellite::begin() is called before Satellite::connect()");
            nwConnected_ = NW_CONNECTED_FAILED;
            errorCount_++;
            return r;
        }
        Log.info("Connected to the Satellite");
        nwConnected_ = NW_CONNECTED_SUCCESS;
    }

    return 0;
}

int Satellite::disconnect() {
    proto_.disconnect();
    nwConnectionDesired_ = NW_STATE_DISCONNECT;
    nwConnected_ = NW_CONNECTED_INIT;
    ntnConnected_ = 0;
    ntnInit_ = 0;
    registrationUpdateMs_ = SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
    registered_ = 0;

#if !USE_NON_IP
    Cellular.command(2000, "AT+QICLOSE=%d", UDP_CONNECT_ID);
    Cellular.command(2000, "AT+QIDEACT=1");
#endif

    Cellular.command(2000, "AT+CFUN=0"); // required to properly end NTN data session

    return 0;
}

bool Satellite::connected(void) {
    return (nwConnected_ == NW_CONNECTED_SUCCESS) && (nwConnectionDesired_ == NW_STATE_CONNECT);
}

// Sole owner of registration state. Polls registration on one timer, maintains
// registered_, handles reattach/detach, and recovers from prolonged loss of
// registration. connectImpl() consumes registered_ but never polls itself.
void Satellite::updateRegistration(bool force) {
    if (!force && millis() - lastRegistrationCheck_ < registrationUpdateMs_) {
        return;
    }
    lastRegistrationCheck_ = millis();

    int r = isRegistered() && (!servingCell_.state[0] || ntnRegistered(servingCell_.state));

    if (r) {
        noRegistrationTimer_ = 0;
        if (!registered_) {
            nwConnected_ = NW_CONNECTED_INIT;
            ntnConnected_ = 0;
        }
    } else {
        nwConnected_ = NW_CONNECTED_INIT;
        ntnInit_ = 0;
        ntnConnected_ = 0;
        if (!noRegistrationTimer_) {
            noRegistrationTimer_ = millis();
        } else if (millis() - noRegistrationTimer_ > SATELLITE_NCP_NO_REGISTRATION_MS) {
            // Prolonged no-registration: kick the radio.
            Log.info("No registration for %d minutes, toggling CFUN.", SATELLITE_NCP_NO_REGISTRATION_MS / 60000);
            Cellular.command(180000, "AT+CFUN=0");
            Cellular.command(180000, "AT+CFUN=1");
            noRegistrationTimer_ = millis();
        }
    }

    registered_ = r;
    // Poll fast until connected, then back off.
    registrationUpdateMs_ = connected() ? SATELLITE_NCP_REGISTRATION_UPDATE_SLOW_MS
                                         : SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
}

void Satellite::receiveData(void) {
    // check for incoming data and update cloud protocol
    if (registered_ && connected() && millis() - lastReceivedCheck_ >= SATELLITE_NCP_RECEIVE_UPDATE_MS) {
        lastReceivedCheck_ = millis();
        int recv = 0;
        char rxData[320] = "";
        int atResponse = 0;

#if USE_NON_IP
        atResponse = Cellular.command(cbQCFGEXTquery, &recv, 10000, "AT+QCFGEXT=\"nipdr\",0");
#else
        Cellular.command(2000, "AT+QISTATE?");
        atResponse = Cellular.command(cbQIRDquery, &recv, 60 * 1000, "AT+QIRD=%d,0", UDP_CONNECT_ID);
#endif
        if ((RESP_OK == atResponse) && (recv > 0)) {
#if USE_NON_IP
            atResponse = Cellular.command(cbQCFGEXTread, rxData, 10000, "AT+QCFGEXT=\"nipdr\",%d,1", recv);
#else
            atResponse = Cellular.command(cbQIRD, rxData, 10000, "AT+QIRD=%d,%d", UDP_CONNECT_ID, recv);
#endif
            // Receive hex data
            if ((RESP_OK == atResponse) && recv) {
                Log.info("%d Bytes Read", recv);
                handleInboundDatagram(rxData, (size_t)recv);
            } else {
                Log.error("ERROR READING DATA!");
            }
        }
    }
}

// Verify + dispatch one inbound datagram; shared by the NTN AT read path and
// the Device OS UDP poll.
void Satellite::handleInboundDatagram(char* data, size_t len) {
#if SECURE_UDP_ENABLED
    // Log the raw encrypted frame in the same hex format as tx(), so inbound
    // datagrams (echoes, dupes) can be matched to uplinks by frame counter.
    {
        char hexBuf[kUdpRxBufferSize * 2 + 1] = {};
        const size_t dumpLen = (len < kUdpRxBufferSize) ? len : kUdpRxBufferSize;
        auto hexLength = toHex(data, dumpLen, hexBuf, sizeof(hexBuf));
        Log.info("RX %u->%u bytes", (unsigned)len, (unsigned)hexLength);
        Log.trace("%s", hexBuf);
    }
    // Verify + strip the secure frame before handing the inner payload to the
    // protocol layer. Every non-Ok status drops the datagram (spec §6.2, §7.2),
    // but the modes are logged and counted separately: a storage fault after a
    // valid tag needs different remediation than an attack or replay noise.
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    const auto status = secureUdp_.verifyDownlink((const uint8_t*)data, len, payload, payloadLen);
    switch (status) {
        case secure_udp::Status::Ok: {
            auto dataBuf = util::Buffer((char*)payload, payloadLen);
            proto_.receive(dataBuf, 223);
            break;
        }
        case secure_udp::Status::Malformed:
            ++secureRxStats_.malformed;
            Log.warn("Secure UDP downlink malformed; dropping %u bytes", (unsigned)len);
            break;
        case secure_udp::Status::Replay:
            ++secureRxStats_.replay;
            Log.warn("Secure UDP downlink replayed/stale; dropping %u bytes", (unsigned)len);
            break;
        case secure_udp::Status::PersistFailed:
            ++secureRxStats_.persistFailed;
            Log.error("Secure UDP downlink authenticated but replay floor persist failed; dropping %u bytes",
                (unsigned)len);
            break;
        case secure_udp::Status::NotReady:
            ++secureRxStats_.notReady;
            Log.warn("Secure UDP downlink before session ready; dropping %u bytes", (unsigned)len);
            break;
        case secure_udp::Status::BadTag:
        case secure_udp::Status::CounterExhausted:
        default:
            ++secureRxStats_.badTag;
            Log.warn("Secure UDP downlink bad tag; dropping %u bytes", (unsigned)len);
            break;
    }
#else
    auto dataBuf = util::Buffer(data, len);
    LOG_DUMP(TRACE, dataBuf.data(), len);
    LOG_PRINTF(TRACE, "\r\n");
    proto_.receive(dataBuf, 223);
#endif
}

int Satellite::tx(const uint8_t* buf, size_t len, int port) {
    if (transportMode_ == TransportMode::DEVICEOS_UDP) {
        if (!udpStarted_ || !Particle.connected()) {
            return SYSTEM_ERROR_INVALID_STATE;
        }
    } else if (!registered_ || !connected()) {
        return SYSTEM_ERROR_INVALID_STATE;
    }

#if SECURE_UDP_ENABLED
    // Wrap the protocol payload as an authenticated uplink frame before the
    // existing hex-encode + QISENDEX. The secure layer is transparent to the
    // CloudProtocol caller (spec §5–§7). The scratch buffer is the wire cap:
    // anything the protocol layer admits (cap − overhead) fits after wrapping.
    uint8_t secureFrame[kMaxWireDatagramBytes];
    size_t frameLen = 0;
    const auto status = secureUdp_.protectUplink(buf, len, secureFrame, sizeof(secureFrame), frameLen);
    switch (status) {
        case secure_udp::Status::Ok:
            break;
        case secure_udp::Status::NotReady:
            Log.error("Secure UDP not ready; dropping %u-byte uplink", (unsigned)len);
            return SYSTEM_ERROR_INVALID_STATE;
        case secure_udp::Status::CounterExhausted:
            Log.error("Secure UDP uplink counter exhausted; dropping %u-byte uplink", (unsigned)len);
            return SYSTEM_ERROR_OUT_OF_RANGE;
        case secure_udp::Status::PersistFailed:
            Log.error("Secure UDP counter persistence failed; dropping %u-byte uplink", (unsigned)len);
            return SYSTEM_ERROR_FLASH_IO;
        case secure_udp::Status::TooLarge:
        default:
            Log.error("Secure UDP payload too large (payload=%u, max=%u; wire cap %u minus %u overhead)",
                (unsigned)len,
                (unsigned)(sizeof(secureFrame) - secure_udp::kUplinkOverheadBytes),
                (unsigned)sizeof(secureFrame),
                (unsigned)secure_udp::kUplinkOverheadBytes);
            return SYSTEM_ERROR_TOO_LARGE;
    }
    // Final wire-size gate: the protocol layer was sized to cap − overhead, so
    // this only catches drift between the two limits.
    if (frameLen > maxPayloadSize_) {
        Log.error("Secure frame %u bytes exceeds on-wire cap %u",
            (unsigned)frameLen, (unsigned)maxPayloadSize_);
        return SYSTEM_ERROR_TOO_LARGE;
    }
    buf = secureFrame;
    len = frameLen;
#endif

    if (transportMode_ == TransportMode::DEVICEOS_UDP) {
        // Raw datagram over the Device OS socket; no hex/AT framing. A send
        // failure here must NOT feed errorCount_ - that counter drives the
        // NTN modem (CFUN) recovery in processErrors(), and a UDP failure on
        // the normal connection must not queue a modem reset for the next
        // NTN session.
        int sent = udp_.sendPacket(buf, len, kUdpEndpointIp, kUdpPort);
        if (sent < (int)len) {
            Log.error("UDP sendPacket failed: %d (%u bytes)", sent, (unsigned)len);
            return -1;
        }
        Log.info("Bytes Sent %u", (unsigned)len);
        return 0;
    }

    auto hexBufSize = len * 2 + 1;
    std::unique_ptr<char[]> hexBuf(new(std::nothrow) char[hexBufSize]);
    if (!hexBuf) {
        return SYSTEM_ERROR_NO_MEMORY;
    }
    memset(hexBuf.get(), 0, hexBufSize);
    auto hexLength = toHex(buf, len, hexBuf.get(), hexBufSize);
    Log.info("TX %d->%d bytes", len, hexLength);
    Log.trace("%s", (char*)hexBuf.get());

    constexpr int kMaxSendAttempts = 3;
#if USE_NON_IP
    auto r = Cellular.command(2000, "AT+QCFGEXT=\"nipds\",1,\"%s\",%d", hexBuf.get(), len);
#else
    int dummy;
    int r = RESP_ERROR;
    for (int attempt = 1; attempt <= kMaxSendAttempts; ++attempt) {
        r = Cellular.command(cbQISENDEX, &dummy, 2000, "AT+QISENDEX=%d,\"%s\",0", UDP_CONNECT_ID, hexBuf.get());
        if (r == RESP_OK) {
            break;
        }
        Log.warn("QISENDEX attempt %d/%d failed: %d", attempt, kMaxSendAttempts, r);
    }
#endif
    // Send hex data
    if (RESP_OK == r) {
        Log.info("Bytes Sent %d", len);
    } else {
        Log.error("Error sending after %d attempts: %d bytes: %d", kMaxSendAttempts, len, r);
        errorCount_++;
        return -1;
    }

    return 0;
}

int Satellite::getGNSSLocation(unsigned int maxFixWaitTimeMs) {
    // No GNSS antenna mode: return the coordinates supplied via setLocationFix()
    // without ever touching the GNSS engine.
    if (locForceFixed_ && locFixValid_) {
        (void)maxFixWaitTimeMs;
        GnssPositioningInfo info = {};
        info.latitude  = locLat_;
        info.longitude = locLon_;
        info.altitude  = locAlt_;
        info.valid = 1;
        lastPositionInfo_ = info;
        Log.info("Using fixed location: %.5lf, %.5lf, ALT:%.1f", info.latitude, info.longitude, info.altitude);
        return 0;
    }

    GnssPositioningInfo info = {};
    auto s = millis();
    Cellular.command(2000, "AT+QGPS=1");
    delay(5000);

    do {
        Cellular.command(cbQGPSLOC, &info, 2000, "AT+QGPSLOC=2");

        if (info.valid) {
            Log.info("GPS TIME: %02d/%02d/%02d %02d:%02d:%02d", info.utcTime.tm_year, info.utcTime.tm_mon,
                    info.utcTime.tm_mday, info.utcTime.tm_hour, info.utcTime.tm_min, info.utcTime.tm_sec);
            Log.info("LOCATION: %.5lf, %.5lf, ALT:%.1f SATS:%d\r\n", info.latitude, info.longitude,
                    info.altitude, info.satsInView);
        } else {
            delay(5000);
        }
    } while (!info.valid && millis() - s < maxFixWaitTimeMs);

    Cellular.command(2000, "AT+QGPSEND");

    if (info.valid) {
        lastPositionInfo_ = info;
    }
    return info.valid == 1 ? 0 : -1;
}

int Satellite::setLocationFix(double lat, double lon, double alt, bool forceFixed) {
    locLat_ = lat;
    locLon_ = lon;
    locAlt_ = alt;
    locFixValid_ = true;
    locForceFixed_ = forceFixed;
    Log.info("NTN location fix set to %.5lf, %.5lf, ALT:%.1lf (forceFixed=%s)",
        lat, lon, alt, forceFixed ? "true" : "false");
    return 0;
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

    return 0;
}

int Satellite::processErrors() {
    if (errorCount_ >= SATELLITE_NCP_COMM_ERRORS_MAX) {
        Log.error("%d errors, resetting modem!", SATELLITE_NCP_COMM_ERRORS_MAX);
        // reset modem and re-init
        Cellular.command(20000, "AT+CFUN=0");
        Cellular.command(20000, "AT+CFUN=1");
        errorCount_ = 0;
        registrationUpdateMs_ = SATELLITE_NCP_REGISTRATION_UPDATE_FAST_MS;
        registered_ = 0;
        nwConnected_ = NW_CONNECTED_INIT;
        ntnInit_ = 0;
        ntnConnected_ = 0;
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
    // Refresh the serving-cell signal report for status/diagnostics.
    if (force || millis() - lastServingCellCheck_ >= SATELLITE_NCP_SERVINGCELL_UPDATE_MS) {
        lastServingCellCheck_ = millis();
        queryServingCell();
    }
    proto_.run();

    return 0;
}

} // namespace particle


