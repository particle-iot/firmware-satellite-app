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

#include "modem_manager.h"

#include "logging.h"
LOG_SOURCE_CATEGORY("ncp.esim");

#include "check.h"
#include "scope_guard.h"
#include "stream_util.h"
#include "hex_to_bytes.h"

#include <str_util.h>

#include <memory>
#include <cstdint>
#include <cstdarg>
#include <pb_encode.h>
#include <cloud/cloud_new.pb.h>

/*
// List of all defined system errors
    NONE                        0
    UNKNOWN                  -100
    BUSY                     -110
    NOT_SUPPORTED            -120
    NOT_ALLOWED              -130
    CANCELLED                -140
    ABORTED                  -150
    TIMEOUT                  -160
    NOT_FOUND                -170
    ALREADY_EXISTS           -180
    TOO_LARGE                -190
    NOT_ENOUGH_DATA          -191
    LIMIT_EXCEEDED           -200
    END_OF_STREAM            -201
    INVALID_STATE            -210
    FLASH_IO                 -219
    IO                       -220
    WOULD_BLOCK              -221
    FILE                     -225
    PATH_TOO_LONG            -226
    NETWORK                  -230
    PROTOCOL                 -240
    INTERNAL                 -250
    NO_MEMORY                -260
    INVALID_ARGUMENT         -270
    BAD_DATA                 -280
    OUT_OF_RANGE             -290
    DEPRECATED               -300
    ...
    AT_NOT_OK               -1200
    AT_RESPONSE_UNEXPECTED  -1210
    ...
*/

namespace particle {

namespace {

#define ICCID_KIGEN_DEFAULT     "89000123456789012358"
#define ICCID_KIGEN_TEST        "89000123456789012341"
// Profiles are identified by their ES10c profileName (tag 92), not by ICCID prefix:
// Twilio (IE Cellular) and Skylo (IE NTN) share the prefix 898830, and the extra
// disambiguating digit is coincidental. Update these if a profile's name changes.
#define PROFILE_NAME_CELLULAR    "Twilio"        // IE Cellular (Twilio) profileName
#define PROFILE_NAME_SATELLITE1  "Pod ENO"       // IE NTN (Skylo) profileName
#define PROFILE_NAME_SATELLITE2  "M27"           // IE NTN (Skylo) profileName
#define ICCID_RESULTS_MAX        (8)
#define ICCID_MARKER             "5A0A"
#define ICCID_MARKER_LEN         (4)
#define ICCID_DISABLE            (0)
#define ICCID_ENABLE             (1)
#define PROFILE_NAME_TAG         "92"            // ES10c profileName TLV tag
#define PROFILE_NAME_LEN_MAX     (32)            // longest expected ES10c profileName

const int PROFILES_SIZE_MAX = 4096;
char profiles[PROFILES_SIZE_MAX] = {0};
const int CSIM_RESPONSE_SIZE_MAX = 4096;
char csimResponse[CSIM_RESPONSE_SIZE_MAX] = {0};

} // namespace annonymous

ModemManager::ModemManager() : begun_(false) {

}

ModemManager::~ModemManager() {
    if (begun_) {
        // de-init stuff
    }
}

int ModemManager::cbCFUN(int type, const char* buf, int len, int* cfun) {
    if ((type == TYPE_PLUS) && cfun) {
        if (sscanf(buf, "\r\n+CFUN: %d", cfun) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbIOTOPMODE(int type, const char* buf, int len, int* mode) {
    if ((type == TYPE_PLUS) && mode) {
        if (sscanf(buf, "\r\n+QCFG=\"iotopmode\",%d", mode) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbCSIMint(int type, const char* buf, int len, int* csimInt) {
    if ((type == TYPE_PLUS) && csimInt) {
        if (sscanf(buf, "\r\n+CSIM: 4,\"61%2x", csimInt) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbCSIMstring(int type, const char* buf, int len, char* csimString) {
    if ((type == TYPE_PLUS) && csimString) {
        if (sscanf(buf, "\r\n+CSIM: %*d,\"%[^\"]\r\n", csimString) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbICCID(int type, const char* buf, int len, char* iccid) {
    if ((type == TYPE_PLUS) && iccid) {
        if (sscanf(buf, "\r\n+QCCID: %[^\r]\r\n", iccid) == 1)
            /*nothing*/;
    }
    return WAIT;
}

void ModemManager::swapNibbles(const char* input, char* output) {
    for (int i = 0; i < ICCID_LEN; i+=2) {
        output[i] = input[i+1];
        output[i+1] = input[i];
    }
    output[ICCID_LEN] = 0;
}

int ModemManager::isValidHexString(const char *str, int length) {
    for (int i = 0; i < length; i++) {
        if (!isxdigit(str[i])) {
            return -1;
        }
    }
    return 0;
}

void ModemManager::stripTrailingF(char* iccid) {
    // Strip trailing F on 19 digit ICCID's
    if (strlen(iccid) == ICCID_LEN && (iccid[strlen(iccid) - 1] == 'f' || iccid[strlen(iccid) - 1] == 'F')) {
        iccid[strlen(iccid) - 1] = 0;
    }
}

void ModemManager::padIccidF(char* iccid) {
    // CSIM commands need 20 nibbles; re-add the trailing F to 19 digit ICCIDs.
    if (strlen(iccid) == (ICCID_LEN - 1)) {
        iccid[ICCID_LEN - 1] = 'F';
        iccid[ICCID_LEN] = 0;
    }
}

void ModemManager::findProfileName(const char* start, const char* end, char* nameOut) {
    // Extract the ES10c profileName (TLV tag 92) for one profile. The name lives
    // inside the profile's E3 block, after the 5A0A<iccid> and 9F7001<state> fields,
    // encoded as "92" <len-byte> <name-bytes>, all ASCII-hex. Scan is bounded to
    // [start, end) - typically up to the next ICCID marker - so a 92 belonging to a
    // later profile is never picked up. Leaves an empty string if no tag is found.
    nameOut[0] = 0;

    const char* pos = strstr(start, PROFILE_NAME_TAG);
    if (!pos || pos >= end) {
        return;
    }

    const char* lenPos = pos + 2;          // 2 hex chars consumed by the tag
    if (lenPos + 2 > end || isValidHexString(lenPos, 2) != 0) {
        return;
    }
    char lenStr[3] = { lenPos[0], lenPos[1], 0 };
    int nameBytes = (int)strtol(lenStr, NULL, 16);

    const char* dataPos = lenPos + 2;      // start of the name's hex bytes
    if (nameBytes <= 0 || dataPos + (nameBytes * 2) > end) {
        return;
    }

    int outLen = nameBytes;
    if (outLen > PROFILE_NAME_MAX - 1) {
        outLen = PROFILE_NAME_MAX - 1;
    }
    for (int i = 0; i < outLen; i++) {
        if (isValidHexString(dataPos + (i * 2), 2) != 0) {
            nameOut[i] = 0;
            return;
        }
        char byteStr[3] = { dataPos[i * 2], dataPos[(i * 2) + 1], 0 };
        nameOut[i] = (char)strtol(byteStr, NULL, 16);
    }
    nameOut[outLen] = 0;
}

int ModemManager::findIccids(const char *input, char results[][ICCID_LEN + 1], bool includeTestProfile,
        char names[][PROFILE_NAME_MAX]) {
    int count = 0;
    const char *pos = input;

    while ((pos = strstr(pos, ICCID_MARKER)) != NULL) {
        pos += ICCID_MARKER_LEN;

        if (strlen(pos) >= ICCID_LEN && isValidHexString(pos, ICCID_LEN) == 0) {
            swapNibbles(pos, results[count]); // swap into results
            stripTrailingF(results[count]);
            if (!includeTestProfile && strncmp(results[count], ICCID_KIGEN_TEST, ICCID_LEN) == 0) {
                results[count][0] = 0; // remove
            } else {
                results[count][ICCID_LEN] = 0;
                if (names) {
                    // Name lives between the end of this ICCID and the next ICCID marker.
                    const char* nameStart = pos + ICCID_LEN;
                    const char* nameEnd = strstr(nameStart, ICCID_MARKER);
                    if (!nameEnd) {
                        nameEnd = nameStart + strlen(nameStart);
                    }
                    findProfileName(nameStart, nameEnd, names[count]);
                }
                count++;
            }

            if (count >= ICCID_RESULTS_MAX) {
                break;
            }
        }

        pos += ICCID_LEN;
    }

    return count;
}

int ModemManager::getICCID(char* i, bool log) {
    char iccid[30] = {0};

    int ret = Cellular.command(cbICCID, iccid, 10000, "AT+QCCID");
    if ((ret != RESP_OK) || (strcmp(iccid, "") == 0)) {
        Log.info("SIM ICCID NOT FOUND!");
        return -1;
    }

    stripTrailingF(iccid);
    if (ret != RESP_OK) {
        strcpy(iccid, "");
        Log.error("getICCID: %d", ret);
        return -2;
    }

    if (log) {
        Log.info("ICCID currently active: %s", iccid);
    }
    strcpy(i, iccid);
    return 0;
}

int ModemManager::csimCommand(unsigned int timeoutMs, const char* format, ...) {
    char cmd[128] = {0};
    va_list args;
    va_start(args, format);
    vsnprintf(cmd, sizeof(cmd), format, args);
    va_end(args);

    int r = Cellular.command(timeoutMs, cmd);
    if (r != RESP_OK) {
        // One retry: these eUICC APDU exchanges are occasionally flaky.
        delay(1000);
        r = Cellular.command(timeoutMs, cmd);
    }
    return r;
}

int ModemManager::openSimChannel() {
    // MANAGE CHANNEL (open) then SELECT the ISD-R applet on logical channel 01.
    int r = csimCommand(10000, "AT+CSIM=10,\"0070000000\"");
    if (r != RESP_OK) {
        return r;
    }
    return csimCommand(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"");
}

int ModemManager::closeSimChannel() {
    // MANAGE CHANNEL (close) logical channel 01.
    return csimCommand(10000, "AT+CSIM=10,\"0070800100\"");
}

int ModemManager::storeProfileState(int type, const char* iccidNibbleSwapped, bool refresh) {
    // ES10c Enable (BF31) / Disable (BF32) profile, terminated by the refresh
    // flag (8101 01 = refresh, 8101 00 = no refresh). No channel open/close and
    // no CFUN here - the caller manages the channel and performs a single CFUN
    // refresh after all profile changes.
    //   AT+CSIM=50,"81E2910014BF3<1|2>11A00F5A0A<iccid>8101<refresh>"
    int r = csimCommand(10000,
            "AT+CSIM=50,\"81E2910014BF3%c11A00F5A0A%s8101%02X\"",
            type == ICCID_ENABLE ? '1' : '2',
            iccidNibbleSwapped,
            refresh ? 0x01 : 0x00);
    delay(1000); // allow the eUICC to process before GET RESPONSE
    csimCommand(10000, "AT+CSIM=10,\"81C0000006\""); // GET RESPONSE
    return r;
}

int ModemManager::refreshModem(int radioType) {
    // Single modem power cycle so it re-reads the now-active eUICC profile.
    // Sets iotopmode while powered down, unless RADIO_UNKNOWN was specified.
    Log.info("Toggling modem power to refresh SIM info...");
    Cellular.command(180000, "AT+CFUN=0");
    waitAtResponse(10);
    if (radioType != RADIO_UNKNOWN) {
        Cellular.command(2000, "AT+QCFG=\"iotopmode\",%d,1", radioType == RADIO_CELLULAR ? 0 : 3);
    }
    Cellular.command(180000, "AT+CFUN=1");
    waitAtResponse(10);
    return 0;
}

bool ModemManager::verifyActiveIccid(const char* expectedIccid, unsigned int tries) {
    char iccid[30] = {0};
    for (unsigned int i = 0; i < tries; i++) {
        if (getICCID(iccid, /* log */ false) == 0 && strncmp(iccid, expectedIccid, ICCID_LEN) == 0) {
            return true;
        }
        delay(2000); // modem may still be settling after the CFUN refresh
    }
    return false;
}

bool ModemManager::profileExists(const char* targetIccid) {
    // QUERY ALL PROFILES and check the target ICCID is present.
    Cellular.command(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"");  // returns +CSIM: 4,"6121"
    int profileSize = 0;
    Cellular.command(cbCSIMint, &profileSize, 10000, "AT+CSIM=28,\"81E2910009BF2D065C045A9F7092\""); // returns +CSIM: 4,"614E"
    if (profileSize <= 0) {
        return false;
    }

    char requestData[32] = {0};
    memset(&csimResponse, 0, sizeof(csimResponse));
    snprintf(requestData, sizeof(requestData), "AT+CSIM=10,\"81C00000%02X\"", profileSize);
    Cellular.command(cbCSIMstring, csimResponse, requestData);
    if (strlen(csimResponse) == 0) {
        return false;
    }

    char iccidList[ICCID_RESULTS_MAX][ICCID_LEN + 1];
    int iccidsFound = findIccids(csimResponse, iccidList, true /*includeTestProfile*/);
    for (int i = 0; i < iccidsFound; i++) {
        if (strcmp(iccidList[i], targetIccid) == 0) {
            return true;
        }
    }
    return false;
}

int ModemManager::enableDisableProfile(int type, char* specifiedIccid, int radioType, bool validateExists) {
    char iccid[30] = {0};

    stripTrailingF(specifiedIccid);

    if (strcmp(specifiedIccid, ICCID_KIGEN_DEFAULT) == 0) {
        Log.error("This is the Kigen Default ICCID. Invalid argument.");
        return ENABLE_DISABLE_ICCID_IS_DEFAULT;
    }

    // Make sure the modem is powered so we can talk to the eUICC.
    int cfunVal = -1;
    Cellular.command(cbCFUN, &cfunVal, 10000, "AT+CFUN?");
    if (cfunVal != 1) {
        Cellular.command(10000, "AT+CFUN=1");
        delay(5000);
    }

    int iotopmodeVal = -1;
    Cellular.command(cbIOTOPMODE, &iotopmodeVal, 10000, "AT+QCFG=\"iotopmode\"");
    if ((radioType == RADIO_CELLULAR && iotopmodeVal == 0) ||
            (radioType == RADIO_SATELLITE && iotopmodeVal == 3)) {
        radioType = RADIO_UNKNOWN;
    }

    // Validate the requested profile actually exists. Skipped on the
    // radioEnable() fast path, which already selected the ICCID from the live
    // profile list, so we don't dump+parse all profiles twice per switch.
    if (validateExists && !profileExists(specifiedIccid)) {
        Log.error("Invalid ICCID!");
        return ENABLE_DISABLE_ICCID_DOES_NOT_EXIST;
    }

    // What is active right now?
    getICCID(iccid, /* log */ false);
    Log.info("ICCID currently active: %s", iccid);

    // Decide which profile(s) to disable / enable, then do both in ONE eUICC
    // session followed by a SINGLE modem refresh.
    char toDisable[ICCID_LEN + 1] = {0};
    char toEnable[ICCID_LEN + 1] = {0};

    if (type == ICCID_ENABLE) {
        if (strncmp(iccid, specifiedIccid, ICCID_LEN) == 0) {
            Log.info("Profile already active!");
            if (radioType != RADIO_UNKNOWN) {
                refreshModem(radioType); // still ensure iotopmode is correct
            }
            return ENABLE_DISABLE_ICCID_IS_ACTIVE;
        }
        strncpy(toEnable, specifiedIccid, ICCID_LEN);
    } else { // ICCID_DISABLE
        if (strncmp(iccid, specifiedIccid, ICCID_LEN) != 0) {
            Log.info("Profile not active!");
            if (radioType != RADIO_UNKNOWN) {
                refreshModem(radioType);
            }
            return ENABLE_DISABLE_ICCID_NOT_ACTIVE;
        }
        strncpy(toDisable, specifiedIccid, ICCID_LEN);
    }

    Log.info("%sabling profile %s", type ? "En" : "Dis", specifiedIccid);

    // --- Single eUICC session: disable old + enable new ---
    char padded[ICCID_LEN + 2] = {0};
    char swapped[ICCID_LEN + 1] = {0};

    openSimChannel();
    if (toDisable[0]) {
        strncpy(padded, toDisable, ICCID_LEN);
        padded[ICCID_LEN] = 0;
        padIccidF(padded);
        swapNibbles(padded, swapped);
        storeProfileState(ICCID_DISABLE, swapped, /* refresh */ false);
    }
    if (toEnable[0]) {
        strncpy(padded, toEnable, ICCID_LEN);
        padded[ICCID_LEN] = 0;
        padIccidF(padded);
        swapNibbles(padded, swapped);
        storeProfileState(ICCID_ENABLE, swapped, /* refresh */ true);
    }
    closeSimChannel();

    // --- Single modem refresh adopts the new profile (and sets iotopmode) ---
    refreshModem(radioType);

    // Verify the switch took effect before reporting success.
    if (toEnable[0] && !verifyActiveIccid(toEnable, /* tries */ 3)) {
        getICCID(iccid, /* log */ true);
        Log.error("Profile switch verification FAILED: active=%s expected=%s", iccid, toEnable);
        return ENABLE_DISABLE_VERIFY_FAILED;
    }

    getICCID(iccid, /* log */ true);
    return ENABLE_DISABLE_SUCCESS;
}

int ModemManager::esimProfiles(char* specifiedIccid, char* profilesBuffer, int profilesBufferLen) {
    char iccid[30] = {0};
    int matched = 0;
    int silent = 0;
    if (specifiedIccid && strlen(specifiedIccid) > 0) {
        silent = 1;
        stripTrailingF(specifiedIccid);
    }

    int cfunVal = -1;
    Cellular.command(cbCFUN, &cfunVal, 10000, "AT+CFUN?");
    if (cfunVal != 1) {
        Cellular.command(10000, "AT+CFUN=1");
        delay(5000);
    }

    // Query SIM card Currently Active ICCID
    getICCID(iccid, /* log results */ false);

    // QUERY ALL PROFILES
    Cellular.command(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"");  // returns +CSIM: 4,"6121"
    int profileSize = 0;
    Cellular.command(cbCSIMint, &profileSize, 10000, "AT+CSIM=28,\"81E2910009BF2D065C045A9F7092\""); // returns +CSIM: 4,"614E"
    int iccidsFound = 0;
    char iccidList[ICCID_RESULTS_MAX][ICCID_LEN + 1];
    char nameList[ICCID_RESULTS_MAX][PROFILE_NAME_MAX];
    if (profileSize > 0) {
        char requestData[32] = {0};
        memset(&csimResponse, 0, sizeof(csimResponse));
        sprintf(requestData, "AT+CSIM=10,\"81C00000%02X\"", profileSize);
        Cellular.command(cbCSIMstring, csimResponse, requestData); // returns +CSIM: 160,"BF2D4BA049E32D5A0A980010325476981032149F700100921B47534D412054532E343820584F5220546573742050726F66696C65E3185A0A988803070000156406669F70010192065477696C696F9000"
        LOG_PRINTF_C(TRACE, "app", "%010lu [%s] D[%d]: ", millis(), "app", strlen(csimResponse));
        LOG_WRITE_C(TRACE, "app", csimResponse, strlen(csimResponse));
        LOG_PRINTF(TRACE, "\r\n");

        if (strlen(csimResponse) > 0) {
            // Test with 3 profiles (TEST, SKYLO, TWILIO) !!!! DO NOT TRY TO SET THIS DATA !!!!
            // iccidsFound = findIccids("+CSIM: 238,\"BF2D72A070E32D5A0A980010325476981032149F700100921B47534D412054532E343820584F5220546573742050726F66696C65E3255A0A980991080120002004309F7001009213536B796C6F202D204F7065726174696F6E616CE3185A0A988803070000155488619F70010192065477696C696F9000\"", iccidList, true /*includeTestProfile*/);
            iccidsFound = findIccids(csimResponse, iccidList, true /*includeTestProfile*/, nameList);
            // Log.info("iccidsFound: %d", iccidsFound);
            char temp_profiles[512] = {0};
            // if (!silent) {
            //     Log.info("\n");
            // }
            for (int i = 0; i < iccidsFound; i++) {
                char temp[128] = {0};
                bool isEnabled = (strcmp(iccid, iccidList[i]) == 0);
                // Cache the active radio type by the enabled profile's name.
                if (isEnabled) {
                    cachedRadioType_ = radioTypeForName(nameList[i]);
                }
                sprintf(temp, "[%s, %s, %s]", iccidList[i], nameList[i],
                        isEnabled ? "enabled" : "disabled");
                if (!silent) {
                    Log.info("%s", temp);
                    strcat(temp_profiles, temp);
                    if (i+1 != iccidsFound) {
                        strcat(temp_profiles, " ");
                    }
                }
                if (silent) {
                    if (strcmp(specifiedIccid, iccidList[i]) == 0) {
                        matched = 1; // found
                        if (strcmp(iccid, iccidList[i])==0) {
                            matched = 2; // enabled
                        }
                    }
                }
            }
            if (!silent) {
                if (profilesBuffer && ((int)strlen(temp_profiles) < profilesBufferLen)) {
                    strncpy(profilesBuffer, temp_profiles, profilesBufferLen);
                }
            }
        } else {
            Log.error("No CSIM Response received: %s", csimResponse);
        }
    } else {
        Log.error("CSIM Profile Size: %d", profileSize);
    }

    return matched;
}

int ModemManager::esimEnable(char* specifiedIccid) {
    return enableDisableProfile(ICCID_ENABLE, specifiedIccid, RADIO_UNKNOWN);
}

int ModemManager::esimDisable(char* specifiedIccid) {
    return enableDisableProfile(ICCID_DISABLE, specifiedIccid, RADIO_UNKNOWN);
}

radio_type_t ModemManager::radioTypeForName(const char* name) {
    if (strcmp(name, PROFILE_NAME_CELLULAR) == 0) {
        return RADIO_CELLULAR;
    } else if (strcmp(name, PROFILE_NAME_SATELLITE1) == 0 || strcmp(name, PROFILE_NAME_SATELLITE2) == 0) {
        return RADIO_SATELLITE;
    }
    return RADIO_UNKNOWN;
}

int ModemManager::findIccidByType(const char* inputBuffer, int inputBufferLen, char* matchedIccid, int radioType) {
    // inputBuffer holds "[ICCID, name, status]" entries (see esimProfiles). Classify each
    // profile name with radioTypeForName() and return the ICCID of the first profile
    // matching the requested radio type.
    if (radioType != RADIO_CELLULAR && radioType != RADIO_SATELLITE) {
        return -1;
    }

    const char* p = inputBuffer;
    while ((p = strchr(p, '[')) != NULL) {
        p++;                                       // p -> ICCID
        const char* iccidEnd = strchr(p, ',');
        if (!iccidEnd) {
            break;
        }
        const char* nameStart = iccidEnd + 1;
        while (*nameStart == ' ') {                // skip ", " separator
            nameStart++;
        }
        const char* nameEnd = strchr(nameStart, ',');
        if (!nameEnd) {
            p = iccidEnd + 1;
            continue;
        }

        char name[PROFILE_NAME_LEN_MAX + 1] = {0};
        int nameLen = nameEnd - nameStart;
        if (nameLen > PROFILE_NAME_LEN_MAX) {
            nameLen = PROFILE_NAME_LEN_MAX;        // truncated name won't match any known profile
        }
        strncpy(name, nameStart, nameLen);

        if (radioTypeForName(name) == radioType) {
            int len = iccidEnd - p;
            if (len >= inputBufferLen) {
                len = inputBufferLen - 1;
            }
            strncpy(matchedIccid, p, len);
            matchedIccid[len] = 0;
            return 0;
        }

        p = nameEnd;
    }

    return -1;
}

radio_type_t ModemManager::radioEnabled() {
    if (cachedRadioType_ == RADIO_UNKNOWN) {
        // esimProfiles() identifies the enabled profile and caches its radio type by name.
        esimProfiles(NULL, profiles, PROFILES_SIZE_MAX);
    }

    return cachedRadioType_;
}

int ModemManager::radioEnable(radio_type_t radioType) {
    // Find the ICCID for the requested radio type from the live profile list.
    esimProfiles(NULL, profiles, PROFILES_SIZE_MAX);

    char specifiedIccid[ICCID_LEN + 1] = {0};
    if (findIccidByType(profiles, strlen(profiles), specifiedIccid, radioType) != 0) {
        Log.error("Could not find requested radio_type: %d", radioType);
        return SYSTEM_ERROR_NOT_FOUND;
    }

    int r = enableDisableProfile(ICCID_ENABLE, specifiedIccid, radioType, /* validateExists */ false);
    if (r == ENABLE_DISABLE_SUCCESS || r == ENABLE_DISABLE_ICCID_IS_ACTIVE) {
        cachedRadioType_ = radioType;
        return SYSTEM_ERROR_NONE;
    }

    Log.error("radioEnable(%d) failed: enableDisableProfile returned %d", radioType, r);
    cachedRadioType_ = RADIO_UNKNOWN;
    return SYSTEM_ERROR_NOT_ALLOWED;
}

int ModemManager::waitAtResponse(unsigned int tries, unsigned int timeout) {
    unsigned int attempt = 0;
    for (;;) {
        const int r = Cellular.command(timeout, "AT");
        if (r < 0 && r != SYSTEM_ERROR_TIMEOUT) {
            return r;
        }
        if (r == RESP_OK) {
            return SYSTEM_ERROR_NONE;
        }
        if (++attempt >= tries) {
            break;
        }
    }
    return SYSTEM_ERROR_TIMEOUT;
}

int ModemManager::begin() {
    begun_ = true;

    if (!Cellular.isOn() || Cellular.isOff()) {
        // Turn on the modem
        Cellular.on();
        if (!waitFor(Cellular.isOn, 60000)) {
            return SYSTEM_ERROR_TIMEOUT;
        }
    }

    waitAtResponse(5); // Check if the module is alive

    Cellular.command(2000, "AT+QGMR");

    return 0;
}

} // namespace particle


