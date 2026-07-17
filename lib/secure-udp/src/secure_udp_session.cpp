#include "secure_udp_session.h"
#include "secure_udp_cloud_key.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "Particle.h"
#include "dct.h"
#include "rng_hal.h"

#include "uECC.h"

namespace particle::secure_udp {

namespace {

// micro-ecc RNG callback over the Device OS hardware RNG. Used to blind the
// ECDH scalar multiplication (side-channel hardening, see SecureUdpContext).
// Static ECDH does not strictly require it, but it is cheap on the one-time
// init path.
int ueccRng(uint8_t* dest, unsigned size) {
    while (size > 0) {
        const uint32_t r = HAL_RNG_GetRandomNumber();
        const unsigned n = size < sizeof(r) ? size : sizeof(r);
        memcpy(dest, &r, n);
        dest += n;
        size -= n;
    }
    return 1;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool parseHex(const char* hex, size_t hexLen, uint8_t* out, size_t outLen) {
    if (hexLen != 2 * outLen) {
        return false;
    }
    for (size_t i = 0; i < outLen; ++i) {
        const int hi = hexValue(hex[2 * i]);
        const int lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// SEC1 ECPrivateKey DER: SEQUENCE { INTEGER 1, OCTET STRING(32) privateKey, ... }.
// A fixed-shape walk (not a general ASN.1 parser); trailing optional fields and
// any DCT padding after the SEQUENCE are ignored. Mirrors the POC key loader.
struct Tlv {
    uint8_t tag;
    size_t len;
    const uint8_t* val;
    const uint8_t* next;
};

bool readTlv(const uint8_t* p, const uint8_t* end, Tlv& out) {
    if (end - p < 2) {
        return false;
    }
    out.tag = p[0];
    size_t len = p[1];
    const uint8_t* v = p + 2;
    if (len & 0x80) {
        const size_t n = len & 0x7f;
        if (n == 0 || n > 4 || static_cast<size_t>(end - v) < n) {
            return false;
        }
        len = 0;
        for (size_t i = 0; i < n; ++i) {
            len = (len << 8) | *v++;
        }
    }
    if (static_cast<size_t>(end - v) < len) {
        return false;
    }
    out.len = len;
    out.val = v;
    out.next = v + len;
    return true;
}

bool parseSec1PrivateKey(const uint8_t* der, size_t len, uint8_t priv32[32]) {
    Tlv seq;
    if (!readTlv(der, der + len, seq) || seq.tag != 0x30) {
        return false;
    }
    Tlv version;
    if (!readTlv(seq.val, seq.next, version) || version.tag != 0x02 || version.len != 1 ||
            version.val[0] != 1) {
        return false;
    }
    Tlv key;
    if (!readTlv(version.next, seq.next, key) || key.tag != 0x04 || key.len != 32) {
        return false;
    }
    memcpy(priv32, key.val, 32);
    return true;
}

} // namespace

bool loadDevicePriv32FromDct(uint8_t priv32[32]) {
    uint8_t der[DCT_ALT_DEVICE_PRIVATE_KEY_SIZE];
    // Returns SYSTEM_ERROR_PROTECTED (non-zero) when Device Protection is on (§7.1).
    if (dct_read_app_data_copy(DCT_ALT_DEVICE_PRIVATE_KEY_OFFSET, der, sizeof(der)) != 0) {
        return false;
    }
    const bool ok = parseSec1PrivateKey(der, sizeof(der), priv32);
    memset(der, 0, sizeof(der));
    return ok;
}

bool loadDeviceId12(uint8_t out12[kDeviceIdBytes]) {
    const String id = System.deviceID();
    if (id.length() != 2 * kDeviceIdBytes) {
        return false;
    }
    return parseHex(id.c_str(), id.length(), out12, kDeviceIdBytes);
}

// ---- FlashCounterStore -------------------------------------------------------
namespace {

constexpr size_t kRecordBytes = 16;

void putLe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint64_t getLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

} // namespace

bool FlashCounterStore::loadWatermarks(uint64_t& uplink, uint64_t& downlink) {
    const int fd = open(path_, O_RDONLY);
    if (fd < 0) {
        return false; // no state yet — caller treats both watermarks as 0
    }
    uint8_t buf[kRecordBytes];
    const ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != static_cast<ssize_t>(sizeof(buf))) {
        return false;
    }
    uplink = getLe64(buf);
    downlink = getLe64(buf + 8);
    return true;
}

bool FlashCounterStore::persistWatermarks(uint64_t uplink, uint64_t downlink) {
    const int fd = open(path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }
    uint8_t buf[kRecordBytes];
    putLe64(buf, uplink);
    putLe64(buf + 8, downlink);
    const bool ok = write(fd, buf, sizeof(buf)) == static_cast<ssize_t>(sizeof(buf));
    fsync(fd);
    close(fd);
    return ok;
}

// ---- SecureUdpSession --------------------------------------------------------
bool SecureUdpSession::init(CounterStore& store, uint32_t stride) {
    ready_ = false;
    uECC_set_rng(&ueccRng);

    uint8_t priv32[32];
    if (!loadDevicePriv32FromDct(priv32)) {
        return false;
    }

    uint8_t deviceId[kDeviceIdBytes];
    if (!loadDeviceId12(deviceId)) {
        memset(priv32, 0, sizeof(priv32));
        return false;
    }

    const bool ctxOk = ctx_.init(priv32, kCloudPublicKeyXY, deviceId);
    memset(priv32, 0, sizeof(priv32));
    if (!ctxOk) {
        return false;
    }

    if (!counters_.init(store, stride)) {
        return false;
    }
    ready_ = true;
    return true;
}

Status SecureUdpSession::protectUplink(const uint8_t* payload, size_t payloadLen,
        uint8_t* out, size_t outCap, size_t& frameLenOut) {
    frameLenOut = 0;
    if (!ready_) {
        return Status::NotReady;
    }
    uint64_t counter48 = 0;
    const Status s = counters_.nextUplink(counter48);
    if (s != Status::Ok) {
        return s;
    }
    // The counter is in range by construction, so 0 here can only mean the
    // frame does not fit outCap.
    frameLenOut = ctx_.protectUplink(counter48, payload, payloadLen, out, outCap);
    return frameLenOut > 0 ? Status::Ok : Status::TooLarge;
}

Status SecureUdpSession::verifyDownlink(const uint8_t* datagram, size_t len,
        const uint8_t*& payloadOut, size_t& payloadLenOut) {
    if (!ready_) {
        return Status::NotReady;
    }
    uint64_t counter48 = 0;
    const Status s = ctx_.verifyDownlink(datagram, len, counters_.downlinkFloor(),
            counter48, payloadOut, payloadLenOut);
    if (s != Status::Ok) {
        return s;
    }
    // Advance the replay floor (and stride-persist when crossing the watermark).
    return counters_.acceptDownlink(counter48);
}

} // namespace particle::secure_udp
