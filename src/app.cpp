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

SYSTEM_MODE(SEMI_AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_ALL);

Satellite satellite;

typedef enum AppState {
    GetGNSSLocation,
    PublishGNSSLocation
} AppState;

uint32_t s = 0;
int publishCount = 1;
int satPublishSuccess = 0;
int satPublishFailures = 0;
AppState state = AppState::GetGNSSLocation;

void setup()
{
    //waitUntil(Serial.isConnected);

    pinMode(D7, OUTPUT);
    digitalWrite(D7, LOW);
    RGB.control(true);
    RGB.color(0,255,0);

    // WiFi.setCredentials("MySSID", "MyPassword");
    if (WiFi.hasCredentials()) {
        WiFi.on();
        waitUntil(WiFi.isOn);
        WiFi.connect();
        if (waitFor(WiFi.ready, 30000)) {
            Particle.connect();
        }
    }   

    Log.info("BEGIN --------------------");
    if (satellite.begin() == SYSTEM_ERROR_NONE) {
        satellite.process();

        Log.info("CONNECT ---------------------");
        satellite.connect();
        waitUntil(satellite.connected);
        satellite.process();

        if (satellite.connected()) {
            RGB.color(0,255,255);
            Log.info("SATELLITE CONNECTED ------------------");
            digitalWrite(D7, HIGH);
            s = millis() - 30000;
        }
    } else {
        Log.error("Error initializing Satellite radio");
        RGB.color(255,0,0);
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

void loop()
{
    if (satellite.connected() && millis() - s > 30000) {
        s = millis();

        switch(state) {
            case AppState::GetGNSSLocation:
            {
                satellite.getGNSSLocation();
                // Make sure we re-connect to sklyo NTN after getting gnss fix
                satellite.updateRegistration(true);

                state = AppState::PublishGNSSLocation;
                break;
            }

            case AppState::PublishGNSSLocation:
            {
                Log.info("PUBLISH: satellite/1 {\"count\",%d} ------------------", publishCount);

                Variant data;
                data.set("count", publishCount);
                data.set("lat", satellite.lastPositionInfo().latitude);
                data.set("long", satellite.lastPositionInfo().longitude);
                //data.set("alt", (int)satellite.lastPositionInfo().altitude);
        
                if (satellite.connected()) {
                    auto satPublishResult = satellite.publish(1 /* code */, data);
                    satPublishResult < 0 ? satPublishFailures++ : satPublishSuccess++;
                    Log.info("Satellite publish successes/total %d/%d ", satPublishSuccess, publishCount);
                }
        
                if (Particle.connected()) {
                    auto cloudPublishResult = publishLocation(satellite.lastPositionInfo());
                    Log.info("Cloud publish result: %d", cloudPublishResult);
                }

                publishCount++;
                state = AppState::GetGNSSLocation;
                break;
            }

            default:
                break;
        }
    }

    satellite.process();
}











