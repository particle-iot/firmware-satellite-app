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

SYSTEM_THREAD(ENABLED);
SYSTEM_MODE(SEMI_AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_ALL);

Satellite satellite;

uint32_t s = 0;
int counter = 1;

void setup()
{
    waitUntil(Serial.isConnected);

    pinMode(D7, OUTPUT);
    digitalWrite(D7, LOW);
    RGB.control(true);
    RGB.color(0,255,0);

    Log.info("BEGIN --------------------");
    if (satellite.begin() == SYSTEM_ERROR_NONE) {
        satellite.process();

        Log.info("CONNECT ---------------------");
        satellite.connect();
        waitUntil(satellite.connected);
        satellite.process();

        if (satellite.connected()) {
            RGB.color(0,255,255);
            Log.info("PUBLISH ------------------");
            digitalWrite(D7, HIGH);
            s = millis() - 30000;
        }
    } else {
        Log.error("Error initializing Satellite radio");
        RGB.color(255,0,0);
    }
}

void loop()
{
    if (satellite.connected() && millis() - s > 30000) {
        s = millis();
        Log.info("PUBLISH: satellite/1 {\"count\",%d} ------------------", counter);

        Variant data;
        data.set("count", counter++);
        satellite.publish(1 /* code */, data);
    }

    satellite.process();
}











