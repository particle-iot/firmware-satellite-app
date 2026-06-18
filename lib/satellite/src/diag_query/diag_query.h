#pragma once

#include <vector>
#include <spark_wiring_variant.h>

int getDiagnosticValue(uint32_t id, std::vector<uint8_t>* res);
int getDiagnosticValue(uint32_t id, particle::Variant& out);
