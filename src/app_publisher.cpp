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

#include "app_publisher.h"

#include "app_config.h"
#include "app_events.h"

namespace {
Logger pubLog("app.pub");

// Returns true if `now` is at least `gapMs` past `lastMs`. Treats lastMs == 0
// (never sent) as "always allowed". Safe across millis() wraparound because
// the subtraction is modulo 2^32.
bool gapElapsed(uint32_t lastMs, uint32_t nowMs, uint32_t gapMs) {
    if (lastMs == 0) return true;
    return (nowMs - lastMs) >= gapMs;
}
} // namespace

AppPublisher::AppPublisher(particle::Satellite& sat, particle::ModemManager& modem)
    : sat_(sat),
      modem_(modem),
      stats_(),
      ntnLastSendMs_(0) {
}

int AppPublisher::publish(const char* name, const particle::Variant& data) {
    if (!name) {
        pubLog.error("publish: null event name");
        return SYSTEM_ERROR_INVALID_ARGUMENT;
    }

    // Arbitrary event names are accepted. LTE publishes by name regardless;
    // NTN needs an integer code, so a name not in kEvents falls back to the
    // default code (and is counted so the miss is visible in stats).
    const EventDef* ev = findEvent(name);
    const uint8_t code = ev ? ev->code : kDefaultNtnEventCode;
    if (!ev) {
        ++stats_.unknownEvent;
    }

    const auto radio = modem_.radioEnabled();

    if (radio == RADIO_CELLULAR) {
        if (!Particle.connected()) {
            ++stats_.dropped;
            pubLog.warn("publish '%s': LTE not connected, dropped", name);
            return SYSTEM_ERROR_INVALID_STATE;
        }
        if (g_cfg.constrainedProtocolOnCellular) {
            // Route through the constrained protocol instead of
            // Particle.publish. No fallback: if the UDP transport isn't up,
            // the publish is dropped and counted.
            if (!sat_.cellularTransportActive()) {
                ++stats_.dropped;
                pubLog.warn("publish '%s': constrained-protocol transport not up, dropped", name);
                return SYSTEM_ERROR_INVALID_STATE;
            }
            return publishConstrained(name, code, data, "CP-over-LTE");
        }
        bool ok = Particle.publish(name, data);
        if (ok) {
            ++stats_.lteOk;
            pubLog.info("LTE publish '%s' ok (#%lu)", name, (unsigned long)stats_.lteOk);
            return 0;
        }
        ++stats_.lteFail;
        pubLog.warn("LTE publish '%s' failed (#%lu)", name, (unsigned long)stats_.lteFail);
        return SYSTEM_ERROR_NETWORK;
    }

    if (radio == RADIO_SATELLITE) {
        if (!sat_.connected()) {
            ++stats_.dropped;
            pubLog.warn("publish '%s': NTN not connected, dropped", name);
            return SYSTEM_ERROR_INVALID_STATE;
        }
        return publishConstrained(name, code, data, "NTN");
    }

    // Radio not yet selected.
    ++stats_.dropped;
    pubLog.warn("publish '%s': no radio enabled, dropped", name);
    return SYSTEM_ERROR_INVALID_STATE;
}

int AppPublisher::publishConstrained(const char* name, uint8_t code,
        const particle::Variant& data, const char* via) {
    // Single rate-limit bucket: every constrained-protocol event (including
    // vitals) is gated by the same minimum gap, regardless of transport.
    const uint32_t now = millis();
    const uint32_t gapMs = NTN_PUBLISH_INTERVAL_MIN_S * 1000UL;
    if (!gapElapsed(ntnLastSendMs_, now, gapMs)) {
        ++stats_.rateLimited;
        pubLog.info("%s publish '%s' rate-limited (%lums since last, gap %lums)",
            via, name, (unsigned long)(now - ntnLastSendMs_), (unsigned long)gapMs);
        return SYSTEM_ERROR_LIMIT_EXCEEDED;
    }

    int r = sat_.publish(code, data);
    if (r == 0) {
        ntnLastSendMs_ = now ? now : 1; // avoid the "never sent" sentinel
        ++stats_.ntnOk;
        pubLog.info("%s publish '%s' code=%u accepted (#%lu)",
            via, name, (unsigned)code, (unsigned long)stats_.ntnOk);
        return 0;
    }
    if (r == SYSTEM_ERROR_TOO_LARGE) {
        ++stats_.oversized;
        pubLog.error("%s publish '%s' code=%u rejected as too large",
            via, name, (unsigned)code);
        return r;
    }
    if (r == SYSTEM_ERROR_FLASH_IO) {
        ++stats_.persistFailed;
        pubLog.error("%s publish '%s' code=%u dropped: secure counter persistence failed",
            via, name, (unsigned)code);
        return r;
    }
    if (r == SYSTEM_ERROR_OUT_OF_RANGE) {
        ++stats_.counterExhausted;
        pubLog.error("%s publish '%s' code=%u dropped: secure uplink counter exhausted",
            via, name, (unsigned)code);
        return r;
    }
    ++stats_.ntnFail;
    pubLog.warn("%s publish '%s' code=%u failed: %d (#%lu)",
        via, name, (unsigned)code, r, (unsigned long)stats_.ntnFail);
    return r;
}

void AppPublisher::logStats() const {
    pubLog.info("stats: lte=%lu/%lu ntn=%lu/%lu drop=%lu over=%lu rl=%lu unk=%lu pfail=%lu cex=%lu",
        (unsigned long)stats_.lteOk,
        (unsigned long)(stats_.lteOk + stats_.lteFail),
        (unsigned long)stats_.ntnOk,
        (unsigned long)(stats_.ntnOk + stats_.ntnFail),
        (unsigned long)stats_.dropped,
        (unsigned long)stats_.oversized,
        (unsigned long)stats_.rateLimited,
        (unsigned long)stats_.unknownEvent,
        (unsigned long)stats_.persistFailed,
        (unsigned long)stats_.counterExhausted);
}
