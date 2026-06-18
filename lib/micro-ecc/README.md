# micro-ecc (vendored)

Third-party P-256 (secp256r1) ECDH, vendored verbatim from
[micro-ecc](https://github.com/kmackay/micro-ecc) **v1.1**. BSD 2-clause — see
`LICENSE.txt`. Used by `lib/secure-udp` for the static ECDH in Secure UDP Phase 1.

Two adaptations for the Particle cloud compiler (which only uploads known
source/header extensions, not `.inc`):

- `platform-specific.inc` and `curve-specific.inc` are renamed to `.h`; the two
  `#include`s in `uECC.c` are updated to match.
- `uECC.h` forces `uECC_PLATFORM = uECC_arch_other` (the portable C path), so the
  ARM-assembly `asm_*.inc` files are never reached and have been removed. The
  static ECDH runs once per security context, so the asm speedup is irrelevant,
  and this avoids shipping untested ARMv8-M assembly. Output is unchanged and
  still matches `constrained-device-service/test-vectors/phase1.json`.

To save flash, the unused curves can be dropped via compile flags
(`-DuECC_SUPPORTS_secp160r1=0` etc., `-DuECC_SUPPORT_COMPRESSED_POINT=0`) rather
than editing `uECC.h`.
