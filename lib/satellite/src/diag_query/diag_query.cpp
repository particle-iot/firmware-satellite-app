/* 
 * Project myProject
 * Author: Your Name
 * Date: 
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/firmware/best-practices/firmware-template/
 */

// Include Particle Device OS APIs
#include "Particle.h"
#include "diag_query.h"
#include <spark_wiring_logging.h>

int getDiagValueUint(uint8_t id, uint32_t* res) {
    // Retrieve the source for diag

    const diag_source* DiagSource = nullptr;
    int result = diag_get_source((diag_id)id, &DiagSource, nullptr);

    if (result) {
        return result;
    }
    
    if (DiagSource != nullptr && DiagSource->callback != nullptr) {
        // Prepare command data
        diag_source_get_cmd_data cmdData;
        cmdData.size = sizeof(diag_source_get_cmd_data);
        cmdData.reserved = 0;
        cmdData.data = res;
        cmdData.data_size = sizeof(uint32_t);

        // Call the diag source's callback function to get the diag value
        result = DiagSource->callback(DiagSource, DIAG_SOURCE_CMD_GET, &cmdData);
    }

    return Error::NONE;
}

int getDiagValueInt(uint8_t id, int32_t* res) {
    // Retrieve the source for diag
    const diag_source* DiagSource = nullptr;
    int result = diag_get_source((diag_id)id, &DiagSource, nullptr);

    if (result) {
        return result;
    }
    
    if (DiagSource != nullptr && DiagSource->callback != nullptr) {
        // Prepare command data
        diag_source_get_cmd_data cmdData;
        cmdData.size = sizeof(diag_source_get_cmd_data);
        cmdData.reserved = 0;
        cmdData.data = res;
        cmdData.data_size = sizeof(int32_t);

        // Call the diag source's callback function to get the diag value
        result = DiagSource->callback(DiagSource, DIAG_SOURCE_CMD_GET, &cmdData);
    }

    return Error::NONE;
}

void uintToBytes(unsigned int value, std::vector<uint8_t>* bytes) {

    for (size_t i = 0; i < sizeof(unsigned int); ++i) {
        uint8_t byte = (value >> (i * 8)) & 0xFF;
        bytes->push_back(byte);
    }

    std::reverse(bytes->begin(), bytes->end());
}

void intToBytes(int value, std::vector<uint8_t>* bytes) {

    for (size_t i = 0; i < sizeof(int); ++i) {
        uint8_t byte = (value >> (i * 8)) & 0xFF;
        bytes->push_back(byte);
    }

    std::reverse(bytes->begin(), bytes->end());
}

int getDiagnosticValue(uint32_t id, std::vector<uint8_t>* res) {
    
    int result = Error::NONE;

    const diag_source* DiagSource = nullptr;
    result = diag_get_source((diag_id)id, &DiagSource, nullptr);

    if (DiagSource == nullptr) {
        return Error::INVALID_STATE;
    }

    switch (DiagSource->type) {
        case DIAG_TYPE_INT: {
            int32_t val = 0;
            result = getDiagValueInt((diag_id)id, &val);
            Log.printf(LOG_LEVEL_TRACE, "Diag: %lu --- type: %d --- Value: %ld\r\n", id, DiagSource->type, val);
            intToBytes(val, res);
            break;
        }
        case DIAG_TYPE_UINT: {
            uint32_t val = 0;
            result = getDiagValueUint((diag_id)id, &val);
            Log.printf(LOG_LEVEL_TRACE, "Diag: %lu --- type: %d --- Value: %lu\r\n", id, DiagSource->type, val);
            uintToBytes(val, res);
            break;
        }
        default:
            break;
    }
    return result;
}