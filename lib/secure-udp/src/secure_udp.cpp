#include "secure_udp.h"

#include <string.h>

#include "uECC.h"

extern "C" {
// Path-qualified to disambiguate from Device OS's own services/inc/sha256.h.
#include "bcon/sha256.h"
}

namespace particle::secure_udp {

// ---- SHA-256 platform primitive (vendored B-Con backing) ---------------------
namespace port {

static_assert(sizeof(SHA256_CTX) <= sizeof(Sha256::storage), "storage too small for SHA256_CTX");
static_assert(alignof(SHA256_CTX) <= 8, "storage under-aligned for SHA256_CTX");

void Sha256::init() {
    sha256_init(reinterpret_cast<SHA256_CTX*>(storage));
}

void Sha256::update(const uint8_t* data, size_t len) {
    if (len > 0) {
        sha256_update(reinterpret_cast<SHA256_CTX*>(storage), data, len);
    }
}

void Sha256::finish(uint8_t out[kSha256DigestBytes]) {
    sha256_final(reinterpret_cast<SHA256_CTX*>(storage), out);
}

} // namespace port

// ---- Frame codec -------------------------------------------------------------
bool parseUplink(const uint8_t* buf, size_t len, ParsedUplink& out) {
    if (buf == nullptr || len < kUplinkOverheadBytes) {
        return false;
    }
    out.keyId = buf;
    out.counterLow = static_cast<uint16_t>((buf[kKeyIdBytes] << 8) | buf[kKeyIdBytes + 1]);
    out.payload = buf + kKeyIdBytes + kCounterLowBytes;
    out.payloadLen = len - kUplinkOverheadBytes;
    out.tag = buf + len - kTagBytes;
    return true;
}

bool parseDownlink(const uint8_t* buf, size_t len, ParsedDownlink& out) {
    if (buf == nullptr || len < kDownlinkOverheadBytes) {
        return false;
    }
    out.counterLow = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    out.payload = buf + kCounterLowBytes;
    out.payloadLen = len - kDownlinkOverheadBytes;
    out.tag = buf + len - kTagBytes;
    return true;
}

size_t encodeUplink(const uint8_t keyId[kKeyIdBytes], uint16_t counterLow,
        const uint8_t* payload, size_t payloadLen,
        const uint8_t tag[kTagBytes], uint8_t* out, size_t outCap) {
    const size_t total = kUplinkOverheadBytes + payloadLen;
    if (outCap < total) {
        return 0;
    }
    memcpy(out, keyId, kKeyIdBytes);
    out[kKeyIdBytes] = static_cast<uint8_t>(counterLow >> 8);
    out[kKeyIdBytes + 1] = static_cast<uint8_t>(counterLow & 0xff);
    if (payloadLen > 0) {
        memcpy(out + kKeyIdBytes + kCounterLowBytes, payload, payloadLen);
    }
    memcpy(out + total - kTagBytes, tag, kTagBytes);
    return total;
}

size_t encodeDownlink(uint16_t counterLow, const uint8_t* payload, size_t payloadLen,
        const uint8_t tag[kTagBytes], uint8_t* out, size_t outCap) {
    const size_t total = kDownlinkOverheadBytes + payloadLen;
    if (outCap < total) {
        return 0;
    }
    out[0] = static_cast<uint8_t>(counterLow >> 8);
    out[1] = static_cast<uint8_t>(counterLow & 0xff);
    if (payloadLen > 0) {
        memcpy(out + kCounterLowBytes, payload, payloadLen);
    }
    memcpy(out + total - kTagBytes, tag, kTagBytes);
    return total;
}

void computeTag(const uint8_t key[kKeyBytes], const uint8_t keyId[kKeyIdBytes],
        uint64_t counter48, const uint8_t* payload, size_t payloadLen,
        uint8_t tagOut[kTagBytes]) {
    uint8_t counter[6];
    for (int i = 5; i >= 0; --i) {
        counter[i] = static_cast<uint8_t>(counter48 & 0xff);
        counter48 >>= 8;
    }
    uint8_t mac[port::kSha256DigestBytes];
    HmacSha256 hmac;
    hmac.init(key, kKeyBytes);
    hmac.update(keyId, kKeyIdBytes);
    hmac.update(counter, sizeof(counter));
    hmac.update(payload, payloadLen);
    hmac.finish(mac);
    memcpy(tagOut, mac, kTagBytes);
}

bool verifyTag(const uint8_t key[kKeyBytes], const uint8_t keyId[kKeyIdBytes],
        uint64_t counter48, const uint8_t* payload, size_t payloadLen,
        const uint8_t* tag, size_t tagLen) {
    if (tagLen != kTagBytes || counter48 > kCounter48Max) {
        return false;
    }
    uint8_t expected[kTagBytes];
    computeTag(key, keyId, counter48, payload, payloadLen, expected);
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < kTagBytes; ++i) {
        diff = static_cast<uint8_t>(diff | (expected[i] ^ tag[i]));
    }
    return diff == 0;
}

bool reconstructCounter(uint16_t counterLow, uint64_t windowFloor, uint64_t& out) {
    if (windowFloor > kCounter48Max) {
        return false;
    }
    const uint64_t base = windowFloor - (windowFloor % kCounterResyncSpan);
    uint64_t candidate = base + counterLow;
    if (candidate <= windowFloor) {
        candidate += kCounterResyncSpan;
    }
    if (candidate > kCounter48Max) {
        return false;
    }
    out = candidate;
    return true;
}

// ---- HMAC-SHA256 / HKDF-SHA256 -----------------------------------------------
void HmacSha256::init(const uint8_t* key, size_t keyLen) {
    uint8_t blockKey[port::kSha256BlockBytes];
    memset(blockKey, 0, sizeof(blockKey));
    if (keyLen > sizeof(blockKey)) {
        port::Sha256 hash;
        hash.init();
        hash.update(key, keyLen);
        hash.finish(blockKey);
    } else {
        memcpy(blockKey, key, keyLen);
    }

    uint8_t ipad[port::kSha256BlockBytes];
    for (size_t i = 0; i < sizeof(blockKey); ++i) {
        ipad[i] = static_cast<uint8_t>(blockKey[i] ^ 0x36);
        opad_[i] = static_cast<uint8_t>(blockKey[i] ^ 0x5c);
    }

    inner_.init();
    inner_.update(ipad, sizeof(ipad));
}

void HmacSha256::update(const uint8_t* data, size_t len) {
    inner_.update(data, len);
}

void HmacSha256::finish(uint8_t out[port::kSha256DigestBytes]) {
    uint8_t innerDigest[port::kSha256DigestBytes];
    inner_.finish(innerDigest);

    port::Sha256 outer;
    outer.init();
    outer.update(opad_, sizeof(opad_));
    outer.update(innerDigest, sizeof(innerDigest));
    outer.finish(out);
}

void hkdfSha256(const uint8_t* ikm, size_t ikmLen,
        const uint8_t* salt, size_t saltLen,
        const uint8_t* info, size_t infoLen,
        uint8_t* okm, size_t okmLen) {
    // Extract
    uint8_t prk[port::kSha256DigestBytes];
    HmacSha256 extract;
    extract.init(salt, saltLen);
    extract.update(ikm, ikmLen);
    extract.finish(prk);

    // Expand
    uint8_t t[port::kSha256DigestBytes];
    size_t tLen = 0;
    uint8_t blockCounter = 1;
    size_t pos = 0;
    while (pos < okmLen) {
        HmacSha256 expand;
        expand.init(prk, sizeof(prk));
        expand.update(t, tLen);
        expand.update(info, infoLen);
        expand.update(&blockCounter, 1);
        expand.finish(t);
        tLen = sizeof(t);

        const size_t n = (okmLen - pos < sizeof(t)) ? (okmLen - pos) : sizeof(t);
        memcpy(okm + pos, t, n);
        pos += n;
        ++blockCounter;
    }
}

// ---- Per-device key context --------------------------------------------------
bool SecureUdpContext::init(const uint8_t devicePriv32[32], const uint8_t cloudPub64[64],
        const uint8_t deviceId12[kDeviceIdBytes]) {
    const uECC_Curve curve = uECC_secp256r1();
    if (!uECC_valid_public_key(cloudPub64, curve)) {
        return false;
    }
    if (!uECC_shared_secret(cloudPub64, devicePriv32, z_, curve)) {
        return false;
    }

    memcpy(deviceId_, deviceId12, kDeviceIdBytes);
    memcpy(keyId_, deviceId12 + kDeviceIdBytes - kKeyIdBytes, kKeyIdBytes);

    hkdfSha256(z_, sizeof(z_), deviceId_, kDeviceIdBytes,
            reinterpret_cast<const uint8_t*>(kHkdfInfoUplink), kHkdfInfoUplinkLen,
            kUp_, sizeof(kUp_));
    hkdfSha256(z_, sizeof(z_), deviceId_, kDeviceIdBytes,
            reinterpret_cast<const uint8_t*>(kHkdfInfoDownlink), kHkdfInfoDownlinkLen,
            kDown_, sizeof(kDown_));
    return true;
}

Status SecureUdpContext::verifyDownlink(const uint8_t* datagram, size_t len, uint64_t windowFloor,
        uint64_t& counter48Out, const uint8_t*& payloadOut, size_t& payloadLenOut) const {
    ParsedDownlink parsed;
    if (!parseDownlink(datagram, len, parsed)) {
        return Status::Malformed;
    }
    uint64_t counter48 = 0;
    if (!reconstructCounter(parsed.counterLow, windowFloor, counter48)) {
        return Status::CounterExhausted;
    }
    if (!verifyTag(kDown_, keyId_, counter48, parsed.payload, parsed.payloadLen,
            parsed.tag, kTagBytes)) {
        return Status::BadTag;
    }
    counter48Out = counter48;
    payloadOut = parsed.payload;
    payloadLenOut = parsed.payloadLen;
    return Status::Ok;
}

size_t SecureUdpContext::protectUplink(uint64_t counter48, const uint8_t* payload,
        size_t payloadLen, uint8_t* out, size_t outCap) const {
    if (counter48 == 0 || counter48 > kCounter48Max) {
        return 0;
    }
    uint8_t tag[kTagBytes];
    computeTag(kUp_, keyId_, counter48, payload, payloadLen, tag);
    return encodeUplink(keyId_, static_cast<uint16_t>(counter48 % kCounterResyncSpan),
            payload, payloadLen, tag, out, outCap);
}

// ---- Counter / replay state --------------------------------------------------
bool StridedCounters::init(CounterStore& store, uint32_t stride) {
    store_ = &store;
    stride_ = stride;
    uint64_t uplink = 0;
    uint64_t downlink = 0;
    const bool hadState = store.loadWatermarks(uplink, downlink);
    uplinkWatermark_ = uplink;
    downlinkWatermark_ = downlink;
    // Reboot resynchronization (§6.2): resume the uplink counter one stride
    // PAST the watermark, so the first post-reboot uplink shows a gap >= S —
    // the deterministic reboot signature the server uses to jump its downlink
    // send counter over our restored replay floor. Skipped counters are free.
    // On the very first boot (no persisted state) there is nothing to signal;
    // counter 0 is never sent (it cannot be reconstructed above a 0 floor), so
    // the first uplink is 1.
    uplinkNext_ = hadState ? uplink + stride_ : 1;
    downlinkFloor_ = downlink;
    return true;
}

Status StridedCounters::nextUplink(uint64_t& out) {
    if (store_ == nullptr) {
        return Status::NotReady;
    }
    if (uplinkNext_ > kCounter48Max) {
        return Status::CounterExhausted;
    }
    // Persist before committing the new watermark to RAM: a failed persist
    // must leave state unchanged so the next call retries the write instead
    // of handing out counters no persisted watermark covers.
    if (uplinkNext_ >= uplinkWatermark_) {
        const uint64_t watermark = uplinkNext_ + stride_;
        if (!store_->persistWatermarks(watermark, downlinkWatermark_)) {
            return Status::PersistFailed;
        }
        uplinkWatermark_ = watermark;
    }
    out = uplinkNext_++;
    return Status::Ok;
}

Status StridedCounters::acceptDownlink(uint64_t counter48) {
    if (store_ == nullptr) {
        return Status::NotReady;
    }
    if (counter48 <= downlinkFloor_) {
        return Status::Replay;
    }
    // Same persist-then-commit rule: on failure the floor stays put and the
    // caller drops the (undelivered) datagram, so the cloud's retransmission
    // can be accepted once storage recovers.
    if (counter48 >= downlinkWatermark_) {
        const uint64_t watermark = counter48 + stride_;
        if (!store_->persistWatermarks(uplinkWatermark_, watermark)) {
            return Status::PersistFailed;
        }
        downlinkWatermark_ = watermark;
    }
    downlinkFloor_ = counter48;
    return Status::Ok;
}

} // namespace particle::secure_udp
