/*
 * Project Name: Firestarter
 * Copyright (c) 2024 Henrik Olsson
 *
 * Permission is hereby granted under MIT license.
 */

#include "json_parser.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "jsmn.h"
#include "rurp_voltage_math.h"

uint8_t get_cmd(const char* json, jsmntok_t* tokens, int pos);

int parse_bus_config(const char* json, jsmntok_t* tokens, int token_count, firestarter_handle_t* handle);

bool get_flags(const char* json, jsmntok_t* tokens, int pos, firestarter_handle_t* handle);

bool get_rw_pin(const char* json, jsmntok_t* tokens, int pos, firestarter_handle_t* handle);
bool get_vpp_pin(const char* json, jsmntok_t* tokens, int pos, firestarter_handle_t* handle);

bool get_r1(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config);
bool get_r2(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config);
bool get_rev(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config);
#if RURP_HAS_BANDGAP_CAL
bool get_vcc(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config);
bool get_bg(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config);
#endif

/*
 * The operator's measured supply voltage, in millivolts, for a calibration.
 *
 * It is NOT a stored field, so it has no home in rurp_configuration_t: it is
 * an input the firmware consumes once to back-solve this MCU's bandgap. Reset
 * at the top of json_parse_config so a value can never survive into a later
 * command.
 */
static long s_measured_vcc_mv = 0;

static int jsoneq_(const char* json, jsmntok_t* tok, const char* s);

/*
 * Set when a number does not fit in 32 bits. json_get_cmd, json_parse and
 * json_parse_config clear it on entry and refuse the frame when it is set, so
 * a wrapped value never reaches a field's saturate or mask policy.
 */
static bool s_number_overflow = false;

static unsigned long simple_strtoul(const char* s) {
    uint32_t val = 0;
    // Note: This simple implementation only handles positive decimal numbers.
    while (*s >= '0' && *s <= '9') {
        uint8_t digit = (uint8_t)(*s - '0');
        // val * 10 cannot wrap below this bound; the sum wraps only when the
        // result ends up smaller than the digit that was added.
        if (val > 0xFFFFFFFFUL / 10) {
            s_number_overflow = true;
        }
        val = val * 10 + digit;
        if (val < digit) {
            s_number_overflow = true;
        }
        s++;
    }
    return val;
}

#define jsoneq(json, tok, s) \
    jsoneq_(json, tok, PSTR(s))

/*
 * Read-timing sweep knobs, clamped at parse time so an absurd JSON value
 * cannot reach delayMicroseconds() unbounded. Values < 3 us are below
 * delayMicroseconds() accuracy on a 16 MHz AVR.
 *
 *   read_settling_us == 0 -> no settling delay
 *   read_strobe_us   == 0 -> firmware default 3 us
 *
 * Must be defined above key_parsers[]: the table's `clamp` column references it.
 */
#define READ_TIMING_MAX_US 1000UL   /* sane max (~1ms); caps both knobs */

const char key_mem_size[] PROGMEM = "memory-size";
const char key_address[] PROGMEM = "address";
const char key_flags[] PROGMEM = "flags";
const char key_chip_id[] PROGMEM = "chip-id";
const char key_pin_count[] PROGMEM = "pin-count";
const char key_pulse_delay[] PROGMEM = "pulse-delay";
const char key_vpp_mv[] PROGMEM = "vpp_mv";
const char key_algorithm[] PROGMEM = "algorithm";
/* Host-tunable read-timing knobs. */
const char key_read_settling[] PROGMEM = "read-settling-delay";
const char key_read_strobe[]   PROGMEM = "read-strobe-us";
/* Per-chip page-write size delivered by the host.
 * Wire key is the HYPHEN form "page-size" -- the internal database key
 * programming.page_size uses an underscore, so a PROGMEM string written
 * against the underscore form would silently never match. */
const char key_page_size[]     PROGMEM = "page-size";
/* Absolute, exclusive end address of the operation's region.
 * Wire key is the HYPHEN form "region-end" -- a PROGMEM string written
 * against an underscore form would silently never match, exactly like
 * key_page_size's warning above. */
const char key_region_end[]    PROGMEM = "region-end";

/*
 * field_desc_t -- one row per wire key: {key, clamp, offset, width}.
 *
 * 6 B on AVR because avr-gcc gives every type 1-byte struct alignment;
 * larger and padded on a 64-bit host, where PGM_P is an 8-byte pointer and
 * the compiler inserts alignment padding after the narrower columns. NO
 * ASSERTION ON sizeof(field_desc_t) MAY BE AUTHORED WITHOUT A TARGET GUARD
 * for that reason -- see include/eprom_params.h:52-67's own written warning
 * about exactly this trap (PATTERNS §4).
 *
 * clamp == 0 means "no clamp".
 */
typedef struct {
    PGM_P key;
    uint16_t clamp;
    uint8_t offset;
    uint8_t width;
} field_desc_t;

/*
 * width's low nibble (FIELD_WIDTH_MASK) is the byte width derived from the
 * member. Bit 7 (FIELD_POLICY_MASK), when set, selects MASK semantics for
 * an out-of-range value instead of SATURATE. Bit 6 (FIELD_POLICY_REJECT_NEGATIVE),
 * when set, refuses the whole frame -- see that macro's own comment below.
 */
#define FIELD_WIDTH_MASK  0x0F
#define FIELD_POLICY_MASK 0x80

/* simple_strtoul parses a leading '-' as zero. Reject a negative address
 * instead of silently treating it as address 0; other fields retain their
 * established wire parsing policies. */
#define FIELD_POLICY_REJECT_NEGATIVE 0x40

/* Derive descriptor offsets and widths from firestarter_handle_t itself.
 * AVR and native struct layouts differ; hard-coded offsets would write the
 * wrong fields, and store_field relies on these compiler-derived sizes. */
#define FIELD(k, member, cl)                                                 \
    { (k), (uint16_t)(cl), (uint8_t)offsetof(firestarter_handle_t, member),  \
      (uint8_t)sizeof(((firestarter_handle_t*)0)->member) }

/* Mask bitfields to their storage width instead of saturating: saturation
 * of ctrl_flags could enable FLAG_FORCE, FLAG_SKIP_ERASE and VPE_AS_VPP
 * together. Width-limited masking preserves the existing wire policy. */
#define FIELD_MASK(k, member)                                                \
    { (k), (uint16_t)0, (uint8_t)offsetof(firestarter_handle_t, member),     \
      (uint8_t)(sizeof(((firestarter_handle_t*)0)->member) | FIELD_POLICY_MASK) }

/*
 * FIELD_REJECT_NEGATIVE -- SATURATE policy (same as FIELD above), plus the
 * dispatch loop's sign check ORs in FIELD_POLICY_REJECT_NEGATIVE. Applied
 * to the key_address row only -- see that macro's own comment above.
 */
#define FIELD_REJECT_NEGATIVE(k, member, cl)                                \
    { (k), (uint16_t)(cl), (uint8_t)offsetof(firestarter_handle_t, member), \
      (uint8_t)(sizeof(((firestarter_handle_t*)0)->member) | FIELD_POLICY_REJECT_NEGATIVE) }

static const field_desc_t key_parsers[] PROGMEM = {
    
    FIELD(key_mem_size, mem_size, 0),
    
    FIELD_REJECT_NEGATIVE(key_address, address, 0),
    
    FIELD_MASK(key_flags, ctrl_flags),
    
    FIELD(key_chip_id, chip_id, 0),
    
    FIELD(key_pin_count, pins, 0),
    
    FIELD(key_pulse_delay, pulse_delay, 0),
    
    FIELD(key_vpp_mv, vpp_mv, 0),
    
    FIELD(key_algorithm, protocol, 0),
    
    FIELD(key_read_settling, read_settling_us, READ_TIMING_MAX_US),
    
    FIELD(key_read_strobe, read_strobe_us, READ_TIMING_MAX_US),
    
    FIELD(key_page_size, page_size, 0),
    
    FIELD(key_region_end, region_end, 0),
};

/* Ensure all descriptor offsets fit uint8_t and their fields fit a 32-bit
 * store. Native parsing tests additionally verify each descriptor's target. */
_Static_assert(offsetof(firestarter_handle_t, mem_size) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->mem_size) <= 4,
               "mem_size: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, address) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->address) <= 4,
               "address: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, ctrl_flags) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->ctrl_flags) <= 4,
               "ctrl_flags: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, chip_id) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->chip_id) <= 4,
               "chip_id: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, pins) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->pins) <= 4,
               "pins: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, pulse_delay) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->pulse_delay) <= 4,
               "pulse_delay: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, vpp_mv) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->vpp_mv) <= 4,
               "vpp_mv: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, protocol) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->protocol) <= 4,
               "protocol: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, read_settling_us) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->read_settling_us) <= 4,
               "read_settling_us: a struct reorder moved it past the uint8_t offset column's "
               "range, or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, read_strobe_us) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->read_strobe_us) <= 4,
               "read_strobe_us: a struct reorder moved it past the uint8_t offset column's "
               "range, or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, page_size) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->page_size) <= 4,
               "page_size: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(offsetof(firestarter_handle_t, region_end) < 256 &&
                   sizeof(((firestarter_handle_t*)0)->region_end) <= 4,
               "region_end: a struct reorder moved it past the uint8_t offset column's range, "
               "or gave it a width the 32-bit store cannot carry");
_Static_assert(sizeof(key_parsers) / sizeof(key_parsers[0]) == 12,
               "key_parsers row count changed -- add or remove the matching per-member offset "
               "guard above to match");

/*
 * The single shared write body every table row dispatches through.
 *
 * `field` points INTO PROGMEM: read every column with pgm_read_byte /
 * pgm_read_word, NEVER by dereferencing *field, which compiles and silently
 * returns RAM garbage on AVR.
 *
 * `value` is uint32_t, not `unsigned long`: simple_strtoul returns unsigned
 * long, which is 32-bit on AVR but 64-bit on native x86-64. Fixing it at 4
 * bytes makes the saturation branch behave identically on both, so the native
 * round-trip tests are valid oracles for the AVR build. Do not "simplify" it
 * back.
 */
static void store_field(firestarter_handle_t* handle, const field_desc_t* field, uint32_t value) {
    uint8_t offset = pgm_read_byte(&field->offset);
    uint8_t width_raw = pgm_read_byte(&field->width);
    uint16_t clamp = pgm_read_word(&field->clamp);

    /* 1. The read-timing bound now lives here, as the clamp column. */
    if (clamp != 0 && value > (uint32_t)clamp) {
        value = clamp;
    }

    uint8_t width = width_raw & FIELD_WIDTH_MASK;
    bool mask_policy = (width_raw & FIELD_POLICY_MASK) != 0;

    /* 2/3. Saturate to the member's own maximum unless the row's policy bit
     * selects MASK instead, in which case the value is left untouched and
     * the width-limited store below performs the truncation. The
     * width < sizeof(uint32_t) guard keeps the shift out of undefined
     * behaviour (a 32-bit shift by 32) -- keep it. */
    if (width < sizeof(uint32_t)) {
        uint32_t max_value = ((uint32_t)1u << (width * 8)) - 1u;
        if (value > max_value && !mask_policy) {
            value = max_value;
        }
    }

    /* 4. Little-endian assumption, stated explicitly: AVR, native x86-64
     * and the PY32F071 ARM port are all little-endian, so copying the low
     * `width` bytes of `value` into the handle at the compiler-derived
     * `offset` lands correctly on every target this firmware builds for. */
    memcpy((uint8_t*)handle + offset, &value, width);
}

/* Reset only command-scoped parser state. Never clear data_buffer, data_size,
 * cmd, operation state, callbacks or persistent board calibration.
 *
 * The handle is reused between commands; omission on the wire must never
 * inherit a previous chip's protocol, pulse width, VPP target or bus mapping.
 * Use zero for optional scalar defaults (pulse-delay 0 requests the existing
 * protocol default). No temporary copy of the handle is needed on AVR.
 */
static void json_reset_command_parameters(firestarter_handle_t* handle) {
    s_number_overflow = false;
    handle->protocol = 0;
    handle->pins = 0;
    handle->mem_size = 0;
    handle->address = 0;
    handle->vpp_mv = 0;
    handle->pulse_delay = 0;
    handle->read_settling_us = 0;
    handle->read_strobe_us = 0;
    handle->ctrl_flags = 0;
    handle->chip_id = 0;
    handle->page_size = 0;
    handle->region_end = 0;

    memset(&handle->bus_config, 0, sizeof(handle->bus_config));
    memset(handle->bus_config.address_lines, 0xFF,
           sizeof(handle->bus_config.address_lines));
    handle->bus_config.rw_line = 0xFF;
    handle->bus_config.vpp_line = 0xFF;
}

int json_parse(const char* json, jsmntok_t* tokens, int token_count, firestarter_handle_t* handle) {
    json_reset_command_parameters(handle);

    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        return -1; // Not a JSON object
    }

    int num_pairs = tokens[0].size;
    int token_idx = 1;

    for (int i = 0; i < num_pairs; i++) {
        if (token_idx >= token_count) {
            return -1; // Should not happen with valid JSON
        }

        jsmntok_t* key_token = &tokens[token_idx];

        // The 'cmd' key is handled by json_get_cmd before this function is called.
        // We just need to identify and skip it here.
        if (jsoneq(json, key_token, "cmd") == 0 || jsoneq(json, key_token, "state") == 0) {
            token_idx += 2;
            continue;
        }

        bool found = false;
        for (size_t j = 0; j < sizeof(key_parsers) / sizeof(key_parsers[0]); j++) {
            PGM_P key = (PGM_P)pgm_read_ptr(&key_parsers[j].key);
            if (jsoneq_(json, key_token, key) == 0) {
                const char* value_str = json + tokens[token_idx + 1].start;
                uint8_t width_raw = pgm_read_byte(&key_parsers[j].width);
                if ((width_raw & FIELD_POLICY_REJECT_NEGATIVE) != 0 && *value_str == '-') {
                    return -1; // Refused: this row's value cannot represent a negative number
                }
                store_field(handle, &key_parsers[j], simple_strtoul(value_str));
                token_idx += 2;
                found = true;
                break;
            }
        }

        if (found) {
            continue;
        }

        if (jsoneq(json, key_token, "bus-config") == 0) {
            int consumed = parse_bus_config(json, &tokens[token_idx], token_count - token_idx, handle);
            if (consumed < 0) return -1;
            token_idx += 1 + consumed;
        } else {
            // Unknown field — skip key + value token (forward-compatible with new Python fields)
            token_idx += 2;
        }
    }
    if (s_number_overflow) {
        return -1; // A number did not fit in 32 bits
    }
    if (handle->bus_config.address_lines[0] == 0xFF) {
        handle->bus_config.address_mask = 0xFFFF;
    }
    return 0;
}

/* Every current host memory command includes these four fields, even for
 * 5V-only families. A zero here means omitted or unusable data; refuse it
 * before configuring hardware rather than falling back to prior state.
 *
 * Optional pulse-delay, chip-id, read timings and bus-config deliberately
 * remain optional. pulse-delay 0 means "use protocol default" on EPROMs.
 */
bool json_memory_params_valid(const firestarter_handle_t* handle) {
    if (handle->mem_size == 0 || handle->protocol == 0 ||
        handle->pins == 0 || handle->vpp_mv == 0) {
        return false;
    }
    // Reject bad cursors before configure_memory() touches bus registers.
    // Read uses the whole device; writes may have an exclusive region end.
    if (handle->cmd == CMD_READ) {
        return handle->address <= handle->mem_size;
    }
    if (handle->cmd == CMD_WRITE) {
        uint32_t end = handle->region_end;
        if (end == 0 || end > handle->mem_size) {
            end = handle->mem_size;
        }
        return handle->address <= end;
    }
    return true;
}

int json_parse_config(const char* json, jsmntok_t* tokens, int token_count, rurp_configuration_t* config, firestarter_handle_t* handle) {
    int res = 0;
    s_measured_vcc_mv = 0;  // never let a value survive into a later command
    s_number_overflow = false;
    for (int i = 1; i < token_count; i++) {
        if (get_cmd(json, tokens, i) != 0xFF) {
            i++;
        } else if (get_flags(json, tokens, i, handle)) {
            i++;
        }
#ifdef HARDWARE_REVISION
        else if (get_rev(json, tokens, i, config)) {
            i++;
            res |= JSON_CFG_FIELD_CHANGED;
        }
#endif
        else if (get_r1(json, tokens, i, config)) {
            i++;
            res |= JSON_CFG_FIELD_CHANGED;
        } else if (get_r2(json, tokens, i, config)) {
            i++;
            res |= JSON_CFG_FIELD_CHANGED;
        }
#if RURP_HAS_BANDGAP_CAL
        else if (get_vcc(json, tokens, i, config)) {
            i++;
            res |= JSON_CFG_CALIBRATE;
        } else if (get_bg(json, tokens, i, config)) {
            i++;
            res |= JSON_CFG_FIELD_CHANGED;
        }
#endif
        else {
            // Includes vcc and bg on a platform without bandgap calibration.
            return -1;
        }
    }
    return s_number_overflow ? -1 : res;
}

uint8_t json_get_cmd(const char* json, jsmntok_t* tokens, int token_count, firestarter_handle_t* handle) {
    handle->cmd = 0xFF;
    handle->ctrl_flags = 0;
    s_number_overflow = false;
    bool found_flags = false;
    for (int i = 0; i < token_count; i++) {
        uint8_t cmd = get_cmd(json, tokens, i);
        if (cmd != 0xFF) {
            handle->cmd = cmd;
            i++;
        } else if (get_flags(json, tokens, i, handle)) {
            i++;
            found_flags = true;
        }
        if (handle->cmd != 0xFF && found_flags) {
            break;
        }
    }
    if (s_number_overflow) {
        handle->cmd = 0xFF;  // refused as a frame without a usable command
    }
    return handle->cmd;
}

uint8_t get_cmd(const char* json, jsmntok_t* tokens, int pos) {
    if (jsoneq(json, &tokens[pos], "cmd") == 0 || jsoneq(json, &tokens[pos], "state") == 0) {
        // A value above the uint8_t range must not truncate onto a real
        // command (257 would become CMD_READ); 0xFF means "no command".
        unsigned long cmd = simple_strtoul(json + tokens[pos + 1].start);
        return cmd > 0xFF ? 0xFF : (uint8_t)cmd;
    }
    return 0xFF;
}

int parse_bus_config(const char* json, jsmntok_t* tokens, int token_count, firestarter_handle_t* handle) {
    // tokens[0] is the "bus-config" key.
    // tokens[1] is the object token. Its `size` is the number of key-value pairs.
    if (token_count < 2 || tokens[1].type != JSMN_OBJECT) {
        return 0;
    }

    int num_pairs = tokens[1].size;
    int total_consumed_tokens = 1; // Account for the object token itself.
    int current_token_idx = 2;     // Start at the first key inside the object.

    for (int i = 0; i < num_pairs; i++) {
        if (current_token_idx >= token_count) {
            return -1; // Should not happen with valid JSON
        }
        jsmntok_t* key_token = &tokens[current_token_idx];

        if (jsoneq(json, key_token, "bus") == 0) {
            jsmntok_t* array_token = &tokens[current_token_idx + 1];
            if (array_token->type != JSMN_ARRAY) return -1;

            int bus_array_size = array_token->size;
            int bus_array_start_idx = current_token_idx + 1;

            handle->bus_config.matching_lines = 0xff;
            for (int j = 0; j < bus_array_size && j < ADDRESS_LINES_SIZE; j++) {
                handle->bus_config.address_lines[j] = simple_strtoul(json + tokens[bus_array_start_idx + j + 1].start);
                handle->bus_config.address_mask |= 1UL << handle->bus_config.address_lines[j];
                if (handle->bus_config.matching_lines == 0xff && handle->bus_config.address_lines[j] != j) {
                    handle->bus_config.matching_lines = j;
                }
            }
            if (handle->bus_config.matching_lines == 0xff) { handle->bus_config.matching_lines = bus_array_size; }
            if (bus_array_size < ADDRESS_LINES_SIZE) { handle->bus_config.address_lines[bus_array_size] = 0xFF; }

            int pair_tokens = 1 + 1 + bus_array_size; // key + array_token + elements
            total_consumed_tokens += pair_tokens;
            current_token_idx += pair_tokens;
        } else if (jsoneq(json, key_token, "static-high") == 0) {
            jsmntok_t* array_token = &tokens[current_token_idx + 1];
            if (array_token->type != JSMN_ARRAY) return -1;

            int sh_array_size = array_token->size;
            int sh_array_start_idx = current_token_idx + 1;
            for (int j = 0; j < sh_array_size; j++) {
                uint8_t line = simple_strtoul(json + tokens[sh_array_start_idx + j + 1].start);
                handle->bus_config.static_high_mask |= 1UL << line;
            }

            int pair_tokens = 1 + 1 + sh_array_size; // key + array_token + elements
            total_consumed_tokens += pair_tokens;
            current_token_idx += pair_tokens;
        } else if (get_rw_pin(json, tokens, current_token_idx, handle)) {
            total_consumed_tokens += 2;
            current_token_idx += 2;
        } else if (get_vpp_pin(json, tokens, current_token_idx, handle)) {
            total_consumed_tokens += 2;
            current_token_idx += 2;
        } else {
            // Unknown key — skip key + value tokens
            total_consumed_tokens += 2;
            current_token_idx += 2;
        }
    }
    return total_consumed_tokens;
}

static int jsoneq_(const char* json, jsmntok_t* tok, const char* s) {
    if (tok->type == JSMN_STRING && (int)strlen_P(s) == tok->end - tok->start &&
        strncmp_P(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

#define extract_num(element, register, type)           \
    if (jsoneq(json, &tokens[pos], element) == 0) {    \
        register = type(json + tokens[pos + 1].start); \
        return 1;                                      \
    }                                                  \
    return 0;

#define extract_long(element, register) \
    extract_num(element, register, simple_strtoul)

#define extract_int(element, register) extract_long(element, register)

/*
 * Hand-expanded rather than the extract_long macro form: that macro emits its
 * own anonymous PSTR("flags"), so expanding it here would store the wire key
 * twice. get_flags survives deliberately -- it is called directly from
 * json_parse_config and json_get_cmd, neither of which walks the field table.
 *
 * Truncating by assignment gives the same observable result as the table's
 * FIELD_MASK row, so all three `flags` paths stay consistent.
 */
bool get_flags(const char* json, jsmntok_t* tokens, int pos, firestarter_handle_t* handle) {
    if (jsoneq_(json, &tokens[pos], key_flags) == 0) {
        handle->ctrl_flags = simple_strtoul(json + tokens[pos + 1].start);
        return 1;
    }
    return 0;
}

bool get_rw_pin(const char* json, jsmntok_t* tokens, int pos, firestarter_handle_t* handle) {
    extract_int("rw-pin", handle->bus_config.rw_line);
}

bool get_vpp_pin(const char* json, jsmntok_t* tokens, int pos, firestarter_handle_t* handle) {
    extract_int("vpp-pin", handle->bus_config.vpp_line);
}

bool get_r1(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config) {
    extract_long("r1", config->r1);
}

bool get_r2(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config) {
    extract_long("r2", config->r2);
}

bool get_rev(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config) {
    extract_int("rev", config->hardware_revision);
}

#if RURP_HAS_BANDGAP_CAL
bool get_vcc(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config) {
    (void)config;
    extract_long("vcc", s_measured_vcc_mv);
}

/*
 * Write the stored bandgap directly. This is the escape hatch -- restoring the
 * nominal, or re-applying a calibration recorded elsewhere. The config command
 * refuses an out-of-band value before it is stored, so a bad one cannot reach
 * the conversion.
 */
bool get_bg(const char* json, jsmntok_t* tokens, int pos, rurp_configuration_t* config) {
    extract_int("bg", config->bandgap_mv);
}
#endif

long json_config_measured_vcc_mv(void) {
    return s_measured_vcc_mv;
}
