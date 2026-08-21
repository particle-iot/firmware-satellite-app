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

#include "system_error.h"

namespace particle {

typedef enum {
    RADIO_UNKNOWN                    = 1,
    RADIO_CELLULAR                   = 2,
    RADIO_SATELLITE                  = 3,
} radio_type_t;

#define ICCID_LEN               (20)
#define PROFILE_NAME_MAX        (65)    // SGP.22 profileName is <= 64 bytes, +1 for NUL
#define NOTIF_SEQ_HEX_MAX       (9)     // notification seqNumber in hex, up to 4 bytes, +1 for NUL

typedef enum {
    ENABLE_DISABLE_SUCCESS                         = 0,
    ENABLE_DISABLE_ICCID_IS_DEFAULT                = 1,
    ENABLE_DISABLE_ICCID_DOES_NOT_EXIST            = 2,
    ENABLE_DISABLE_ICCID_NOT_ACTIVE                = 3,
    ENABLE_DISABLE_ICCID_IS_ACTIVE                 = 4,
    ENABLE_DISABLE_VERIFY_FAILED                   = 5,
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
    int simChannel_; // logical channel the card allocated, -1 when no channel is open

    static int cbCFUN(int type, const char* buf, int len, int* cfun);
    static int cbIOTOPMODE(int type, const char* buf, int len, int* mode);
    static int cbCSIMint(int type, const char* buf, int len, int* csimInt);
    static int cbCSIMstring(int type, const char* buf, int len, char* csimString);
    static int cbICCID(int type, const char* buf, int len, char* iccid);
    static int cbCPIN(int type, const char* buf, int len, char* code);

    int waitAtResponse(unsigned int tries, unsigned int timeout = 3000);

    void swapNibbles(const char* input, char* output);
    int isValidHexString(const char *str, int length);
    void stripTrailingF(char* iccid);
    void padIccidF(char* iccid);
    void findProfileName(const char* start, const char* end, char* nameOut);
    int findIccids(const char *input, char results[][ICCID_LEN + 1], bool includeTestProfile,
            char names[][PROFILE_NAME_MAX] = nullptr);
    int getICCID(char* i, bool log);

    // Low-level eUICC (ES10) helpers, composed by enableDisableProfile().
    int csimCommand(unsigned int timeoutMs, const char* format, ...);
    int simIsoCla();                                                       // CLA for SELECT on the open channel
    int simGpCla();                                                        // CLA for STORE DATA / GET RESPONSE
    bool simReady();                                                       // AT+CPIN? reports READY
    int waitForSimReady(unsigned int timeoutMs);                           // poll until the card is back after a REFRESH
    int openSimChannel();                                                  // MANAGE CHANNEL open + SELECT ISD-R
    int closeSimChannel();                                                 // MANAGE CHANNEL close
    int storeProfileState(int type, const char* iccidNibbleSwapped, bool refresh); // ES10c Enable/Disable APDU (no CFUN)
    int esimClearNotifications();

    // ES10b notification helpers
    static int tlvNext(const char* hex, int hexLen, int pos, unsigned int* tag, int* valPos,
            int* valLen, int* nextPos);                                        // walk one ASCII-hex TLV
    int listNotificationSeqs(char seqList[][NOTIF_SEQ_HEX_MAX], int maxCount);  // ES10b.ListNotification (BF28)
    int parseNotificationSeqs(const char* respHex, char seqList[][NOTIF_SEQ_HEX_MAX],
            int maxCount);                                                     // seqNumbers from a BF28 response
    int removeNotification(const char* seqHex);                                // ES10b.RemoveNotificationFromList (BF30)
    int sweepNotifications();                                                  // list + delete, channel must be open
    int refreshModem(int radioType);                                       // single CFUN cycle (+ iotopmode)
    bool verifyActiveIccid(const char* expectedIccid, unsigned int tries);
    bool profileExists(const char* targetIccid);

    int enableDisableProfile(int type, char* specifiedIccid, int radioType, bool validateExists = true);
    int findIccidByType(const char* inputBuffer, int inputBufferLen, char* matchedIccid, int radioType);
    radio_type_t radioTypeForName(const char* name);

};

} // particle