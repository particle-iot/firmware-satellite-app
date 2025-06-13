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

#pragma once

#include "Particle.h"

#include "system_error.h"

namespace particle {

typedef enum {
    RADIO_UNKNOWN                    = 1,
    RADIO_CELLULAR                   = 2,
    RADIO_SATELLITE                  = 3,
} radio_type_t;

#define ICCID_LEN               (20)

typedef enum {
    ENABLE_DISABLE_SUCCESS                         = 0,
    ENABLE_DISABLE_ICCID_IS_DEFAULT                = 1,
    ENABLE_DISABLE_ICCID_DOES_NOT_EXIST            = 2,
    ENABLE_DISABLE_ICCID_NOT_ACTIVE                = 3,
    ENABLE_DISABLE_ICCID_IS_ACTIVE                 = 4,
} enable_disable_error_t;

class ModemManager {

public:

    ModemManager();
    ~ModemManager();

    int begin(void);
    int esimEnable(char* specifiedIccid);
    int esimDisable(char* specifiedIccid);
    int esimProfiles(char* specifiedIccid, char* profilesBuffer, int profilesBufferLen);
    radio_type_t radioEnabled();
    int radioEnable(radio_type_t radio_type);

private:

    bool begun_; // true if begin() previously called
    radio_type_t cachedRadioType_;

    static int cbCFUN(int type, const char* buf, int len, int* cfun);
    static int cbIOTOPMODE(int type, const char* buf, int len, int* mode);
    static int cbCSIMint(int type, const char* buf, int len, int* csimInt);
    static int cbCSIMstring(int type, const char* buf, int len, char* csimString);
    static int cbICCID(int type, const char* buf, int len, char* iccid);

    int waitAtResponse(unsigned int tries, unsigned int timeout = 3000);

    void swapNibbles(const char* input, char* output);
    int isValidHexString(const char *str, int length);
    void stripTrailingF(char* iccid);
    int findIccids(const char *input, char results[][ICCID_LEN + 1], bool includeTestProfile);
    int getICCID(char* i, bool log);
    void enableDisableICCID(int type, char* specifiedIccid, int radioType);
    int enableDisableProfile(int type, char* specifiedIccid, int radioType);
    int findIccidByType(const char* inputBuffer, int inputBufferLen, char* matchedIccid, int radioType);
    void updateCachedRadioType(char* iccid);

};

} // particle