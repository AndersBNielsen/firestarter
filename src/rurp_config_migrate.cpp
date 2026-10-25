/*
 * Project Name: Firestarter
 * Copyright (c) 2026 Henrik Olsson
 *
 * Permission is hereby granted under MIT license.
 */

#include "rurp_config_migrate.h"

#include <string.h>

#include "rurp_shield.h"
#include "rurp_voltage_math.h"

#define NO_HARDWARE_REVISION_OVERRIDE 0xFF

static bool r1_in_band(long r1) {
    return r1 >= VALUE_R1_MIN && r1 <= VALUE_R1_MAX;
}

static bool r2_in_band(long r2) {
    return r2 >= VALUE_R2_MIN && r2 <= VALUE_R2_MAX;
}

static bool bandgap_in_band(uint16_t bandgap_mv) {
    return bandgap_mv >= RURP_BANDGAP_MIN_MV && bandgap_mv <= RURP_BANDGAP_MAX_MV;
}

static bool hardware_revision_known(uint8_t revision) {
    if (revision == NO_HARDWARE_REVISION_OVERRIDE) {
        return true;
    }
#ifdef HARDWARE_REVISION
    // REVISION_0 is 0, so the lower bound needs no test.
    return revision <= REVISION_2_3;
#else
    // A build without revision support has no revision to override with.
    return false;
#endif
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Every configuration this firmware has written carries "VER" and two digits.
static bool is_config_version(const char* version) {
    return version[0] == 'V' && version[1] == 'E' && version[2] == 'R' &&
           is_digit(version[3]) && is_digit(version[4]) && version[5] == '\0';
}

bool rurp_config_is_valid(const rurp_configuration_t* config) {
    return r1_in_band(config->r1) && r2_in_band(config->r2) &&
           bandgap_in_band(config->bandgap_mv) &&
           hardware_revision_known(config->hardware_revision);
}

bool rurp_config_commit(rurp_configuration_t* live, const rurp_configuration_t* candidate) {
    if (!rurp_config_is_valid(candidate)) {
        return false;
    }
    *live = *candidate;
    return true;
}

bool rurp_config_migrate(rurp_configuration_t* config) {
    if (!is_config_version(config->version)) {
        // Nothing in a foreign record is trusted, the shield override least
        // of all: a stray 0x00 there would select REVISION_0.
        config->r1 = VALUE_R1;
        config->r2 = VALUE_R2;
        config->hardware_revision = NO_HARDWARE_REVISION_OVERRIDE;
        config->bandgap_mv = (uint16_t)RURP_BANDGAP_NOMINAL_MV;
        strcpy(config->version, CONFIG_VERSION);
        return true;
    }

    bool changed = false;

    if (!r1_in_band(config->r1)) {
        config->r1 = VALUE_R1;
        changed = true;
    }
    if (!r2_in_band(config->r2)) {
        config->r2 = VALUE_R2;
        changed = true;
    }
    if (!bandgap_in_band(config->bandgap_mv)) {
        config->bandgap_mv = (uint16_t)RURP_BANDGAP_NOMINAL_MV;
        changed = true;
    }
    if (!hardware_revision_known(config->hardware_revision)) {
        config->hardware_revision = NO_HARDWARE_REVISION_OVERRIDE;
        changed = true;
    }
    if (strncmp(config->version, CONFIG_VERSION, sizeof(config->version)) != 0) {
        strcpy(config->version, CONFIG_VERSION);
        changed = true;
    }
    return changed;
}
