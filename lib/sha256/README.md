# sha256 (vendored)

Third-party public-domain SHA-256, vendored verbatim from
[B-Con/crypto-algorithms](https://github.com/B-Con/crypto-algorithms). Backs the
`crypto_port` SHA-256 primitive in `lib/secure-udp` (HMAC/HKDF for Secure UDP
Phase 1).

The sources are kept byte-identical and live under `src/bcon/` so they are
included as `"bcon/sha256.h"`. The `bcon/` qualifier is required: Device OS ships
its own `services/inc/sha256.h` earlier on the include path, so a bare
`#include "sha256.h"` from outside this directory would resolve to the wrong
header. `lib/secure-udp/src/secure_udp.cpp` includes it path-qualified.
