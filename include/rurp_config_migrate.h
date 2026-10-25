/*
 * Project Name: Firestarter
 * Copyright (c) 2026 Henrik Olsson
 *
 * Permission is hereby granted under MIT license.
 */

#ifndef RURP_CONFIG_MIGRATE_H
#define RURP_CONFIG_MIGRATE_H

#include <stdbool.h>

#include "rurp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate and migrate stored configuration fields without accessing storage.
 * Range checks run even when the configuration version matches, so stale
 * calibration values do not survive indefinitely. Preserve hardware_revision,
 * including 0xFF (no override), across migrations between Firestarter
 * configuration versions.
 *
 * A record whose version is not a Firestarter configuration version ("VER"
 * and two digits) is foreign: another sketch's EEPROM bytes, or a store that
 * holds no record. Every field of it resets to the default, and
 * hardware_revision resets to 0xFF, so foreign bytes never become a shield
 * override. A hardware_revision that is not 0xFF and not a known revision
 * also resets to 0xFF. Returns true when any field changed. */
bool rurp_config_migrate(rurp_configuration_t* config);

#ifdef __cplusplus
}
#endif

#endif  // RURP_CONFIG_MIGRATE_H
