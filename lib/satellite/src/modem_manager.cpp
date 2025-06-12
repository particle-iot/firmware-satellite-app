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
#define ICCID_TWILIO_PREFIX     "8988"
#define ICCID_SKYLO_PREFIX      "8990"
#define ICCID_PREFIX_LEN        (4)
#define ICCID_RESULTS_MAX       (8)
#define ICCID_MARKER            "5A0A"
#define ICCID_MARKER_LEN        (4)
#define ICCID_DISABLE           (0)
#define ICCID_ENABLE            (1)

const int PROFILES_SIZE_MAX = 4096;
char profiles[PROFILES_SIZE_MAX] = {0};

} // namespace annonymous

ModemManager::ModemManager() : begun_(false)
{

}

ModemManager::~ModemManager() {
    if (begun_) {
        // de-init stuff
    }
}

int ModemManager::cbCFUN(int type, const char* buf, int len, int* cfun)
{
    if ((type == TYPE_PLUS) && cfun) {
        if (sscanf(buf, "\r\n+CFUN: %d", cfun) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbIOTOPMODE(int type, const char* buf, int len, int* mode)
{
    if ((type == TYPE_PLUS) && mode) {
        if (sscanf(buf, "\r\n+QCFG=\"iotopmode\",%d", mode) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbCSIMint(int type, const char* buf, int len, int* csimInt)
{
    if ((type == TYPE_PLUS) && csimInt) {
        if (sscanf(buf, "\r\n+CSIM: 4,\"61%2x", csimInt) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbCSIMstring(int type, const char* buf, int len, char* csimString)
{
    if ((type == TYPE_PLUS) && csimString) {
        if (sscanf(buf, "\r\n+CSIM: %*d,\"%[^\"]\r\n", csimString) == 1)
            /*nothing*/;
    }
    return WAIT;
}

int ModemManager::cbICCID(int type, const char* buf, int len, char* iccid)
{
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

int ModemManager::findIccids(const char *input, char results[][ICCID_LEN + 1], bool includeTestProfile) {
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

    int ret = Cellular.command(cbICCID, iccid, 10000, "AT+QCCID\r\n");
    if ((ret == RESP_OK) && (strcmp(iccid, "") != 0)) {
        // Log.info("SIM ICCID = %s", iccid);
    } else {
        Log.info("SIM ICCID NOT FOUND!");
        return -1;
    }

    stripTrailingF(iccid);
    if (ret != RESP_OK) {
        strcpy(iccid, "");
        Log.error("getICCID: %d", ret);
        return -2;
    } else {
        if (log) {
            Log.info("ICCID currently active: %s ", iccid);
            if (strcmp(iccid, ICCID_KIGEN_DEFAULT) == 0) {
                LOG_PRINTF(TRACE, "(Kigen Default Profile)\n");
            } else if (strcmp(iccid, ICCID_KIGEN_TEST) == 0) {
                LOG_PRINTF(TRACE, "(Kigen Test Profile)\n");
            } else if (strncmp(iccid, ICCID_TWILIO_PREFIX, ICCID_PREFIX_LEN) == 0) {
                LOG_PRINTF(TRACE, "(Twilio Super SIM)\n");
            } else if (strncmp(iccid, ICCID_SKYLO_PREFIX, ICCID_PREFIX_LEN) == 0) {
                LOG_PRINTF(TRACE, "(Skylo SIM)\n");
            } else {
                LOG_PRINTF(TRACE, "(Unknown)\n");
            }
        }
    }

    updateCachedRadioType(iccid);

    strcpy(i, iccid);
    return 0;
}

void ModemManager::enableDisableICCID(int type, char* specifiedIccid, int radioType) {
    if (strlen(specifiedIccid) < (ICCID_LEN-1)) {
        return;
    }

    // Add F to the end of 19 digit ICCIDs for CSIM commands
    if (strlen(specifiedIccid) == (ICCID_LEN-1)) {
        specifiedIccid[ICCID_LEN-1] = 'F';
        specifiedIccid[ICCID_LEN] = 0;
    }

    char iccidNibbleSwapped[ICCID_LEN + 1] = {0};
    swapNibbles(specifiedIccid, iccidNibbleSwapped);
    if (RESP_OK != Cellular.command(10000, "AT+CSIM=10,\"0070000000\"\r\n")) {
        delay(1000);
        Cellular.command(10000, "AT+CSIM=10,\"0070000000\"");
    }
    if (RESP_OK != Cellular.command(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"\r\n")) {
        delay(1000);
        Cellular.command(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"\r\n");
    }
    // Insert the desired nibble swapped profile here
    // AT+CSIM=50,"81E2910014BF3211A00F5A0Axxxxxxxxxxxxxxxxxxxx810101"
    char requestData[70] = {0};
    if (type == 0) {
        // Disable
        sprintf(requestData, "AT+CSIM=50,\"81E2910014BF3211A00F5A0A%s810101\"\r\n", iccidNibbleSwapped);
    } else {
        // Enable
        sprintf(requestData, "AT+CSIM=50,\"81E2910014BF3111A00F5A0A%s810101\"\r\n", iccidNibbleSwapped);
    }
    if (RESP_OK != Cellular.command(10000, requestData)) {
        delay(1000);
        Cellular.command(10000, requestData);
    }
    delay(1000); // wait a bit before closing the channel
    if (RESP_OK != Cellular.command(10000, "AT+CSIM=10,\"81C0000006\"\r\n")) {
        delay(1000);
        Cellular.command(10000, "AT+CSIM=10,\"81C0000006\"\r\n");
    }
    if (RESP_OK != Cellular.command(10000, "AT+CSIM=10,\"0070800100\"\r\n")) {
        delay(1000);
        Cellular.command(10000, "AT+CSIM=10,\"0070800100\"\r\n");
    }

    // Toggle CFUN to refresh SIM data
    Log.info("Toggling modem power to refresh SIM info...");
    Cellular.command(180000, "AT+CFUN=0\r\n");
    waitAtResponse(10);
    // If RADIO_UNKNOWN specified, nothing will be set.
    if (radioType != RADIO_UNKNOWN) {
        Cellular.command(2000, "AT+QCFG=\"iotopmode\",%d,1\r\n", radioType == RADIO_CELLULAR ? 0 : 3);
    }
    Cellular.command(180000, "AT+CFUN=1\r\n");
    waitAtResponse(10);
}

int ModemManager::enableDisableProfile(int type, char* specifiedIccid, int radioType) {
    char iccid[30] = {0};

    stripTrailingF(specifiedIccid);

    if (strcmp(specifiedIccid, ICCID_KIGEN_DEFAULT) == 0) {
        Log.error("This is the Kigen Default ICCID. Invalid argument.");
        return ENABLE_DISABLE_ICCID_IS_DEFAULT;
    }

    int cfunVal = -1;
    Cellular.command(cbCFUN, &cfunVal, 10000, "AT+CFUN?\r\n");
    if (cfunVal != 1) {
        Cellular.command(10000, "AT+CFUN=1");
        delay(5000);
    }

    int iotopmodeVal = -1;
    Cellular.command(cbIOTOPMODE, &iotopmodeVal, 10000, "AT+QCFG=\"iotopmode\"\r\n");
    if ((radioType == RADIO_CELLULAR && iotopmodeVal == 0) ||
            (radioType == RADIO_SATELLITE && iotopmodeVal == 3)) {
        // If already set correctly, prevent IOTOPMODE from being set later on
        radioType = RADIO_UNKNOWN;
    }

    // QUERY ALL PROFILES
    Cellular.command(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"\r\n");  // returns +CSIM: 4,"6121"
    int profileSize = 0;
    Cellular.command(cbCSIMint, &profileSize, 10000, "AT+CSIM=28,\"81E2910009BF2D065C045A9F7092\"\r\n"); // returns +CSIM: 4,"614E"
    int iccidsFound = 0;
    char iccidList[ICCID_RESULTS_MAX][ICCID_LEN + 1];
    if (profileSize > 0) {
        char requestData[32] = {0};
        char csimResponse[4096] = {0};
        sprintf(requestData, "AT+CSIM=10,\"81C00000%02X\"", profileSize);
        Cellular.command(cbCSIMstring, csimResponse, requestData); // returns +CSIM: 160,"BF2D4BA049E32D5A0A980010325476981032149F700100921B47534D412054532E343820584F5220546573742050726F66696C65E3185A0A988803070000156406669F70010192065477696C696F9000"

        if (strlen(csimResponse) > 0) {
            // Test with 3 profiles (TEST, SKYLO, TWILIO) !!!! DO NOT TRY TO SET THIS DATA !!!!
            // iccidsFound = findIccids("+CSIM: 238,\"BF2D72A070E32D5A0A980010325476981032149F700100921B47534D412054532E343820584F5220546573742050726F66696C65E3255A0A980991080120002004309F7001009213536B796C6F202D204F7065726174696F6E616CE3185A0A988803070000155488619F70010192065477696C696F9000\"", iccidList, false /*includeTestProfile*/);
            iccidsFound = findIccids(csimResponse, iccidList, true /*includeTestProfile*/);
            if (iccidsFound == 0) {
                Log.error("No ICCID's found");
                return ENABLE_DISABLE_ICCID_DOES_NOT_EXIST;
            }
            // for (int i = 0; i < iccidsFound; i++) {
            //     Log.info("ICCID%d: %s\n", i+1, iccidList[i]);
            // }
            int iccidMatched = 0;
            for (int i = 0; i < iccidsFound; i++) {
                // Log.info("ICCID%d: %s\n", i+1, iccidList[i]);
                if (strcmp(iccidList[i], specifiedIccid) == 0) {
                    iccidMatched = 1;
                    break;
                }
            }
            if (!iccidMatched) {
                Log.error("Invalid ICCID!");
                return ENABLE_DISABLE_ICCID_DOES_NOT_EXIST;
            }
        }
    }

    getICCID(iccid, /* log results */ false);

    int ret = ENABLE_DISABLE_SUCCESS;
    Log.info("ICCID currently active: %s", iccid);
    if (type == ICCID_DISABLE && strncmp(iccid, specifiedIccid, 20) != 0) {
        Log.info("Profile not active!");
        ret = ENABLE_DISABLE_ICCID_NOT_ACTIVE;
    } else if (type == ICCID_ENABLE) {
        if (strncmp(iccid, specifiedIccid, 20) == 0) {
            Log.info("Profile already active!");
            ret = ENABLE_DISABLE_ICCID_IS_ACTIVE;
        } else if (strncmp(iccid, ICCID_KIGEN_DEFAULT, 20) != 0) {
            // disable currently active ICCID that is not the Kigen Default
            Log.info("Disabling currently active: %s", iccid);
            enableDisableICCID(ICCID_DISABLE, iccid, RADIO_UNKNOWN);
        }
    }

    if (ret != ENABLE_DISABLE_SUCCESS) {
        // we have an error, but need to set iotopmode before exiting
        if (radioType != RADIO_UNKNOWN) {
            Cellular.command(180000, "AT+CFUN=0\r\n");
            waitAtResponse(10);
            Cellular.command(2000, "AT+QCFG=\"iotopmode\",%d,1\r\n", radioType == RADIO_CELLULAR ? 0 : 3);
            Cellular.command(180000, "AT+CFUN=1\r\n");
            waitAtResponse(10);
        }

        return ret;
    }

    Log.info("%sabling profile %s\n", type ? "En" : "Dis", specifiedIccid);
    enableDisableICCID(type, specifiedIccid, radioType);

    getICCID(iccid, /* log results */ true);

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
    Cellular.command(cbCFUN, &cfunVal, 10000, "AT+CFUN?\r\n");
    if (cfunVal != 1) {
        Cellular.command(10000, "AT+CFUN=1");
        delay(5000);
    }

    // Query SIM card Currently Active ICCID
    getICCID(iccid, /* log results */ false);

    // QUERY ALL PROFILES
    Cellular.command(10000, "AT+CSIM=42,\"01A4040410A0000005591010FFFFFFFF8900000100\"\r\n");  // returns +CSIM: 4,"6121"
    int profileSize = 0;
    Cellular.command(cbCSIMint, &profileSize, 10000, "AT+CSIM=28,\"81E2910009BF2D065C045A9F7092\"\r\n"); // returns +CSIM: 4,"614E"
    int iccidsFound = 0;
    char iccidList[ICCID_RESULTS_MAX][ICCID_LEN + 1];
    if (profileSize > 0) {
        char requestData[32] = {0};
        char csimResponse[4096] = {0};
        sprintf(requestData, "AT+CSIM=10,\"81C00000%02X\"", profileSize);
        Cellular.command(cbCSIMstring, csimResponse, requestData); // returns +CSIM: 160,"BF2D4BA049E32D5A0A980010325476981032149F700100921B47534D412054532E343820584F5220546573742050726F66696C65E3185A0A988803070000156406669F70010192065477696C696F9000"
        LOG_PRINTF_C(TRACE, "app", "%010lu [%s] D[%d]: ", millis(), "app", strlen(csimResponse));
        LOG_WRITE_C(TRACE, "app", csimResponse, strlen(csimResponse));
        LOG_PRINTF(TRACE, "\r\n");

        if (strlen(csimResponse) > 0) {
            // Test with 3 profiles (TEST, SKYLO, TWILIO) !!!! DO NOT TRY TO SET THIS DATA !!!!
            // iccidsFound = findIccids("+CSIM: 238,\"BF2D72A070E32D5A0A980010325476981032149F700100921B47534D412054532E343820584F5220546573742050726F66696C65E3255A0A980991080120002004309F7001009213536B796C6F202D204F7065726174696F6E616CE3185A0A988803070000155488619F70010192065477696C696F9000\"", iccidList, true /*includeTestProfile*/);
            iccidsFound = findIccids(csimResponse, iccidList, true /*includeTestProfile*/);
            // Log.info("iccidsFound: %d", iccidsFound);
            char temp_profiles[512] = {0};
            // if (!silent) {
            //     Log.info("\n");
            // }
            for (int i = 0; i < iccidsFound; i++) {
                char temp[40] = {0};
                sprintf(temp, "[%s, %s]", iccidList[i], strcmp(iccid, iccidList[i])==0 ? "enabled" : "disabled");
                if (!silent) {
                    // printf("%s", temp);
                    strcat(temp_profiles, temp);
                }
                // if (strcmp(iccidList[i], ICCID_KIGEN_TEST) == 0) {
                //     printf(" (Kigen Test Profile)");
                // }
                if (i+1 != iccidsFound) {
                    if (!silent) {
                        // printf("\n");
                        strcat(temp_profiles, "\n");
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
                Log.info("\n%s", temp_profiles);
                if (profilesBuffer && ((int)strlen(temp_profiles) < profilesBufferLen)) {
                    strncpy(profilesBuffer, temp_profiles, profilesBufferLen);
                }
            }
        } else {
            Log.info("[]");
        }
    } else {
        Log.info("[]");
    }

    return matched;
}

int ModemManager::esimEnable(char* specifiedIccid) {
    return enableDisableProfile(ICCID_ENABLE, specifiedIccid, RADIO_UNKNOWN);
}

int ModemManager::esimDisable(char* specifiedIccid) {
    return enableDisableProfile(ICCID_DISABLE, specifiedIccid, RADIO_UNKNOWN);
}

int ModemManager::findIccidByType(const char* inputBuffer, int inputBufferLen, char* matchedIccid, int radioType) {
    const char* p = inputBuffer;

    while ((p = strchr(p, '[')) != NULL) {
        p++;
        const char* end = strchr(p, ',');
        if (!end || (end - p) < ICCID_PREFIX_LEN) {
            p++;
            continue;
        }

        if ((radioType == RADIO_CELLULAR && strncmp(p, ICCID_TWILIO_PREFIX, ICCID_PREFIX_LEN) == 0) ||
                (radioType == RADIO_SATELLITE && strncmp(p, ICCID_SKYLO_PREFIX, ICCID_PREFIX_LEN) == 0))
        {
            int len = end - p;
            if (len >= inputBufferLen) {
                len = inputBufferLen - 1;
            }
            strncpy(matchedIccid, p, len);
            matchedIccid[len] = 0;

            return 0;
        }

        p = end;
    }

    return -1;
}

void ModemManager::updateCachedRadioType(char* iccid) {
    if (strncmp(iccid, ICCID_TWILIO_PREFIX, ICCID_PREFIX_LEN) == 0) {
        cachedRadioType_ = RADIO_CELLULAR;
    } else if (strncmp(iccid, ICCID_SKYLO_PREFIX, ICCID_PREFIX_LEN) == 0) {
        cachedRadioType_ = RADIO_SATELLITE;
    } else {
        cachedRadioType_ = RADIO_UNKNOWN;
    }
}

radio_type_t ModemManager::radioEnabled() {
    if (cachedRadioType_ == RADIO_UNKNOWN) {
        char iccid[ICCID_LEN + 1] = {0};
        getICCID(iccid, /* log results */ false);
    }

    // switch (cachedRadioType_) {
    //     case RADIO_UNKNOWN: Log.info("RADIO_UNKNOWN"); break;
    //     case RADIO_CELLULAR: Log.info("RADIO_CELLULAR"); break;
    //     case RADIO_SATELLITE: Log.info("RADIO_SATELLITE"); break;
    // };
    // delay(1000);

    return cachedRadioType_;
}

int ModemManager::radioEnable(radio_type_t radioType) {
    // Find the ICCID by radio type
    esimProfiles(NULL, profiles, PROFILES_SIZE_MAX);

    char specifiedIccid[ICCID_LEN + 1] = {0};
    if (findIccidByType(profiles, strlen(profiles), specifiedIccid, radioType) != 0) {
        Log.error("Could not find requested radio_type!");
        return -1;
    }

    if (enableDisableProfile(ICCID_ENABLE, specifiedIccid, radioType) == ENABLE_DISABLE_SUCCESS) {
        cachedRadioType_ = radioType;
    }

    return 0;
}

int ModemManager::waitAtResponse(unsigned int tries, unsigned int timeout) {
    unsigned int attempt = 0;
    for (;;) {
        const int r = Cellular.command(timeout, "AT\r\n");
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

    Cellular.command(2000, "AT+QGMR\r\n");

    return 0;
}

} // namespace particle


