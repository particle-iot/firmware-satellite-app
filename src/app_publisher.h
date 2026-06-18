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
// NTN rate limit: a single bucket covers every NTN event (including vitals),
// gated by g_cfg.ntnPublishIntervalS. LTE is not rate-limited at the app
// layer (the cloud enforces its own limits).
//
// NTN size cap: enforced inside the satellite library (MessageChannel checks
// header + body against the configured maxPayloadSize and returns
// Error::TOO_LARGE). The publisher just maps that return code to
// stats_.oversized so the app surfaces oversized failures consistently.
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

    // NTN last-publish timestamp (millis()). 0 = never sent.
    uint32_t ntnLastSendMs_;
};

extern AppPublisher publisher;
