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

#include <cstddef>
#include <cstdint>
#include <cstring>

// Static event table shared by the LTE and NTN publish paths. The same (name,
// code) pair must be configured on the cloud side - LTE publishes by name,
// NTN publishes by integer code, and subscriptions on both sides reuse this
// table.
//
// Arbitrary event names are allowed. LTE always publishes by name. NTN needs
// an integer code: names listed here map to their code; any name not listed
// falls back to kDefaultNtnEventCode.
//
// To add an event: append a row. Codes must be unique; names must be unique.
struct EventDef {
    const char* name;
    uint8_t     code;
};

constexpr EventDef kEvents[] = {
    { "loc",    1 },
    { "vitals", 2 },
    { "event",  3 }
};

constexpr size_t kEventsCount = sizeof(kEvents) / sizeof(kEvents[0]);

constexpr uint8_t kDefaultNtnEventCode = 0;

inline const EventDef* findEvent(const char* name) {
    for (size_t i = 0; i < kEventsCount; ++i) {
        if (std::strcmp(kEvents[i].name, name) == 0) {
            return &kEvents[i];
        }
    }
    return nullptr;
}
