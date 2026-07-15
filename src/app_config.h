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

// =============================================================================
// M635E NTN Blueprint Demo - Application Configuration
// =============================================================================
// Build-time configuration sourced from environment variables defined in the
// top-level `env.json` Call
// loadAppConfig() once during setup() before any component that reads g_cfg. If
// a variable is missing or invalid, the default defined in src/app_config.cpp
// is used for that field.
//
// Conventions:
//   *_S values are seconds. Convert at the call site if a Device OS API needs
//   milliseconds (e.g. `g_cfg.fooS * 1000UL`).
// =============================================================================


struct AppConfig {
    // ---- Feature toggles --------------------------------------------------
    // Enable / disable each connectivity stack at runtime. Both stacks are
    // always compiled in; these gates short-circuit the corresponding
    // switch-decision helpers.
    bool lteEnabled;
    bool ntnEnabled;

    // ---- Startup ----------------------------------------------------------
    // If > 0, connect to the Particle cloud once at boot to give the opportunity
    // for the cloud to update the device state and update environment variables.
    uint32_t initialOnlineTimeoutS;

    // true  = boot on Cellular (LTE-M)
    // false = boot on Satellite (NTN). Useful for NTN-first demos.
    bool startOnCellular;

    // ---- Constrained protocol over the normal connection (testing aid) ----
    // When true, app publishes made while on the normal Device OS connection
    // (cellular, or WiFi) are routed through the constrained protocol - the
    // same CloudProtocol / secure-UDP datagram stack used over NTN, sent to
    // the NTN ingress - INSTEAD of Particle.publish. The Particle cloud
    // session stays up (OTA/console keep working) and the NTN rate limit
    // still applies, so the wire behavior matches a satellite-attached
    // device without waiting for an NTN attach. There is no fallback: if the
    // secure-UDP session cannot initialize, publishes fail visibly in stats.
    bool constrainedProtocolOnCellular;

    // ---- Publish timing ---------------------------------------------------
    // Per-stack publish cadence in seconds. Do NOT set the satellite interval
    // below 10 s.
    uint32_t ltePublishIntervalS;
    uint32_t ntnPublishIntervalS;

    // Device vitals (diagnostics) publish cadence in seconds. Vitals are always
    // published once on (re)connect; this controls the periodic refresh
    // afterwards. 0 = on-connect only (no periodic vitals). Shares the active
    // radio's publish path and, on NTN, the single NTN rate-limit bucket.
    uint32_t vitalsIntervalS;

    // ---- NTN payload cap --------------------------------------------------
    // Max ON-WIRE datagram size for outbound NTN publishes, including the
    // Secure UDP frame overhead (9 bytes of KeyId/CounterLow/Tag) when enabled
    // — the protocol frame (header + body) gets the remainder. The satellite
    // library rejects larger frames with Error::TOO_LARGE before any AT
    // traffic, and clamps out-of-range values to the transport maximum of 256
    // (the modem's AT-command body limit: 256 raw bytes = 512 hex chars).
    uint32_t ntnMaxPayloadSize;

    // ---- Radio switching timeouts ----------------------------------------
    // Seconds. It is NOT recommended to set these below 10 minutes (600 s) for
    // production.
    //   cellularDisconnectedTimeoutS : time disconnected on LTE before
    //       switching to Satellite. (There is no cellular "connected" timeout
    //       - if LTE is connected there is no reason to switch.)
    //   satelliteConnectedTimeoutS   : time connected on Satellite contiguously
    //       before switching back to test Cellular again.
    //   satelliteDisconnectedTimeoutS: time disconnected on Satellite contiguously
    //       before switching back to Cellular. Satellite can take a while to connect
    //       - don't set this too low.
    //   satelliteConnectedTimeoutS + satelliteDisconnectedTimeoutS : absolute max
    //       time Satellite radio may be in operation before system will
    //       automatically switch back to Cellular
    //
    // --- For quick debug testing -----------
    //       Set all 3 to 2 minutes each, and disconnect the
    //       antenna to simulate a cellular outage. Reconnect
    //       the antenna after the switch to Satellite.
    // ---------------------------------------
    uint32_t cellularDisconnectedTimeoutS;
    uint32_t satelliteConnectedTimeoutS;
    uint32_t satelliteDisconnectedTimeoutS;

    // ---- Forced switching (testing only) ---------------------------------
    // Set both force flags to false for normal operation. When true, the
    // matching c2s/s2c timeout (seconds) drives the switch decision purely on
    // elapsed time since radio enable, ignoring connection state - useful for
    // exercising switch logic on the bench.
    bool forceCellularToSatelliteSwitch;
    bool forceSatelliteToCellularSwitch;
    uint32_t forceC2sSwitchTimeoutS;
    uint32_t forceS2cSwitchTimeoutS;

    // ---- Location (for NTN locfix) ---------------------------------------
    // NTN attach requires a location. The device programs it on the modem via
    // AT+QNWCFG="ntn_locfix",... before registration.
    //   true  : GNSS antenna present; use the onboard GNSS engine to acquire a
    //           fix, trying for up to onboardGnssFixTimeoutS. If no fix is obtained
    //           the application falls back to the fixed coords below so NTN
    //           attach can still proceed.
    //   false : no GNSS antenna; always use the configured fixed coords. The
    //           GNSS engine is never queried.
    bool      useOnboardGnssForLocation;
    uint32_t  onboardGnssFixTimeoutS;
    double    locFixedLatitude;
    double    locFixedLongitude;
    double    locFixedAltitude;
};


// Global, populated by loadAppConfig(). Reads before loadAppConfig() runs see
// the defaults from src/app_config.cpp.
extern AppConfig g_cfg;

// Populate g_cfg from the `env.json` environment variables. Each missing or
// invalid variable leaves that field's compiled default in place. Call once,
// early in setup().
void loadAppConfig();
