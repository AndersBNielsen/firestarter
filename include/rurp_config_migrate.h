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
 * including 0xFF (no override), across migrations. */
bool rurp_config_migrate(rurp_configuration_t* config);

#ifdef __cplusplus
}
#endif

#endif  // RURP_CONFIG_MIGRATE_H
