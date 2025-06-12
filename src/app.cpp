/*
 * Copyright (c) 2024 Particle Industries, Inc.  All rights reserved.
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

SYSTEM_MODE(SEMI_AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_ALL);

Satellite satellite;
ModemManager modem;

// NOTE: Set both of the following options to 0 for normal operation, or 1 to TEST forced switching between radios
//       based on the timeouts that follow.  e.g. if FORCE_CELLULAR_TO_SATELLITE_SWITCH is set to 1 and
//       FORCE_RADIO_CELLULAR_TO_SATELLITE_SWITCH_TIMEOUT is set to 120000, the application will switch from Cellular
//       to Satellite after 2 minutes.
#define FORCE_CELLULAR_TO_SATELLITE_SWITCH (1)
#define FORCE_SATELLITE_TO_CELLULAR_SWITCH (1)
#define FORCE_RADIO_CELLULAR_TO_SATELLITE_SWITCH_TIMEOUT (2 * 60 * 1000)
#define FORCE_RADIO_SATELLITE_TO_CELLULAR_SWITCH_TIMEOUT (2 * 60 * 1000)

// These are pretty standard timeouts.  It is NOT recommended to set them below 10 minutes.
// There is no CELLULAR_CONNECTED_TIMEOUT because if it's connected there's no reason to switch to satellite.
#define CELLULAR_DISCONNECTED_TIMEOUT (10 * 60 * 1000)
#define SATELLITE_CONNECTED_TIMEOUT (10 * 60 * 1000)
#define SATELLITE_DISCONNECTED_TIMEOUT (20 * 60 * 1000)

// NOT recommended to set the publish interval below 10 seconds when on satellite
#define PUBLISH_INTERVAL (30000)

// Start up on Cellular (1) Start up on Satellite (0)
// NOTE: This is just for testing, you should always start on Cellular and only switch
// to Satellite if cellular signal drops.
#define START_ON_CELLULAR (1)

typedef enum AppPublishState {
    WaitForConnnect,
    GetGNSSLocation,
    PublishGNSSLocation
} AppPublishState;

uint32_t lastPublish = 0;
uint32_t connectedTime = 0;
uint32_t disconnectedTime = 0;
uint32_t radioTime = 0;

int publishCount = 1;
int satPublishSuccess = 0;
int satPublishFailures = 0;
AppPublishState publishState = AppPublishState::WaitForConnnect;


void updateConnectionTimers() {
    bool connected = false;
    static radio_type_t lastRadio = RADIO_UNKNOWN;

    if (modem.radioEnabled() == RADIO_CELLULAR) {
        if (Particle.connected()) {
            connected = true;
        }
    } else if (modem.radioEnabled() == RADIO_SATELLITE) {
        if (satellite.connected()) {
            connected = true;
        }
    }

    if (lastRadio != modem.radioEnabled()) {
        connectedTime = 0;
        disconnectedTime = 0;
        radioTime = millis(); // reset radio time
        lastRadio = modem.radioEnabled();
    }

    if (connected) {
        if (!connectedTime) {
            connectedTime = millis();
        }
        disconnectedTime = 0;
    } else {
        if (!disconnectedTime) {
            disconnectedTime = millis();
        }
        connectedTime = 0;
    }
}

// Manually construct a 'loc' object to publish position to Particle Cloud
int publishLocation(GnssPositioningInfo position) {
    char publishBuffer[1024] = {};

    if (!position.valid) {
        return -1;
    }

    memset(publishBuffer, 0, sizeof(publishBuffer));
    SpecialJSONWriter writer(publishBuffer, sizeof(publishBuffer));
    auto now = (unsigned int)Time.now();
    writer.beginObject();
        writer.name("cmd").value("loc");
        writer.name("time").value(now);
        writer.name("loc").beginObject();
            writer.name("lck").value(1);
            writer.name("time").value(now);
            writer.name("lat").value(position.latitude);
            writer.name("lon").value(position.longitude);
            writer.name("alt").value(position.altitude);
        writer.endObject();
    writer.endObject();

    return Particle.publish("loc", publishBuffer);
}

void setup()
{
    // waitUntil(Serial.isConnected);
    WiFi.clearCredentials(); // force testing on Cellular/Satellite

    pinMode(D7, OUTPUT);
    digitalWrite(D7, LOW);

    // Make sure we start up with Cellular enabled,
    // it is less expensive and can handle larger payloads.
    Log.info("RADIO CELLULAR --------------------");
    modem.begin();

#if START_ON_CELLULAR
    // Start on Cellular
    modem.radioEnable(RADIO_CELLULAR);

    Particle.connect();
    waitFor(Particle.connected, 120000);

#else
    // Start on Satellite
    modem.radioEnable(RADIO_SATELLITE);
    RGB.control(true);
    RGB.color(0,255,0);

    if (satellite.begin() == SYSTEM_ERROR_NONE) {
        satellite.process();

        satellite.connect();
    } else {
        Log.error("Error initializing Satellite radio");
        RGB.color(255,0,0);
    }
#endif // START_ON_CELLULAR
}

void loop()
{
    updateConnectionTimers();

    // If on Cellular connection, if no signal for 10 minutes, switch to Satellite
    if ( (modem.radioEnabled() == RADIO_CELLULAR) &&

#if FORCE_CELLULAR_TO_SATELLITE_SWITCH
            (radioTime && (millis() - radioTime > FORCE_RADIO_CELLULAR_TO_SATELLITE_SWITCH_TIMEOUT))
#else
            (disconnectedTime && (millis() - disconnectedTime > CELLULAR_DISCONNECTED_TIMEOUT))
#endif // FORCE_RADIO_CELLULAR_TO_SATELLITE_SWITCH_TIMEOUT
            )
    {
        Log.info("SWITCH to SATELLITE --------------------");
        // NOTE: Very important to disconnect both Cloud and Cellular before switching to Satellite
        Particle.disconnect();
        waitFor(Particle.disconnected, 60000);
        Cellular.disconnect();

        Log.info("RADIO SATELLITE --------------------");
        if (modem.radioEnable(RADIO_SATELLITE) == SYSTEM_ERROR_NONE) {
            RGB.control(true);
            RGB.color(0,255,0);

            Log.info("SATELLITE BEGIN --------------------");
            if (satellite.begin() == SYSTEM_ERROR_NONE) {
                satellite.process();

                Log.info("SATELLITE CONNECT ---------------------");
                satellite.connect();
            } else {
                Log.error("Error initializing Satellite radio");
                RGB.color(255,0,0);
            }
        }
        publishState = AppPublishState::WaitForConnnect;
    }
    // If on Satellite connection, if connected or disconnected for 10 minutes, switch to Cellular
    //
    // Note: we don't want to camp on Satellite if Cellular is available, but
    //       the only way to know if Cellular is available is to go test it again.
    else if ( (modem.radioEnabled() == RADIO_SATELLITE) &&

#if FORCE_SATELLITE_TO_CELLULAR_SWITCH
            (radioTime && (millis() - radioTime > FORCE_RADIO_SATELLITE_TO_CELLULAR_SWITCH_TIMEOUT))
#else
            ( (disconnectedTime && (millis() - disconnectedTime > SATELLITE_DISCONNECTED_TIMEOUT)) || (connectedTime && (millis() - connectedTime > SATELLITE_CONNECTED_TIMEOUT)) )
#endif // FORCE_RADIO_SATELLITE_TO_CELLULAR_SWITCH_TIMEOUT
            )
    {
        Log.info("SWITCH to CELLULAR --------------------");
        // NOTE: Very important to disconnect Satellite before switching to Cellular
        satellite.disconnect();
        satellite.process(); // process disconnect
        RGB.control(false);

        Log.info("RADIO CELLULAR --------------------");
        if (modem.radioEnable(RADIO_CELLULAR) == SYSTEM_ERROR_NONE) {

            Log.info("CELLULAR CONNECT ---------------------");
            Particle.connect();
            waitFor(Particle.connected, 120000);
        }
        publishState = AppPublishState::WaitForConnnect;
    }

    // Attempt to publish
    if (millis() - lastPublish > PUBLISH_INTERVAL) {
        // Handle publish differently based on connection type, also publish location
        switch (publishState) {
            case AppPublishState::WaitForConnnect:
            {
                if (satellite.connected() || Particle.connected()) {
                    publishState = AppPublishState::GetGNSSLocation;
                }
                break;
            }

            case AppPublishState::GetGNSSLocation:
            {
                satellite.getGNSSLocation();
                if (modem.radioEnabled() == RADIO_SATELLITE) {
                    // Make sure we re-connect to Skylo NTN after getting gnss fix
                    satellite.process(true /* force updateRegistration */);
                }
                lastPublish = millis() - PUBLISH_INTERVAL + 2000; // Ensure we don't try to publish immediately after using GNSS
                publishState = AppPublishState::PublishGNSSLocation;
                break;
            }

            case AppPublishState::PublishGNSSLocation:
            {
                Variant data;
                data.set("count", publishCount);
                data.set("lat", satellite.lastPositionInfo().latitude);
                data.set("long", satellite.lastPositionInfo().longitude);
                //data.set("alt", (int)satellite.lastPositionInfo().altitude);

                if (satellite.connected())
                {
                    Log.info("SATELLITE PUBLISH: {\"count\",%d} ------------------", publishCount);
                    auto satPublishResult = satellite.publish(1 /* code */, data);
                    satPublishResult < 0 ? satPublishFailures++ : satPublishSuccess++;
                    Log.info("Satellite publish successes/total %d/%d ", satPublishSuccess, satPublishSuccess + satPublishFailures);
                    lastPublish = millis();
                }
                else if (Particle.connected())
                {
                    Log.info("CELLULAR PUBLISH: {\"count\",%d} ------------------", publishCount);
                    Particle.publish("cellular", String::format("{\"count\",%d}", publishCount));

                    auto cloudPublishResult = publishLocation(satellite.lastPositionInfo());
                    Log.info("Cellular publish result: %d", cloudPublishResult);
                    lastPublish = millis();
                } else {
                    publishState = AppPublishState::WaitForConnnect;
                    break;
                }

                publishCount++;
                publishState = AppPublishState::GetGNSSLocation;
                break;
            }

            default:
                break;
        }

    }

    if (modem.radioEnabled() == RADIO_SATELLITE) {
        satellite.process();

        if (satellite.connected()) {
            RGB.color(0,255,255);
        }
    }
}











