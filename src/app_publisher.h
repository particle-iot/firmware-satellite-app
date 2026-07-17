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

#include "satellite.h"
#include "modem_manager.h"

// Unified publish API for the LTE and NTN stacks.
//
// Routing is driven by the active radio (ModemManager::radioEnabled()), not by
// whichever stack happens to be connected. If the active radio is not
// connected, the call is dropped and counted - the publisher never falls back
// to the other stack. Mode switching is owned by the app state machine.
//
// Exception: when g_cfg.constrainedProtocolOnCellular is set, the cellular
// branch routes through the constrained protocol (CloudProtocol/secure-UDP
// datagrams) instead of Particle.publish, so the NTN wire path can be tested
// over the normal connection. The NTN rate limit and size cap apply, results
// count in the ntn* stats (ntnOk then means "handed to CloudProtocol/UDP",
// not AT-accepted), and there is still no fallback: if the cellular transport
// is not up, the publish is dropped and counted.
//
// NTN rate limit: a single bucket covers every constrained-protocol event
// (including vitals) regardless of transport, gated by
// g_cfg.ntnPublishIntervalS. LTE Particle.publish is not rate-limited at the
// app layer (the cloud enforces its own limits).
//
// NTN size cap: enforced inside the satellite library (MessageChannel checks
// header + body against the on-wire cap minus the Secure UDP frame overhead
// and returns Error::TOO_LARGE). The publisher maps that return code to
// stats_.oversized, and the secure layer's distinct failures (FLASH_IO,
// OUT_OF_RANGE) to their own counters, so remediation signals stay separate.
//
// Success counters reflect the synchronous return from the underlying stack:
// for NTN, that is "AT command accepted by the modem", NOT cloud delivery.
// See README/docs for the distinction.
class AppPublisher {
public:
    struct Stats {
        uint32_t lteOk       = 0;
        uint32_t lteFail     = 0;
        uint32_t ntnOk       = 0; // AT-accepted, not end-to-end ack
        uint32_t ntnFail     = 0;
        uint32_t dropped     = 0; // no radio connected at publish time
        uint32_t oversized   = 0; // library returned Error::TOO_LARGE
        uint32_t rateLimited = 0; // bucket gap not yet elapsed
        uint32_t unknownEvent = 0; // name not in kEvents (used kDefaultNtnEventCode on NTN)
        // Secure-UDP failures, split from ntnFail: a flash fault or an
        // exhausted counter needs different remediation than a radio problem.
        uint32_t persistFailed = 0;    // counter watermark write failed (FLASH_IO)
        uint32_t counterExhausted = 0; // 48-bit uplink counter space used up (OUT_OF_RANGE)
    };

    AppPublisher(particle::Satellite& sat, particle::ModemManager& modem);

    // Returns 0 on accepted send (LTE) / AT-accepted send (NTN). Negative on
    // any rejection; the reason is reflected in stats().
    int publish(const char* name, const particle::Variant& data);

    const Stats& stats() const { return stats_; }
    void logStats() const;

private:
    particle::Satellite&    sat_;
    particle::ModemManager& modem_;
    Stats                   stats_;

    // Constrained-protocol last-publish timestamp (millis()). 0 = never sent.
    // One bucket across radio switches - the cloud-side pacing assumes a
    // single device-wide cadence.
    uint32_t ntnLastSendMs_;

    // Constrained-protocol send path shared by the satellite branch and the
    // CONSTRAINED_PROTOCOL_ON_CELLULAR cellular branch: rate-limit gate,
    // sat_.publish(), result-to-stats mapping. `via` labels the transport in
    // logs.
    int publishConstrained(const char* name, uint8_t code,
        const particle::Variant& data, const char* via);
};

extern AppPublisher publisher;
