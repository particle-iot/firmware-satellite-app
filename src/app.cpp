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

#include "Particle.h"
#include "satellite.h"
#include "modem_manager.h"
#include "app_config.h"
#include "app_publisher.h"
#include "diag_query/diag_query.h"

SYSTEM_MODE(SEMI_AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_ALL);

Satellite satellite;
ModemManager modem;
AppPublisher publisher(satellite, modem);

// -----------------------------------------------------------------------------
// Application state machine
// -----------------------------------------------------------------------------
enum class AppState {
    Boot,              // init: enable the start radio, reset timers
    CellularConnect,   // Particle.connect() issued; waiting for the cloud
    CellularOnline,    // connected on LTE; publishing on the LTE interval
    AcquireLocation,   // get a GPS fix (or fixed fallback) and set ntn_locfix
    SatelliteConnect,  // satellite.begin()/connect(); waiting for NTN registration
    SatelliteOnline,   // connected on NTN; publishing on the NTN interval
    SwitchToSatellite, // tear down cloud + cellular, enable the Satellite radio
    SwitchToCellular,  // tear down satellite, enable the Cellular radio
    Fault              // error / recovery (modem-reset escalation hook)
};

static AppState appState = AppState::Boot;
static bool stateEntry = true;   // true on the first loop() after a transition
uint32_t stateEnterTime = 0;     // millis() when the current state was entered

uint32_t lastPublish = 0;
uint32_t lastVitals = 0;
uint32_t radioTime = 0;        // millis() when the current radio was enabled
uint32_t connStateSince = 0;   // millis() when the current connected/disconnected state began

// Location used for outgoing publishes (decimal degrees / metres). Populated
// from a GPS fix or the fixed fallback; reused until refreshed.
double pubLat = 0;
double pubLon = 0;
double pubAlt = 0;
bool havePubLoc = false;

const char* stateName(AppState s) {
    switch (s) {
        case AppState::Boot:              return "Boot";
        case AppState::CellularConnect:   return "CellularConnect";
        case AppState::CellularOnline:    return "CellularOnline";
        case AppState::AcquireLocation:   return "AcquireLocation";
        case AppState::SatelliteConnect:  return "SatelliteConnect";
        case AppState::SatelliteOnline:   return "SatelliteOnline";
        case AppState::SwitchToSatellite: return "SwitchToSatellite";
        case AppState::SwitchToCellular:  return "SwitchToCellular";
        case AppState::Fault:             return "Fault";
        default:                          return "Unknown";
    }
}

void transitionTo(AppState next) {
    Log.info("STATE: %s -> %s", stateName(appState), stateName(next));
    appState = next;
    stateEntry = true;
    stateEnterTime = millis();
}

// True only on the first loop() iteration after entering the current state.
// Used to run one-shot entry actions (e.g. begin()/connect()).
bool onEntry() {
    bool e = stateEntry;
    stateEntry = false;
    return e;
}

// Should we leave Cellular for Satellite? Driven by the disconnect timer (or the
// force flag for bench testing). Never true if NTN is disabled in config.
bool cellularShouldSwitchToSatellite() {
    if (!g_cfg.ntnEnabled) {
        return false;
    }
    if (g_cfg.forceCellularToSatelliteSwitch) {
        if (radioTime && (millis() - radioTime > g_cfg.forceC2sSwitchTimeoutS * 1000UL)) {
            Log.warn("SIMULATED LTE failure: FORCE_CELLULAR_TO_SATELLITE_SWITCH tripped after %lus on radio; switching to Satellite",
                (unsigned long)g_cfg.forceC2sSwitchTimeoutS);
            return true;
        }
        return false;
    }
    // Switch only after we have been continuously disconnected on LTE for the
    // timeout. connStateSince resets on connect, so it can't go stale.
    return !Particle.connected() &&
           (millis() - connStateSince > g_cfg.cellularDisconnectedTimeoutS * 1000UL);
}

// Should we leave Satellite for Cellular? We don't camp on Satellite if Cellular
// might be available - go test it again after the connected/disconnected
// timeout. Never true if LTE is disabled in config.
bool satelliteShouldSwitchToCellular() {
    if (!g_cfg.lteEnabled) {
        return false;
    }
    if (g_cfg.forceSatelliteToCellularSwitch) {
        if (radioTime && (millis() - radioTime > g_cfg.forceS2cSwitchTimeoutS * 1000UL)) {
            Log.warn("SIMULATED Satellite failure: FORCE_SATELLITE_TO_CELLULAR_SWITCH tripped after %lus on radio; switching to Cellular",
                (unsigned long)g_cfg.forceS2cSwitchTimeoutS);
            return true;
        }
        return false;
    }
    // Apply the timeout for the state we are actually in, measured from the last
    // connect/disconnect flip: switch after satelliteConnectedTimeoutS connected
    // (periodically retry LTE) or satelliteDisconnectedTimeoutS disconnected.
    uint32_t inStateMs = millis() - connStateSince;
    return satellite.connected()
        ? (inStateMs > g_cfg.satelliteConnectedTimeoutS * 1000UL)
        : (inStateMs > g_cfg.satelliteDisconnectedTimeoutS * 1000UL);
}

// Acquire the location to program into the modem's NTN location fix, then hand
// it to the satellite library. When the onboard GNSS engine is disabled we never
// query GNSS - we pass the configured coords through and tell the library to
// short-circuit any later getGNSSLocation() call (forceFixed=true). When it is
// enabled we try the GNSS engine for up to onboardGnssFixTimeoutS and fall back to the
// fixed coords.
void acquireAndSetLocationFix() {
    double lat = g_cfg.locFixedLatitude;
    double lon = g_cfg.locFixedLongitude;
    double alt = g_cfg.locFixedAltitude;
    const bool forceFixed = !g_cfg.useOnboardGnssForLocation;

    if (!forceFixed) {
        if (satellite.getGNSSLocation(g_cfg.onboardGnssFixTimeoutS * 1000UL) == 0) {
            auto p = satellite.lastPositionInfo();
            lat = p.latitude;
            lon = p.longitude;
            alt = p.altitude;
        } else {
            Log.warn("No GNSS fix; using fixed fallback location");
        }
    }

    pubLat = lat;
    pubLon = lon;
    pubAlt = alt;
    havePubLoc = true;
    satellite.setLocationFix(lat, lon, alt, forceFixed);
}

// -----------------------------------------------------------------------------
// Active radio accessors
// -----------------------------------------------------------------------------

// Human-readable name of the currently enabled radio.
const char* activeProfileName() {
    switch (modem.radioEnabled()) {
        case RADIO_CELLULAR:  return "Cellular";
        case RADIO_SATELLITE: return "Satellite";
        default:              return "None";
    }
}

// Is the currently enabled radio connected?
bool activeRadioConnected() {
    switch (modem.radioEnabled()) {
        case RADIO_CELLULAR:  return Particle.connected();
        case RADIO_SATELLITE: return satellite.connected();
        default:              return false;
    }
}

// Publish interval (seconds) for the currently enabled radio.
uint32_t activePublishIntervalS() {
    return (modem.radioEnabled() == RADIO_SATELLITE) ? g_cfg.ntnPublishIntervalS
                                                     : g_cfg.ltePublishIntervalS;
}

// Human-readable name for a Device OS cellular access technology.
const char* accessTechName(hal_net_access_tech_t rat) {
    switch (rat) {
        case NET_ACCESS_TECHNOLOGY_GSM:         return "GSM";
        case NET_ACCESS_TECHNOLOGY_LTE:         return "LTE";
        case NET_ACCESS_TECHNOLOGY_LTE_CAT_M1:  return "LTE-M";
        case NET_ACCESS_TECHNOLOGY_LTE_CAT_NB1: return "NB-IoT";
        default:                                return "unknown";
    }
}

// -----------------------------------------------------------------------------
// Publish payloads
// -----------------------------------------------------------------------------
// The blueprint calls appPublishData() on every publish tick once the active
// radio is connected. Edit it to publish whatever payloads your application
// needs. Each helper below is an example of the pattern:
//   - any per-tick preparation (e.g. refresh the GNSS fix on cellular)
//   - building a `Variant` payload and calling `publisher.publish(name, v)`

static void publishLocationExample() {
    if (modem.radioEnabled() == RADIO_CELLULAR) {
        if (satellite.getGNSSLocation(g_cfg.onboardGnssFixTimeoutS * 1000UL) == 0) {
            auto p = satellite.lastPositionInfo();
            pubLat = p.latitude;
            pubLon = p.longitude;
            pubAlt = p.altitude;
            havePubLoc = true;
        }
    }

    auto now = (unsigned int)Time.now();
    particle::Variant locEvent;
    locEvent.set("cmd", "loc");
    locEvent.set("time", now);
    particle::Variant locationObject;
        locationObject.set("lck", havePubLoc ? 1 : 0);
        locationObject.set("time", now);
        locationObject.set("lat", pubLat);
        locationObject.set("lon", pubLon);
        locationObject.set("alt", pubAlt);
    locEvent.set("loc", locationObject);

    publisher.publish("loc", locEvent);
}

// Example of an arbitrary named event. "event" is in the kEvents table so it
// maps to that NTN code; any name not in the table falls back to
// kDefaultNtnEventCode.
static void publishEventExample() {
    auto now = (unsigned int)Time.now();
    particle::Variant event;
    event.set("cmd", "test");
    event.set("time", now);

    publisher.publish("event", event);
}

void appPublishData() {
    // publishLocationExample();
    publishEventExample();
    publisher.logStats();
}

// -----------------------------------------------------------------------------
// Device vitals (diagnostics) push
// -----------------------------------------------------------------------------
// Device-initiated vitals: the device gathers Device OS diagnostic sources on
// its own and publishes them, rather than waiting for the cloud to request
// specific IDs. It reuses getDiagnosticValue() - the same query primitive the
// server-pull DiagnosticsRequest path uses - and ships the values as the
// "vitals" event through the unified publisher (NTN code 2). Like any publish
// it is subject to the active radio's path, the single NTN rate-limit bucket,
// and the NTN payload-size cap.
//
// Keep kVitalsIds small: every NTN vitals publish must fit ntn_max_payload_size
// (256 B on-wire) and each entry costs ~key+value bytes after CBOR encoding.
// Only DIAG_TYPE_INT / DIAG_TYPE_UINT sources can be read; others are skipped.
static const uint32_t kVitalsIds[] = {
    DIAG_ID_SYSTEM_UPTIME,                 // sys:uptime (s)
    DIAG_ID_SYSTEM_BATTERY_CHARGE,         // batt:soc (%)
    DIAG_ID_SYSTEM_BATTERY_STATE,          // batt:state
    DIAG_ID_SYSTEM_POWER_SOURCE,           // pwr:src
    DIAG_ID_SYSTEM_FREE_MEMORY,            // mem:free (B)
    DIAG_ID_SYSTEM_LAST_RESET_REASON,      // sys:reset
    DIAG_ID_SYSTEM_VERSION,               // sys:version
    DIAG_ID_SYSTEM_USER_PART_HASH,        // sys:userphash
    DIAG_ID_NETWORK_SIGNAL_STRENGTH,       // net:sigstr (% *100)
    DIAG_ID_NETWORK_SIGNAL_STRENGTH_VALUE, // net:sigstrv (dBm *100)
};

// Gather the configured diagnostic IDs into a Variant map keyed by the numeric
// diag ID (as a string). IDs that cannot be read are skipped.
static particle::Variant collectVitals() {
    particle::Variant diag;
    for (auto id : kVitalsIds) {
        particle::Variant val;
        if (getDiagnosticValue(id, val) == 0) {
            diag.set(String((unsigned)id), val);
        }
    }
    return diag;
}

static void publishVitals() {
    // TODO: This is published as a generic event right now, but constrained device
    // service should be updated to handle these properly as a DIAGNOSTICS message 
    // and processed the same as non NTN vitals. 
    particle::Variant vitals;
    vitals.set("cmd", "vitals");
    vitals.set("time", (unsigned int)Time.now());
    vitals.set("diag", collectVitals());
    publisher.publish("vitals", vitals);
}

static void runPublishTick() {
    bool firstPublish = onEntry();
    uint32_t now = millis();

    // Vitals: always once on (re)connect, then every vitals_interval_s
    bool vitalsDue = g_cfg.vitalsIntervalS > 0 &&
                     (now - lastVitals > g_cfg.vitalsIntervalS * 1000UL);
    if (firstPublish || vitalsDue) {
        publishVitals();
        lastVitals = now;
    }

    if (firstPublish) {
        lastPublish = now;
    } else if (now - lastPublish > activePublishIntervalS() * 1000UL) {
        appPublishData();
        lastPublish = now;
    }
}

// Tracks how long the active radio has been continuously connected or
// disconnected. `connStateSince` is the millis() timestamp of the most recent
// connected<->disconnected flip (or radio switch); the switch-decision helpers
// compare millis() - connStateSince against the configured timeouts. A radio
// switch restarts both this clock and `radioTime` (used by the force paths).
void updateConnectionTimers() {
    static radio_type_t lastRadio = RADIO_UNKNOWN;
    static bool lastConnected = false;

    radio_type_t radio = modem.radioEnabled();
    bool connected = activeRadioConnected();

    if (radio != lastRadio) {
        lastRadio = radio;
        lastConnected = connected;
        radioTime = millis();
        connStateSince = millis();
        return;
    }

    if (connected != lastConnected) {
        lastConnected = connected;
        connStateSince = millis();
    }
}

// Throttled (5 s) device status line: active profile, app state, time in
// state, time until next publish, and the active radio's signal / band. Pass
// force=true to print immediately (e.g. right after a radio switch).
void logStatusLine(bool force = false) {
    static uint32_t lastCheck = millis();
    if (!force && millis() - lastCheck <= 5000) {
        return;
    }
    lastCheck = millis();

    uint32_t timeInStateS = (millis() - stateEnterTime) / 1000UL;

    char line[256];
    size_t off = snprintf(line, sizeof(line), "[%s][%s][Time In State: %lus]",
        activeProfileName(), stateName(appState), (unsigned long)timeInStateS);

    if (off < sizeof(line) && activeRadioConnected()) {
        uint32_t intervalMs = activePublishIntervalS() * 1000UL;
        uint32_t sinceLast = millis() - lastPublish;
        uint32_t untilNextS = (sinceLast >= intervalMs) ? 0 : (intervalMs - sinceLast) / 1000UL;
        off += snprintf(line + off, sizeof(line) - off,
            "[Time Until Next Publish: %lus]", (unsigned long)untilNextS);
    }

    // NTN signal / band, refreshed by satellite.process().
    if (off < sizeof(line) && modem.radioEnabled() == RADIO_SATELLITE) {
        auto c = satellite.servingCellInfo();
        if (c.state[0] != '\0') {
            if (c.valid) {
                off += snprintf(line + off, sizeof(line) - off,
                    "[Sig: %s band=%d earfcn=%d RSRP=%ddBm RSRQ=%ddB RSSI=%ddBm SINR=%d]",
                    c.state, c.band, c.earfcn, c.rsrp, c.rsrq, c.rssi, c.sinr);
            } else {
                off += snprintf(line + off, sizeof(line) - off,
                    "[Sig: %s (acquiring)]", c.state);
            }
        }
    }

    // LTE signal via Device OS (Cellular.RSSI() -> cellular_signal()).
    // Reports access tech, RSRP (strength) and RSRQ (quality) in real units
    // plus 0-100% bars. RSSI/SINR are not exposed by this API.
    if (off < sizeof(line) && modem.radioEnabled() == RADIO_CELLULAR) {
        CellularSignal sig = Cellular.RSSI();
        if (sig.isValid()) {
            off += snprintf(line + off, sizeof(line) - off,
                "[Sig: %s RSRP=%.0fdBm RSRQ=%.0fdB Strength=%.0f%% Quality=%.0f%%]",
                accessTechName(sig.getAccessTechnology()),
                sig.getStrengthValue(), sig.getQualityValue(),
                sig.getStrength(), sig.getQuality());
        } else {
            off += snprintf(line + off, sizeof(line) - off, "[Sig: no service]");
        }
    }

    Log.info("%s", line);
}

void setup()
{
    waitFor(Serial.isConnected, 10000);
    // WiFi.clearCredentials(); // force testing on Cellular/Satellite

    pinMode(D7, OUTPUT);
    digitalWrite(D7, LOW);

    // Load config from the env.json environment variables before anything else
    // reads g_cfg. Missing/invalid variables fall back to the compiled defaults
    // in app_config.cpp.
    loadAppConfig();

    satellite.setMaxPayloadSize(g_cfg.ntnMaxPayloadSize);

    modem.begin();

    // Hardware is initialised; the Boot state selects and enables the start radio.
    appState = AppState::Boot;
    stateEntry = true;
    stateEnterTime = millis();
}

void loop()
{
    updateConnectionTimers();
    logStatusLine();

    switch (appState) {
        // --------------------------------------------------------------------
        case AppState::Boot:
        {
            if (g_cfg.startOnCellular && g_cfg.lteEnabled) {
                Log.info("RADIO CELLULAR --------------------");
                modem.radioEnable(RADIO_CELLULAR);
                RGB.control(false);
            } else {
                Log.info("RADIO SATELLITE --------------------");
                modem.radioEnable(RADIO_SATELLITE);
                RGB.control(true);
                RGB.color(0,255,0);
            }
            updateConnectionTimers();
            logStatusLine(true /* forced */);
            transitionTo(AppState::AcquireLocation);
            break;
        }

        // --------------------------------------------------------------------
        case AppState::CellularConnect:
        {
            if (onEntry()) {
                Log.info("CELLULAR CONNECT ---------------------");
                Particle.connect();
            }
            if (cellularShouldSwitchToSatellite()) {
                transitionTo(AppState::SwitchToSatellite);
                break;
            }
            if (Particle.connected()) {
                transitionTo(AppState::CellularOnline);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::CellularOnline:
        {
            if (cellularShouldSwitchToSatellite()) {
                transitionTo(AppState::SwitchToSatellite);
                break;
            }
            if (Particle.connected()) {
                runPublishTick();
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::AcquireLocation:
        {
            // Acquire the location and program the modem's NTN location fix
            // before either stack attempts to register. 
            acquireAndSetLocationFix();
            if (modem.radioEnabled() == RADIO_CELLULAR) {
                transitionTo(AppState::CellularConnect);
            } else {
                transitionTo(AppState::SatelliteConnect);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SatelliteConnect:
        {
            if (onEntry()) {
                Log.info("SATELLITE BEGIN --------------------");
                if (satellite.begin() != SYSTEM_ERROR_NONE) {
                    Log.error("Error initializing Satellite radio");
                    RGB.color(255,0,0);
                    transitionTo(AppState::Fault);
                    break;
                }
                satellite.process();
                Log.info("SATELLITE CONNECT ---------------------");
                satellite.connect();
            }

            satellite.process();

            if (satelliteShouldSwitchToCellular()) {
                transitionTo(AppState::SwitchToCellular);
                break;
            }
            if (satellite.connected()) {
                transitionTo(AppState::SatelliteOnline);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SatelliteOnline:
        {
            satellite.process();
            if (satellite.connected()) {
                RGB.color(0,255,255);
            }

            if (satelliteShouldSwitchToCellular()) {
                transitionTo(AppState::SwitchToCellular);
                break;
            }
            if (satellite.connected()) {
                runPublishTick();
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SwitchToSatellite:
        {
            Log.info("SWITCH to SATELLITE --------------------");
            // NOTE: Very important to disconnect both Cloud and Cellular before switching to Satellite
            Particle.disconnect();
            waitFor(Particle.disconnected, 60000);
            Cellular.disconnect();

            Log.info("RADIO SATELLITE --------------------");
            if (modem.radioEnable(RADIO_SATELLITE) == SYSTEM_ERROR_NONE) {
                updateConnectionTimers();
                logStatusLine(true /* forced */);
                RGB.control(true);
                RGB.color(0,255,0);
                transitionTo(AppState::SatelliteConnect);
            } else {
                Log.error("Failed to enable Satellite radio");
                transitionTo(AppState::Fault);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::SwitchToCellular:
        {
            Log.info("SWITCH to CELLULAR --------------------");
            // NOTE: Very important to disconnect Satellite before switching to Cellular
            satellite.disconnect();
            satellite.process();
            RGB.control(false);

            Log.info("RADIO CELLULAR --------------------");
            if (modem.radioEnable(RADIO_CELLULAR) == SYSTEM_ERROR_NONE) {
                updateConnectionTimers();
                logStatusLine(true /* forced */);
                transitionTo(AppState::CellularConnect);
            } else {
                Log.error("Failed to enable Cellular radio");
                transitionTo(AppState::Fault);
            }
            break;
        }

        // --------------------------------------------------------------------
        case AppState::Fault:
        {
            // Placeholder for recovery / modem-reset escalation (future work).
            // For now, surface the fault and re-initialise from Boot.
            Log.error("FAULT state - re-initialising");
            RGB.control(true);
            RGB.color(255,0,0);
            delay(5000);
            transitionTo(AppState::Boot);
            break;
        }

        default:
            transitionTo(AppState::Fault);
            break;
    }
}
