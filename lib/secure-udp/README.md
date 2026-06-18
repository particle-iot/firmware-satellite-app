# secure-udp

Device-side implementation of **Secure UDP Phase 1 (CDS-UDP-v1)** — origin
authentication, integrity, and replay protection for the constrained-device UDP
path, payload in cleartext. See `secure-udp-spec.md` (§5–§7, §9) in the
`constrained-device-service` repo for the normative protocol.

## What this is

This library holds **only our code** — the portable protocol plus the Device OS
port. The third-party crypto it depends on lives in its own sibling libraries so
vendored code is never mixed with implementation:

- `../micro-ecc/` — vendored micro-ecc (P-256 ECDH).
- `../sha256/` — vendored B-Con SHA-256.

Particle puts every `lib/*/src` on the global include path, so `secure_udp.cpp`'s
`#include "uECC.h"` and `#include "bcon/sha256.h"` resolve against those sibling
libraries.

### Files

- `secure_udp.{h,cpp}` — the portable protocol: frame codec, the SHA-256 /
  HMAC-SHA256 / HKDF-SHA256 primitives, the ECDH+HKDF `SecureUdpContext`, and the
  `StridedCounters` replay state machine.
- `secure_udp_session.{h,cpp}` — the Device OS port: device key from the DCT,
  device ID from `System.deviceID()`, the micro-ecc HAL-RNG callback, the
  `FlashCounterStore`, and the `SecureUdpSession` façade.
- `secure_udp_cloud_key.h` — the pinned cloud public key (generated; see below).

The logic mirrors the proof-of-concept in `constrained-device-service/firmware/lib`
(validated against `constrained-device-service/test-vectors/phase1.json`) and the
server's `src/secure-frame.ts`, but is written in this repo's house style rather
than copied verbatim.

## Keeping in sync with the protocol

The wire protocol's source of truth is the spec and the POC/server in
`constrained-device-service`. Any protocol change must land there (and in the
shared `test-vectors/phase1.json`) and be hand-ported here. The third-party crypto
and its Particle-specific adaptations are documented in `../micro-ecc/README.md`
and `../sha256/README.md`.

## Pinned cloud key

`src/secure_udp_cloud_key.h` holds the dedicated cloud P-256 public key as a raw
64-byte X‖Y point. It is the protocol's source-of-truth key from the service
repo. Regenerate it from the canonical SPKI PEM with:

```
scripts/gen-cloud-key-header.sh <cloud-pub.pem>
```

## Phase 1 prerequisite

Phase 1 reads the device private key from the DCT, which requires **Device
Protection to be OFF** (spec §7.1). Protected devices return
`SYSTEM_ERROR_PROTECTED` and must wait for Phase 2.
