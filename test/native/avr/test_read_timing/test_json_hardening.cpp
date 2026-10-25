/*
 * Project Name: Firestarter
 * Copyright (c) 2024 Henrik Olsson
 *
 * Permission is hereby granted under MIT license.
 *
 * JSON parser hardening cases: numeric overflow, nested values, token
 * lookahead and bus-config line validation. main() in
 * test_read_timing_params.cpp runs them through run_json_hardening_tests().
 */

#include <Arduino.h>
#include <unity.h>

#include <string.h>

extern "C" {
#include "json_parser.h"
#include "jsmn.h"
}
#include "firestarter.h"

static jsmntok_t tokens[NUMBER_JSNM_TOKENS];

static int tokenize(const char* json) {
    jsmn_parser parser;
    jsmn_init(&parser);
    return jsmn_parse(&parser, json, strlen(json), tokens, NUMBER_JSNM_TOKENS);
}

static int parse(const char* json, firestarter_handle_t* h) {
    int n = tokenize(json);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "test JSON must tokenize");
    return json_parse(json, tokens, n, h);
}

static uint8_t get_cmd(const char* json, firestarter_handle_t* h) {
    int n = tokenize(json);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "test JSON must tokenize");
    return json_get_cmd(json, tokens, n, h);
}

static int parse_config(const char* json, rurp_configuration_t* config,
                        firestarter_handle_t* h) {
    int n = tokenize(json);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "test JSON must tokenize");
    return json_parse_config(json, tokens, n, config, h);
}

/* ---- henols/firestarter#130: numeric overflow ---- */

void test_overflow_uint32_max_is_accepted(void) {
    firestarter_handle_t h = {};
    TEST_ASSERT_EQUAL_INT(0, parse("{\"cmd\":2,\"memory-size\":4294967295}", &h));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, h.mem_size);
    TEST_ASSERT_EQUAL_INT(0, parse("{\"cmd\":2,\"memory-size\":0}", &h));
    TEST_ASSERT_EQUAL_UINT32(0, h.mem_size);
}

void test_overflow_uint32_max_plus_one_is_refused(void) {
    firestarter_handle_t h = {};
    /* 4294967296 wraps to 0, 4294967300 to 4 and 42949672960 to 0. */
    TEST_ASSERT_EQUAL_INT(-1, parse("{\"cmd\":2,\"memory-size\":4294967296}", &h));
    TEST_ASSERT_EQUAL_INT(-1, parse("{\"cmd\":2,\"pulse-delay\":4294967300}", &h));
    TEST_ASSERT_EQUAL_INT(-1, parse("{\"cmd\":2,\"address\":42949672960}", &h));
}

void test_overflow_long_decimal_string_is_refused(void) {
    firestarter_handle_t h = {};
    TEST_ASSERT_EQUAL_INT(-1, parse("{\"cmd\":2,\"vpp_mv\":123456789012345678901234567890}", &h));
}

void test_overflow_on_a_saturating_field_is_refused_not_saturated(void) {
    firestarter_handle_t h = {};
    /* A value that fits 32 bits still saturates to the field width. */
    TEST_ASSERT_EQUAL_INT(0, parse("{\"cmd\":2,\"algorithm\":4294967295}", &h));
    TEST_ASSERT_EQUAL_UINT8(0xFF, h.protocol);
    /* 4294967301 wraps to 5 (PROTO_FLASH_5V_PAGE): refuse the frame. */
    TEST_ASSERT_EQUAL_INT(-1, parse("{\"cmd\":2,\"algorithm\":4294967301}", &h));
}

void test_overflow_flag_does_not_leak_into_the_next_frame(void) {
    firestarter_handle_t h = {};
    TEST_ASSERT_EQUAL_INT(-1, parse("{\"cmd\":2,\"chip-id\":99999999999}", &h));
    TEST_ASSERT_EQUAL_INT(0, parse("{\"cmd\":2,\"chip-id\":4660}", &h));
    TEST_ASSERT_EQUAL_UINT16(4660, h.chip_id);
}

void test_overflow_in_cmd_or_flags_gives_no_command(void) {
    firestarter_handle_t h = {};
    TEST_ASSERT_EQUAL_UINT8(CMD_READ, get_cmd("{\"cmd\":1,\"flags\":5}", &h));
    TEST_ASSERT_EQUAL_UINT16(5, h.ctrl_flags);
    /* 4294967297 wraps to 1 = CMD_READ. */
    TEST_ASSERT_EQUAL_UINT8(0xFF, get_cmd("{\"cmd\":4294967297}", &h));
    TEST_ASSERT_EQUAL_UINT8(0xFF, get_cmd("{\"cmd\":1,\"flags\":4294967296}", &h));
    /* 257 fits 32 bits but not the command byte: it must not become 1. */
    TEST_ASSERT_EQUAL_UINT8(0xFF, get_cmd("{\"cmd\":257}", &h));
    TEST_ASSERT_EQUAL_UINT8(0xFF, get_cmd("{\"state\":255}", &h));
}

void test_overflow_in_a_config_frame_is_refused(void) {
    firestarter_handle_t h = {};
    rurp_configuration_t config = {};
    TEST_ASSERT_EQUAL_INT(0, parse_config("{\"state\":12,\"flags\":3}", &config, &h));
    TEST_ASSERT_EQUAL_INT(-1, parse_config("{\"state\":12,\"flags\":4294967299}", &config, &h));
}

void run_json_hardening_tests(void) {
    RUN_TEST(test_overflow_uint32_max_is_accepted);
    RUN_TEST(test_overflow_uint32_max_plus_one_is_refused);
    RUN_TEST(test_overflow_long_decimal_string_is_refused);
    RUN_TEST(test_overflow_on_a_saturating_field_is_refused_not_saturated);
    RUN_TEST(test_overflow_flag_does_not_leak_into_the_next_frame);
    RUN_TEST(test_overflow_in_cmd_or_flags_gives_no_command);
    RUN_TEST(test_overflow_in_a_config_frame_is_refused);
}
