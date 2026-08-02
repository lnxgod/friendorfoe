#include <unity.h>

#include "../support/backend_test_main.h"

void test_backend_json_writer_escapes_and_fails_closed(void);
void test_backend_json_reader_typed_accessors(void);
void test_backend_json_reader_rejects_malformed_and_limits(void);
void test_detection_codec_round_trips_full_record(void);
void test_detection_codec_never_returns_partial_json(void);
void test_detection_codec_matches_independent_http_mapping(void);
void test_detection_codec_rejects_hostile_and_boundary_inputs(void);
void test_detection_assert_helper_detects_each_field_group(void);
void test_detection_frequency_channel_boundaries(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    BACKEND_RUN_TEST(test_backend_json_writer_escapes_and_fails_closed);
    BACKEND_RUN_TEST(test_backend_json_reader_typed_accessors);
    BACKEND_RUN_TEST(test_backend_json_reader_rejects_malformed_and_limits);
    BACKEND_RUN_TEST(test_detection_codec_round_trips_full_record);
    BACKEND_RUN_TEST(test_detection_codec_never_returns_partial_json);
    BACKEND_RUN_TEST(test_detection_codec_matches_independent_http_mapping);
    BACKEND_RUN_TEST(test_detection_codec_rejects_hostile_and_boundary_inputs);
    BACKEND_RUN_TEST(test_detection_assert_helper_detects_each_field_group);
    BACKEND_RUN_TEST(test_detection_frequency_channel_boundaries);
    return UNITY_END();
}
