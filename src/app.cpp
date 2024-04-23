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
SYSTEM_MODE(MANUAL);

SerialLogHandler logHandler(LOG_LEVEL_ALL);

Satellite satellite;

void setup()
{
    waitUntil(Serial.isConnected);

    pinMode(D7, OUTPUT);
    digitalWrite(D7, LOW);
    RGB.control(true);
    RGB.color(0,255,0);

    Log.info("BEGIN --------------------");
    int begin = satellite.begin();
    if (begin < 0) {
        Log.info("Error initializing Satellite radio");
    }
    satellite.process();

    if (begin != 0) {
        Log.info("Init failed.");
        RGB.color(255,0,0);
    } else {
        Log.info("CONNECT ---------------------");
        satellite.connect();
        waitUntil(satellite.connected);
    }
    satellite.process();

    if (satellite.connected()) {
        RGB.color(0,255,255);
        Log.info("PUBLISH ------------------");
        digitalWrite(D7, HIGH);
    }
}

void loop()
{
    static uint32_t s = millis() - 20000;
    static uint32_t c = 0;
    static String str = "1234567890";
    if (satellite.connected() && millis() - s > 20000) {
        // if (c == 2) {
        //     satellite.disconnect();
        // } else if (c == 4) {
        //     satellite.connect();
        // }
        Log.info("TXing: %lu", c);
        Variant v;
        v["foo"] = str.c_str();
        satellite.publish(123 /* code */, v);
        // str += "1234567890";
        s = millis();
        c++;

    }

    satellite.process();
    Particle.process(); // MANUAL MODE only
}
