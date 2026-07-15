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

#include "app_config.h"

namespace {

static const int NTN_PUBLISH_INTERVAL_MIN_S = 30;

Logger cfgLog("app.cfg");

// Each setting is sourced from a build-time environment variable defined in the
// top-level `env.json`. Workbench builds these into the application
// binary; we read them back with System.getEnv() at boot.
//
// The apply*Env() helpers all share the same contract: they overwrite `dest`
// only when the variable is present AND parses to a valid value of the right
// type, leaving the compiled default in place otherwise. A present-but-invalid
// value is logged and ignored.

// Bool: the getEnv(bool&) overload validates the value is exactly "true" or
// "false" and only writes on success.
void applyBoolEnv(const char* key, bool& dest) {
    bool v = false;
    if (System.getEnv(key, v)) {
        dest = v;
    } else if (System.hasEnv(key)) {
        cfgLog.warn("env '%s' is not 'true'/'false'; using default", key);
    }
}

// Unsigned: the getEnv(int&) overload validates a signed 32-bit decimal integer
// and only writes on success. We additionally reject negatives.
void applyU32Env(const char* key, uint32_t& dest) {
    int v = 0;
    if (System.getEnv(key, v)) {
        if (v < 0) {
            cfgLog.warn("env '%s' is negative; using default", key);
            return;
        }
        dest = static_cast<uint32_t>(v);
    } else if (System.hasEnv(key)) {
        cfgLog.warn("env '%s' is not a valid integer; using default", key);
    }
}

constexpr const char* kEnvLocationFixed = "PARTICLE_LOCATION_FIXED";

// Returns true only when the env var is present AND parses to valid coordinates.
bool applyFixedLocationEnv() {
    String val;
    if (!System.getEnv(kEnvLocationFixed, val)) {
        return false;
    }
    double lat = 0, lon = 0, alt = 0;
    if (sscanf(val.c_str(), "%lf,%lf,%lf", &lat, &lon, &alt) != 3) {
        cfgLog.warn("env '%s'='%s' not in '<lat>,<lon>,<alt>' form; using defaults",
            kEnvLocationFixed, val.c_str());
        return false;
    }
    g_cfg.locFixedLatitude = lat;
    g_cfg.locFixedLongitude = lon;
    g_cfg.locFixedAltitude = alt;
    cfgLog.info("env '%s' sets fixed location: (%f, %f, %f)",
        kEnvLocationFixed, lat, lon, alt);
    return true;
}
} // namespace


// Defaults so the device boots correctly even if no env vars are set.
AppConfig g_cfg = {
    /* lteEnabled                     */ false,
    /* ntnEnabled                     */ true,
    /* initialOnlineTimeoutS          */ 0,
    /* startOnCellular                */ false,
    /* constrainedProtocolOnCellular  */ false,
    /* ltePublishIntervalS            */ 60,
    /* ntnPublishIntervalS            */ 3 * 60,
    /* vitalsIntervalS                */ 10 * 60,
    /* ntnMaxPayloadSize              */ 256,
    /* cellularDisconnectedTimeoutS   */ 10 * 60,
    /* satelliteConnectedTimeoutS     */ 10 * 60,
    /* satelliteDisconnectedTimeoutS  */ 10 * 60,
    /* forceCellularToSatelliteSwitch */ false,
    /* forceSatelliteToCellularSwitch */ false,
    /* forceC2sSwitchTimeoutS         */ 5 * 60,
    /* forceS2cSwitchTimeoutS         */ 5 * 60,
    /* useOnboardGnssForLocation      */ false,
    /* onboardGnssFixTimeoutS         */ 5 * 60,
    /* locFixedLatitude               */ 44.92653,
    /* locFixedLongitude              */ -93.39767,
    /* locFixedAltitude               */ 283,
};


void loadAppConfig() {
    // Source every setting from its environment variable (env.json). Each helper
    // leaves the compiled default in place when the variable is absent or
    // invalid, so the device always boots with a usable configuration.
    applyU32Env      ("INITIAL_ONLINE_TIMEOUT_S",           g_cfg.initialOnlineTimeoutS);
    applyBoolEnv     ("FEATURE_LTE_ENABLED",                g_cfg.lteEnabled);
    applyBoolEnv     ("FEATURE_NTN_ENABLED",                g_cfg.ntnEnabled);
    applyBoolEnv     ("START_ON_CELLULAR",                  g_cfg.startOnCellular);
    applyBoolEnv     ("CONSTRAINED_PROTOCOL_ON_CELLULAR",   g_cfg.constrainedProtocolOnCellular);
    applyU32Env      ("LTE_PUBLISH_INTERVAL_S",             g_cfg.ltePublishIntervalS);
    applyU32Env      ("NTN_PUBLISH_INTERVAL_S",             g_cfg.ntnPublishIntervalS);
    applyU32Env      ("VITALS_INTERVAL_S",                  g_cfg.vitalsIntervalS);
    applyU32Env      ("NTN_MAX_PAYLOAD_SIZE",               g_cfg.ntnMaxPayloadSize);
    applyU32Env      ("CELLULAR_DISCONNECTED_TIMEOUT_S",    g_cfg.cellularDisconnectedTimeoutS);
    applyU32Env      ("SATELLITE_CONNECTED_TIMEOUT_S",      g_cfg.satelliteConnectedTimeoutS);
    applyU32Env      ("SATELLITE_DISCONNECTED_TIMEOUT_S",   g_cfg.satelliteDisconnectedTimeoutS);
    applyBoolEnv     ("FORCE_CELLULAR_TO_SATELLITE_SWITCH", g_cfg.forceCellularToSatelliteSwitch);
    applyBoolEnv     ("FORCE_SATELLITE_TO_CELLULAR_SWITCH", g_cfg.forceSatelliteToCellularSwitch);
    applyU32Env      ("FORCE_C2S_SWITCH_TIMEOUT_S",         g_cfg.forceC2sSwitchTimeoutS);
    applyU32Env      ("FORCE_S2C_SWITCH_TIMEOUT_S",         g_cfg.forceS2cSwitchTimeoutS);
    applyBoolEnv     ("USE_ONBOARD_GNSS_FOR_LOCATION",      g_cfg.useOnboardGnssForLocation);
    applyU32Env      ("ONBOARD_GNSS_FIX_TIMEOUT_S",         g_cfg.onboardGnssFixTimeoutS);

    // Fixed coordinates come as a single "lat,lon,alt" value (decimal degrees /
    // metres). Used directly when GNSS is disabled, and as the fallback when the
    // onboard GNSS engine fails to get a fix.
    const bool haveFixedLocation = applyFixedLocationEnv();

    // When GNSS is disabled the fixed coords are the device's only location, so
    // warn if they were not supplied.
    if (!g_cfg.useOnboardGnssForLocation && !haveFixedLocation) {
        cfgLog.warn("USE_ONBOARD_GNSS_FOR_LOCATION is false but '%s' is missing or invalid; "
            "using compiled default coordinates (%f, %f, %f)",
            kEnvLocationFixed,
            g_cfg.locFixedLatitude, g_cfg.locFixedLongitude, g_cfg.locFixedAltitude);
    }

    // At least one stack must be enabled.
    if (!g_cfg.lteEnabled && !g_cfg.ntnEnabled) {
        cfgLog.error("both LTE and NTN disabled; re-enabling LTE to keep the device usable");
        g_cfg.lteEnabled = true;
    }

    if (g_cfg.ntnPublishIntervalS < NTN_PUBLISH_INTERVAL_MIN_S) {
        cfgLog.warn("NTN publish interval %lus is below the NTN allowed floor of 30s, raising it to 30s",
            (unsigned long)g_cfg.ntnPublishIntervalS);
        g_cfg.ntnPublishIntervalS = NTN_PUBLISH_INTERVAL_MIN_S;
    }

    cfgLog.info("App config:");
    cfgLog.info("  lteEnabled=%s ntnEnabled=%s startOnCellular=%s constrainedProtoOnCell=%s initialOnlineTimeoutS=%lus",
        g_cfg.lteEnabled ? "true" : "false",
        g_cfg.ntnEnabled ? "true" : "false",
        g_cfg.startOnCellular ? "true" : "false",
        g_cfg.constrainedProtocolOnCellular ? "true" : "false",
        (unsigned long)g_cfg.initialOnlineTimeoutS);
    cfgLog.info("  publish: lte=%lus ntn=%lus vitals=%lus ntnMaxBytes=%lu",
        (unsigned long)g_cfg.ltePublishIntervalS,
        (unsigned long)g_cfg.ntnPublishIntervalS,
        (unsigned long)g_cfg.vitalsIntervalS,
        (unsigned long)g_cfg.ntnMaxPayloadSize);
    cfgLog.info("  switch timeouts: cellDis=%lus satCon=%lus satDis=%lus",
        (unsigned long)g_cfg.cellularDisconnectedTimeoutS,
        (unsigned long)g_cfg.satelliteConnectedTimeoutS,
        (unsigned long)g_cfg.satelliteDisconnectedTimeoutS);
    cfgLog.info("  force: c2s=%s(%lus) s2c=%s(%lus)",
        g_cfg.forceCellularToSatelliteSwitch ? "true" : "false",
        (unsigned long)g_cfg.forceC2sSwitchTimeoutS,
        g_cfg.forceSatelliteToCellularSwitch ? "true" : "false",
        (unsigned long)g_cfg.forceS2cSwitchTimeoutS);
    cfgLog.info("  loc: useOnboardGnss=%s timeout=%lus fixed=(%f, %f, %f)",
        g_cfg.useOnboardGnssForLocation ? "true" : "false",
        (unsigned long)g_cfg.onboardGnssFixTimeoutS,
        g_cfg.locFixedLatitude, g_cfg.locFixedLongitude, g_cfg.locFixedAltitude);
}
