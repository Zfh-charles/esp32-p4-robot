#include <cstdint>
#include <cstdio>

#include "mjpeg_playback_clock.h"
#include "mjpeg_playback_clock_player_adapter.h"

extern "C" mjpeg_playback_clock_poll_t mjpeg_playback_clock_player_poll_direct(
    const mjpeg_playback_clock_player_binding_t *binding,
    uint64_t now_us);
extern "C" mjpeg_playback_clock_effect_t
mjpeg_playback_clock_player_on_decode_result_direct(
    const mjpeg_playback_clock_player_binding_t *binding,
    mjpeg_playback_decode_result_t result,
    uint64_t now_us,
    uint32_t yield_interval);

namespace {

int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                              \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

mjpeg_playback_clock_state_t Clock(uint32_t normal_interval_us = 33333)
{
    mjpeg_playback_clock_state_t state{};
    CHECK(mjpeg_playback_clock_init(&state, normal_interval_us, 1000000));
    return state;
}

void CheckStateEqual(
    const mjpeg_playback_clock_state_t &actual,
    const mjpeg_playback_clock_state_t &expected)
{
    CHECK(actual.paused == expected.paused);
    CHECK(actual.frame_interval_us == expected.frame_interval_us);
    CHECK(actual.normal_frame_interval_us == expected.normal_frame_interval_us);
    CHECK(actual.deadline_us == expected.deadline_us);
    CHECK(actual.warmup_until_us == expected.warmup_until_us);
    CHECK(actual.soft_start_until_us == expected.soft_start_until_us);
    CHECK(actual.last_frame_time_us == expected.last_frame_time_us);
    CHECK(actual.last_decode_success_us == expected.last_decode_success_us);
    CHECK(actual.last_stall_diag_us == expected.last_stall_diag_us);
    CHECK(actual.current_frame == expected.current_frame);
    CHECK(actual.decode_error_count == expected.decode_error_count);
}

void CheckPollEqual(
    const mjpeg_playback_clock_poll_t &actual,
    const mjpeg_playback_clock_poll_t &expected)
{
    CHECK(actual.action == expected.action);
    CHECK(actual.wait_us == expected.wait_us);
    CHECK(actual.soft_start_finished == expected.soft_start_finished);
    CHECK(actual.stall_detected == expected.stall_detected);
    CHECK(actual.stall_diag == expected.stall_diag);
    CHECK(actual.yield_now == expected.yield_now);
}

void CheckEffectEqual(
    const mjpeg_playback_clock_effect_t &actual,
    const mjpeg_playback_clock_effect_t &expected)
{
    CHECK(actual.stream_ended == expected.stream_ended);
    CHECK(actual.delay_one_tick == expected.delay_one_tick);
    CHECK(actual.enter_error_state == expected.enter_error_state);
}

uint32_t NextRandom(uint32_t &state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

void TestResumeWarmupSoftStartAndRestore()
{
    auto state = Clock();
    mjpeg_playback_clock_resume(&state, 1000000, 33333);
    CHECK(!state.paused);
    CHECK(state.frame_interval_us == MJPEG_CLOCK_RESUME_MIN_INTERVAL_US);
    CHECK(state.deadline_us == 1000000 + MJPEG_CLOCK_WARMUP_US);

    auto poll = mjpeg_playback_clock_poll(
        &state, 1000000 + MJPEG_CLOCK_WARMUP_US - 1);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_WARMUP);
    CHECK(poll.wait_us == 5000);

    poll = mjpeg_playback_clock_poll(&state, 1000000 + MJPEG_CLOCK_WARMUP_US);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_DECODE);
    CHECK(!poll.soft_start_finished);

    state.last_decode_success_us = 1000000 + MJPEG_CLOCK_SOFT_START_US - 1000;
    state.deadline_us = 1000000 + MJPEG_CLOCK_SOFT_START_US;
    poll = mjpeg_playback_clock_poll(&state, 1000000 + MJPEG_CLOCK_SOFT_START_US);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_DECODE);
    CHECK(poll.soft_start_finished);
    CHECK(state.frame_interval_us == 33333);
}

void TestPauseAndResumeReplaceStaleDeadline()
{
    auto state = Clock(100000);
    state.deadline_us = 99;
    mjpeg_playback_clock_pause(&state);
    auto poll = mjpeg_playback_clock_poll(&state, 10000000);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_PAUSED);
    CHECK(poll.wait_us == 10000);

    mjpeg_playback_clock_resume(&state, 10000000, 100000);
    CHECK(state.deadline_us == 10000000 + MJPEG_CLOCK_WARMUP_US);
    CHECK(mjpeg_playback_clock_poll(&state, 10000000).action ==
          MJPEG_CLOCK_ACTION_WARMUP);
}

void TestWaitAndLateSuccessReanchorWithoutCatchup()
{
    auto state = Clock(100000);
    mjpeg_playback_clock_resume(&state, 0, 100000);
    const uint64_t late_us = MJPEG_CLOCK_WARMUP_US + 900000;
    CHECK(mjpeg_playback_clock_poll(&state, late_us).action ==
          MJPEG_CLOCK_ACTION_DECODE);
    auto effect = mjpeg_playback_clock_on_decode_result(
        &state, MJPEG_CLOCK_DECODE_OK, late_us, 4);
    CHECK(state.deadline_us == late_us + MJPEG_CLOCK_RESUME_MIN_INTERVAL_US);
    CHECK(!effect.enter_error_state);
    auto poll = mjpeg_playback_clock_poll(&state, late_us + 1);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_WAIT);
    CHECK(poll.wait_us == MJPEG_CLOCK_RESUME_MIN_INTERVAL_US - 1);
}

void TestSuccessResetsErrorsAndRequestsPeriodicYield()
{
    auto state = Clock(100000);
    mjpeg_playback_clock_resume(&state, 0, 100000);
    state.decode_error_count = 7;
    state.current_frame = 3;
    auto effect = mjpeg_playback_clock_on_decode_result(
        &state, MJPEG_CLOCK_DECODE_OK, 500000, 4);
    CHECK(state.current_frame == 4);
    CHECK(state.decode_error_count == 0);
    CHECK(state.last_decode_success_us == 500000);
    CHECK(effect.delay_one_tick);
}

void TestEofMakesNextLoopDueAndKeepsCursorOutside()
{
    auto state = Clock(100000);
    mjpeg_playback_clock_resume(&state, 0, 100000);
    auto effect = mjpeg_playback_clock_on_decode_result(
        &state, MJPEG_CLOCK_DECODE_EOF, 800000, 4);
    CHECK(effect.stream_ended);
    CHECK(state.deadline_us == 800000);
    CHECK(state.last_decode_success_us == 800000);
    CHECK(mjpeg_playback_clock_poll(&state, 800000).action ==
          MJPEG_CLOCK_ACTION_DECODE);
}

void TestEleventhErrorEntersErrorWithoutMovingDeadline()
{
    auto state = Clock(100000);
    state.deadline_us = 1234;
    for (int i = 0; i < 10; ++i) {
        auto effect = mjpeg_playback_clock_on_decode_result(
            &state, MJPEG_CLOCK_DECODE_ERROR, 5000 + i, 4);
        CHECK(!effect.enter_error_state);
        CHECK(state.deadline_us == 1234);
    }
    auto effect = mjpeg_playback_clock_on_decode_result(
        &state, MJPEG_CLOCK_DECODE_ERROR, 6000, 4);
    CHECK(effect.enter_error_state);
    CHECK(state.decode_error_count == 11);
    CHECK(state.deadline_us == 1234);
}

void TestStallReanchorsAndRateLimitsDiagnostic()
{
    auto state = Clock(100000);
    state.paused = false;
    state.frame_interval_us = 100000;
    state.normal_frame_interval_us = 100000;
    state.deadline_us = 0;
    state.last_decode_success_us = 100;
    state.last_stall_diag_us = 0;

    auto poll = mjpeg_playback_clock_poll(
        &state, 100 + MJPEG_CLOCK_HANG_THRESHOLD_US + 1);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_WAIT);
    CHECK(poll.stall_detected);
    CHECK(poll.stall_diag);
    CHECK(poll.yield_now);
    CHECK(state.deadline_us ==
          100 + MJPEG_CLOCK_HANG_THRESHOLD_US + 1 + 100000);

    const uint64_t again = 100 + MJPEG_CLOCK_HANG_THRESHOLD_US + 500000;
    poll = mjpeg_playback_clock_poll(&state, again);
    CHECK(poll.stall_detected);
    CHECK(!poll.stall_diag);
    CHECK(poll.yield_now);
}

void TestInvalidIntervalFailsClosed()
{
    mjpeg_playback_clock_state_t state{};
    CHECK(!mjpeg_playback_clock_init(&state, 0, 0));
}

void TestProductionAndLegacyRemainEquivalent()
{
    auto production = Clock(100000);
    auto legacy = production;
    production.paused = false;
    legacy.paused = false;
    production.deadline_us = legacy.deadline_us = 2000000;
    production.last_decode_success_us = legacy.last_decode_success_us = 1500000;
    const uint64_t times[] = {1900000, 2000000, 2100000, 3700001};
    for (uint64_t now : times) {
        auto actual = mjpeg_playback_clock_poll(&production, now);
        auto expected = mjpeg_playback_clock_legacy_poll(&legacy, now);
        CHECK(actual.action == expected.action);
        CHECK(actual.wait_us == expected.wait_us);
        CHECK(actual.stall_detected == expected.stall_detected);
        CHECK(production.deadline_us == legacy.deadline_us);
        CHECK(production.frame_interval_us == legacy.frame_interval_us);
    }
    auto actual = mjpeg_playback_clock_on_decode_result(
        &production, MJPEG_CLOCK_DECODE_OK, 4000000, 10);
    auto expected = mjpeg_playback_clock_legacy_on_decode_result(
        &legacy, MJPEG_CLOCK_DECODE_OK, 4000000, 10);
    CHECK(actual.delay_one_tick == expected.delay_one_tick);
    CHECK(production.current_frame == legacy.current_frame);
    CHECK(production.deadline_us == legacy.deadline_us);
}

void TestProductionAndLegacyTraceEquivalence()
{
    uint32_t random = 0x51C10C4U;
    for (int scenario = 0; scenario < 64; ++scenario) {
        const uint32_t normal_interval_us = 20000U + NextRandom(random) % 180001U;
        auto production = Clock(normal_interval_us);
        auto legacy = production;
        uint64_t now_us = 1000000U + NextRandom(random) % 1000000U;

        if ((NextRandom(random) & 1U) != 0U) {
            CHECK(mjpeg_playback_clock_resume(
                      &production, now_us, normal_interval_us) ==
                  mjpeg_playback_clock_resume(&legacy, now_us, normal_interval_us));
        }
        CheckStateEqual(production, legacy);

        for (int step = 0; step < 256; ++step) {
            now_us += 1U + NextRandom(random) % 250000U;
            switch (NextRandom(random) % 5U) {
                case 0:
                    mjpeg_playback_clock_pause(&production);
                    mjpeg_playback_clock_pause(&legacy);
                    break;
                case 1: {
                    const uint32_t next_interval =
                        20000U + NextRandom(random) % 180001U;
                    CHECK(mjpeg_playback_clock_resume(
                              &production, now_us, next_interval) ==
                          mjpeg_playback_clock_resume(
                              &legacy, now_us, next_interval));
                    break;
                }
                case 2: {
                    const auto actual =
                        mjpeg_playback_clock_poll(&production, now_us);
                    const auto expected =
                        mjpeg_playback_clock_legacy_poll(&legacy, now_us);
                    CheckPollEqual(actual, expected);
                    break;
                }
                default: {
                    const auto result = static_cast<mjpeg_playback_decode_result_t>(
                        NextRandom(random) % 3U);
                    const uint32_t yield_interval = NextRandom(random) % 17U;
                    const auto actual = mjpeg_playback_clock_on_decode_result(
                        &production, result, now_us, yield_interval);
                    const auto expected =
                        mjpeg_playback_clock_legacy_on_decode_result(
                            &legacy, result, now_us, yield_interval);
                    CheckEffectEqual(actual, expected);
                    break;
                }
            }
            CheckStateEqual(production, legacy);
        }
    }
}

void TestPlayerAdapterSynchronizesOwnerState()
{
    uint32_t interval = 100000, normal = 100000, frame = 9, errors = 3;
    uint64_t deadline = 2000000, warmup = 0, soft = 0;
    uint64_t last_frame = 1000000, last_success = 1900000, last_diag = 0;
    const mjpeg_playback_clock_player_binding_t binding = {
        &interval, &normal, &deadline, &warmup, &soft,
        &last_frame, &last_success, &last_diag, &frame, &errors,
    };
    auto poll = mjpeg_playback_clock_player_poll(&binding, 2000000, true);
    CHECK(poll.action == MJPEG_CLOCK_ACTION_DECODE);
    auto effect = mjpeg_playback_clock_player_on_decode_result(
        &binding, MJPEG_CLOCK_DECODE_OK, 2000000, 10, true);
    CHECK(effect.delay_one_tick);
    CHECK(frame == 10 && errors == 0);
    CHECK(deadline == 2100000 && last_success == 2000000);
}

void TestDirectBindingMatchesProductionTrace()
{
    uint32_t random = 0xD1BEC7U;
    for (int scenario = 0; scenario < 64; ++scenario) {
        uint32_t interval_a = 20000U + NextRandom(random) % 180001U;
        uint32_t interval_b = interval_a;
        uint32_t normal_a = interval_a, normal_b = normal_a;
        uint32_t frame_a = NextRandom(random) % 100U, frame_b = frame_a;
        uint32_t errors_a = NextRandom(random) % 11U, errors_b = errors_a;
        uint64_t now = 1000000U + NextRandom(random) % 1000000U;
        uint64_t deadline_a = now + NextRandom(random) % 250000U;
        uint64_t deadline_b = deadline_a;
        uint64_t warmup_a = (NextRandom(random) & 1U) ? now + 10000U : 0U;
        uint64_t warmup_b = warmup_a;
        uint64_t soft_a = (NextRandom(random) & 1U) ? now + 500000U : 0U;
        uint64_t soft_b = soft_a;
        uint64_t last_frame_a = now - 1000U, last_frame_b = last_frame_a;
        uint64_t last_success_a = now - NextRandom(random) % 2000000U;
        uint64_t last_success_b = last_success_a;
        uint64_t last_diag_a = 0U, last_diag_b = 0U;
        const mjpeg_playback_clock_player_binding_t production = {
            &interval_a, &normal_a, &deadline_a, &warmup_a, &soft_a,
            &last_frame_a, &last_success_a, &last_diag_a, &frame_a, &errors_a,
        };
        const mjpeg_playback_clock_player_binding_t direct = {
            &interval_b, &normal_b, &deadline_b, &warmup_b, &soft_b,
            &last_frame_b, &last_success_b, &last_diag_b, &frame_b, &errors_b,
        };
        for (int step = 0; step < 256; ++step) {
            now += 1U + NextRandom(random) % 250000U;
            if ((NextRandom(random) & 1U) == 0U) {
                const auto expected =
                    mjpeg_playback_clock_player_poll(&production, now, true);
                const auto actual =
                    mjpeg_playback_clock_player_poll_direct(&direct, now);
                CheckPollEqual(actual, expected);
            } else {
                const auto result = static_cast<mjpeg_playback_decode_result_t>(
                    NextRandom(random) % 3U);
                const uint32_t yield_interval = NextRandom(random) % 17U;
                const auto expected = mjpeg_playback_clock_player_on_decode_result(
                    &production, result, now, yield_interval, true);
                const auto actual =
                    mjpeg_playback_clock_player_on_decode_result_direct(
                        &direct, result, now, yield_interval);
                CheckEffectEqual(actual, expected);
            }
            CHECK(interval_a == interval_b);
            CHECK(normal_a == normal_b);
            CHECK(deadline_a == deadline_b);
            CHECK(warmup_a == warmup_b);
            CHECK(soft_a == soft_b);
            CHECK(last_frame_a == last_frame_b);
            CHECK(last_success_a == last_success_b);
            CHECK(last_diag_a == last_diag_b);
            CHECK(frame_a == frame_b);
            CHECK(errors_a == errors_b);
        }
    }

    CHECK(mjpeg_playback_clock_player_poll_direct(nullptr, 0).action ==
          MJPEG_CLOCK_ACTION_PAUSED);
    const auto effect = mjpeg_playback_clock_player_on_decode_result_direct(
        nullptr, MJPEG_CLOCK_DECODE_OK, 0, 0);
    CHECK(!effect.stream_ended && !effect.delay_one_tick &&
          !effect.enter_error_state);
}

}  // namespace

int main()
{
    TestResumeWarmupSoftStartAndRestore();
    TestPauseAndResumeReplaceStaleDeadline();
    TestWaitAndLateSuccessReanchorWithoutCatchup();
    TestSuccessResetsErrorsAndRequestsPeriodicYield();
    TestEofMakesNextLoopDueAndKeepsCursorOutside();
    TestEleventhErrorEntersErrorWithoutMovingDeadline();
    TestStallReanchorsAndRateLimitsDiagnostic();
    TestInvalidIntervalFailsClosed();
    TestProductionAndLegacyRemainEquivalent();
    TestProductionAndLegacyTraceEquivalence();
    TestPlayerAdapterSynchronizesOwnerState();
    TestDirectBindingMatchesProductionTrace();
    if (failures != 0) {
        std::fprintf(stderr, "mjpeg_playback_clock_candidate_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("mjpeg_playback_clock_candidate_test: PASS (12 production clock contracts)");
    return 0;
}
