// Device OS port for Secure UDP Phase 1 (CDS-UDP-v1).
//
// Ties the portable SecureUdpContext + StridedCounters (secure_udp.h) to the
// device's own key material (P-256 private key from the DCT), the pinned cloud
// public key, the device ID, and a flash-backed CounterStore. Everything here is
// Device OS specific; the protocol below it is platform-agnostic.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "secure_udp.h"

namespace particle::secure_udp {

// Reads the device's secp256r1 private scalar (raw 32 bytes) from the DCT
// alt-device-key slot via dct_read_app_data_copy (spec §7.1). Returns false if
// the read fails — notably SYSTEM_ERROR_PROTECTED when Device Protection is on.
bool loadDevicePriv32FromDct(uint8_t priv32[32]);

// Fills the 12-byte device ID from System.deviceID() (24 hex chars). Used as
// the HKDF salt and the source of the 3-byte Key Identifier.
bool loadDeviceId12(uint8_t out12[kDeviceIdBytes]);

// CounterStore backed by a flash file on the Device OS user filesystem. The
// stride rule (StridedCounters, §6.2) caps writes to roughly one per `stride`
// datagrams and the filesystem wear-levels, so flash endurance is not a concern
// at the §3 traffic rate. The record is a fixed 16-byte little-endian
// {uplink, downlink} watermark pair.
class FlashCounterStore : public CounterStore {
public:
    explicit FlashCounterStore(const char* path) : path_(path) {}

    bool loadWatermarks(uint64_t& uplink, uint64_t& downlink) override;
    bool persistWatermarks(uint64_t uplink, uint64_t downlink) override;

private:
    const char* path_;
};

// One per device link. Construct once, init() after the radio/key material is
// available, then protect/verify on the datagram path.
class SecureUdpSession {
public:
    // Loads the device private key (DCT) + device ID, derives the per-direction
    // keys against the pinned cloud key, and initializes counters from `store`.
    // Returns false on key-read failure (e.g. Device Protection on), invalid
    // ECDH, or counter init failure. Re-init re-derives.
    bool init(CounterStore& store, uint32_t stride = 256);

    bool ready() const { return ready_; }

    // Wraps an application payload as an authenticated uplink frame into `out`
    // (KeyId ‖ CounterLow ‖ Payload ‖ Tag). Returns the frame length, or 0 on
    // counter exhaustion, persist failure, or outCap too small.
    size_t protectUplink(const uint8_t* payload, size_t payloadLen,
            uint8_t* out, size_t outCap);

    // Verifies a received downlink datagram and, on success, advances the replay
    // floor. `payloadOut` points into `datagram`. Returns false (drop) on parse
    // failure, bad tag, or replay/stale counter.
    bool verifyDownlink(const uint8_t* datagram, size_t len,
            const uint8_t*& payloadOut, size_t& payloadLenOut);

    const SecureUdpContext& context() const { return ctx_; }

private:
    SecureUdpContext ctx_;
    StridedCounters counters_;
    bool ready_ = false;
};

} // namespace particle::secure_udp
