// Secure UDP Phase 1 (CDS-UDP-v1) portable protocol: frame codec, SHA-256 /
// HMAC / HKDF, the per-device key context, and the counter/replay state machine
// (spec §5–§7). Mirrors the service's src/secure-frame.ts and is validated
// against constrained-device-service/test-vectors/phase1.json.
//
// Uplink:   KeyId(3) ‖ CounterLow(2, BE) ‖ Payload ‖ Tag(4)
// Downlink:            CounterLow(2, BE) ‖ Payload ‖ Tag(4)
// Tag = HMAC-SHA256(K_dir, KeyId(3) ‖ Counter48(6, BE) ‖ Payload)[0..4)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace particle::secure_udp {

// ---- SHA-256 platform primitive ----------------------------------------------
// The backing implementation lives in secure_udp.cpp (vendored B-Con SHA-256 on
// the host/device). Swapping it to a Device OS SHA-256 means touching only that
// section, not the rest of the library (spec §7.3).
namespace port {

constexpr size_t kSha256DigestBytes = 32;
constexpr size_t kSha256BlockBytes = 64;

struct Sha256 {
    // Opaque storage for the backing implementation's context (no heap).
    alignas(8) uint8_t storage[120];

    void init();
    void update(const uint8_t* data, size_t len);
    void finish(uint8_t out[kSha256DigestBytes]);
};

} // namespace port

// ---- Wire constants ----------------------------------------------------------
constexpr size_t kKeyIdBytes = 3;
constexpr size_t kCounterLowBytes = 2;
constexpr size_t kTagBytes = 4;
constexpr size_t kUplinkOverheadBytes = kKeyIdBytes + kCounterLowBytes + kTagBytes;
constexpr size_t kDownlinkOverheadBytes = kCounterLowBytes + kTagBytes;
constexpr uint64_t kCounter48Max = (1ULL << 48) - 1;
constexpr uint32_t kCounterResyncSpan = 0x10000;
constexpr size_t kDeviceIdBytes = 12;
constexpr size_t kKeyBytes = 32;

constexpr char kHkdfInfoUplink[] = "CDS-UDP-v1 uplink auth";
constexpr char kHkdfInfoDownlink[] = "CDS-UDP-v1 downlink auth";
constexpr size_t kHkdfInfoUplinkLen = sizeof(kHkdfInfoUplink) - 1;   // no NUL on the wire
constexpr size_t kHkdfInfoDownlinkLen = sizeof(kHkdfInfoDownlink) - 1;

// ---- Frame codec -------------------------------------------------------------
// Parsed views point into the caller's datagram buffer; no copies, no heap.
struct ParsedUplink {
    const uint8_t* keyId;
    uint16_t counterLow;
    const uint8_t* payload;
    size_t payloadLen;
    const uint8_t* tag;
};

struct ParsedDownlink {
    uint16_t counterLow;
    const uint8_t* payload;
    size_t payloadLen;
    const uint8_t* tag;
};

bool parseUplink(const uint8_t* buf, size_t len, ParsedUplink& out);
bool parseDownlink(const uint8_t* buf, size_t len, ParsedDownlink& out);

// Return the total frame size written, or 0 if outCap is too small.
size_t encodeUplink(const uint8_t keyId[kKeyIdBytes], uint16_t counterLow,
        const uint8_t* payload, size_t payloadLen,
        const uint8_t tag[kTagBytes], uint8_t* out, size_t outCap);
size_t encodeDownlink(uint16_t counterLow, const uint8_t* payload, size_t payloadLen,
        const uint8_t tag[kTagBytes], uint8_t* out, size_t outCap);

void computeTag(const uint8_t key[kKeyBytes], const uint8_t keyId[kKeyIdBytes],
        uint64_t counter48, const uint8_t* payload, size_t payloadLen,
        uint8_t tagOut[kTagBytes]);

// Constant-time comparison; false on bad tag length or out-of-range counter.
bool verifyTag(const uint8_t key[kKeyBytes], const uint8_t keyId[kKeyIdBytes],
        uint64_t counter48, const uint8_t* payload, size_t payloadLen,
        const uint8_t* tag, size_t tagLen);

// Candidate full-counter reconstruction (§6.2): the unique counter in
// (floor, floor + 65536] whose low 16 bits match counterLow. Tag verification
// confirms the candidate. Returns false on counter exhaustion.
bool reconstructCounter(uint16_t counterLow, uint64_t windowFloor, uint64_t& out);

// ---- HMAC-SHA256 / HKDF-SHA256 -----------------------------------------------
class HmacSha256 {
public:
    void init(const uint8_t* key, size_t keyLen);
    void update(const uint8_t* data, size_t len);
    void finish(uint8_t out[port::kSha256DigestBytes]);

private:
    port::Sha256 inner_;
    uint8_t opad_[port::kSha256BlockBytes];
};

// HKDF-SHA256 (RFC 5869), extract + expand. okmLen must be <= 255 * 32.
void hkdfSha256(const uint8_t* ikm, size_t ikmLen,
        const uint8_t* salt, size_t saltLen,
        const uint8_t* info, size_t infoLen,
        uint8_t* okm, size_t okmLen);

// ---- Per-device key context --------------------------------------------------
// Static-static P-256 ECDH + HKDF-SHA256 (spec §6.1, §7.1). Mirrors the
// server's KeyContextCache derivation; validated against the same test vectors.
class SecureUdpContext {
public:
    // devicePriv32: raw P-256 private scalar (from the SEC1 DER key).
    // cloudPub64:   raw X‖Y public point (no 0x04 prefix), micro-ecc layout.
    // deviceId12:   full 12-byte device ID (HKDF salt).
    // Call uECC_set_rng() before init for side-channel hardening of the
    // point multiplication. Returns false if the public key or ECDH is invalid.
    bool init(const uint8_t devicePriv32[32], const uint8_t cloudPub64[64],
            const uint8_t deviceId12[kDeviceIdBytes]);

    // Encodes an authenticated uplink for the given counter (allocated by the
    // caller's StridedCounters). Returns the frame size, or 0 if outCap is too
    // small or the counter is out of range. No heap.
    size_t protectUplink(uint64_t counter48, const uint8_t* payload, size_t payloadLen,
            uint8_t* out, size_t outCap) const;

    // Verifies a downlink datagram (counterLow ‖ payload ‖ tag) against the
    // device's own Key Identifier and kDown, reconstructing the counter above
    // windowFloor (§6.2/§6.3). On success, payloadOut points into datagram.
    // The caller advances the floor via StridedCounters::acceptDownlink.
    bool verifyDownlink(const uint8_t* datagram, size_t len, uint64_t windowFloor,
            uint64_t& counter48Out, const uint8_t*& payloadOut, size_t& payloadLenOut) const;

    const uint8_t* z() const { return z_; }
    const uint8_t* kUp() const { return kUp_; }
    const uint8_t* kDown() const { return kDown_; }
    const uint8_t* keyId() const { return keyId_; }
    const uint8_t* deviceId() const { return deviceId_; }

private:
    uint8_t z_[32] = { 0 };
    uint8_t kUp_[kKeyBytes] = { 0 };
    uint8_t kDown_[kKeyBytes] = { 0 };
    uint8_t keyId_[kKeyIdBytes] = { 0 };
    uint8_t deviceId_[kDeviceIdBytes] = { 0 };
};

// ---- Counter / replay state --------------------------------------------------
// Persistence port for counter watermarks (spec §6.2). Sim: JSON state file;
// device: flash-file-backed implementation (FlashCounterStore).
class CounterStore {
public:
    // Returns false if no state exists yet (treat both watermarks as 0).
    virtual bool loadWatermarks(uint64_t& uplink, uint64_t& downlink) = 0;
    virtual bool persistWatermarks(uint64_t uplink, uint64_t downlink) = 0;

protected:
    ~CounterStore() = default;
};

// Stride rule (§6.2): watermarks are persisted one stride ahead of use, so a
// restart resumes at or above anything used or accepted before. After a reboot
// the uplink counter resumes one stride PAST the watermark — the reboot
// signature that tells the server to jump its downlink send counter over our
// restored replay floor, so no downlink is ever discarded by it.
class StridedCounters {
public:
    bool init(CounterStore& store, uint32_t stride);

    // Next uplink send counter (first ever: 1); persists ahead when crossing
    // the watermark. Returns false on persist failure or counter exhaustion.
    bool nextUplink(uint64_t& out);

    // Advance the uplink counter without sending (fault injection / testing).
    void jumpUplink(uint64_t n) { uplinkNext_ += n; }

    // Downlink replay floor: the device keeps a high-water mark only. After a
    // restart it resumes at the persisted watermark, which by construction is
    // above anything accepted before.
    uint64_t downlinkFloor() const { return downlinkFloor_; }

    // Record an accepted downlink counter. False if it does not advance the
    // floor (replay / stale) or persistence fails.
    bool acceptDownlink(uint64_t counter48);

private:
    bool persist();

    CounterStore* store_ = nullptr;
    uint32_t stride_ = 256;
    uint64_t uplinkNext_ = 1;
    uint64_t uplinkWatermark_ = 0;
    uint64_t downlinkFloor_ = 0;
    uint64_t downlinkWatermark_ = 0;
};

} // namespace particle::secure_udp
