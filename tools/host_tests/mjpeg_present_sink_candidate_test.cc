#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mjpeg_present_sink.h"

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

struct FakeSink {
    uint64_t clock_us = 0;
    std::vector<bool> active_plan{false, false, false};
    size_t active_cursor = 0;
    std::vector<std::string> effects;
    uint32_t noted_memcpy_ms = 0;
    uint32_t noted_decode_ms = 0;
    uint32_t noted_callback_ms = 0;
    const uint8_t* callback_data = nullptr;
    size_t callback_size = 0;
    uint32_t callback_width = 0;
    uint32_t callback_height = 0;
};

uint64_t NowUs(void* context)
{
    auto* fake = static_cast<FakeSink*>(context);
    fake->effects.emplace_back("now");
    fake->clock_us += 1000;
    return fake->clock_us;
}

bool WindowActive(void* context)
{
    auto* fake = static_cast<FakeSink*>(context);
    fake->effects.emplace_back("active");
    const size_t index = fake->active_cursor < fake->active_plan.size()
                             ? fake->active_cursor
                             : fake->active_plan.size() - 1;
    ++fake->active_cursor;
    return fake->active_plan[index];
}

void Breadcrumb(void* context, const char* name)
{
    static_cast<FakeSink*>(context)->effects.emplace_back(
        std::string("breadcrumb:") + name);
}

void NoteFrame(
    void* context,
    uint32_t memcpy_ms,
    uint32_t decode_ms,
    uint32_t callback_ms)
{
    auto* fake = static_cast<FakeSink*>(context);
    fake->effects.emplace_back("note");
    fake->noted_memcpy_ms = memcpy_ms;
    fake->noted_decode_ms = decode_ms;
    fake->noted_callback_ms = callback_ms;
}

void YieldNow(void* context)
{
    static_cast<FakeSink*>(context)->effects.emplace_back("yield");
}

void FrameCallback(
    void*,
    const uint8_t* data,
    size_t size,
    uint32_t width,
    uint32_t height,
    void* user_data)
{
    auto* fake = static_cast<FakeSink*>(user_data);
    fake->effects.emplace_back("callback");
    fake->callback_data = data;
    fake->callback_size = size;
    fake->callback_width = width;
    fake->callback_height = height;
}

const mjpeg_present_sink_ops_t kOps = {
    NowUs,
    WindowActive,
    Breadcrumb,
    NoteFrame,
    YieldNow,
};

mjpeg_present_sink_frame_t Frame(
    const uint8_t* data,
    size_t size,
    mjpeg_present_frame_callback_t callback,
    FakeSink* fake)
{
    return {
        nullptr,
        data,
        size,
        480,
        480,
        3,
        7,
        callback,
        fake,
    };
}

void TestZeroDecodedSizeOnlyNotesTiming()
{
    FakeSink fake;
    auto frame = Frame(nullptr, 0, FrameCallback, &fake);
    mjpeg_present_sink_result_t result{};
    CHECK(mjpeg_present_sink_present(&frame, &kOps, &fake, &result));
    CHECK(!result.presented);
    CHECK(result.callback_ms == 0);
    CHECK((fake.effects == std::vector<std::string>{"note"}));
    CHECK(fake.noted_memcpy_ms == 3 && fake.noted_decode_ms == 7);
}

void TestMissingCallbackDoesNotPresent()
{
    FakeSink fake;
    const uint8_t data[] = {1, 2, 3};
    auto frame = Frame(data, sizeof(data), nullptr, &fake);
    mjpeg_present_sink_result_t result{};
    CHECK(mjpeg_present_sink_present(&frame, &kOps, &fake, &result));
    CHECK(!result.presented);
    CHECK((fake.effects == std::vector<std::string>{"note"}));
}

void TestInactiveWindowCallsCallbackThenNotes()
{
    FakeSink fake;
    const uint8_t data[] = {4, 5, 6};
    auto frame = Frame(data, sizeof(data), FrameCallback, &fake);
    mjpeg_present_sink_result_t result{};
    CHECK(mjpeg_present_sink_present(&frame, &kOps, &fake, &result));
    CHECK(result.presented);
    CHECK(result.callback_ms == 1);
    CHECK(fake.callback_data == data && fake.callback_size == sizeof(data));
    CHECK(fake.callback_width == 480 && fake.callback_height == 480);
    CHECK((fake.effects == std::vector<std::string>{
                               "now", "active", "callback", "active", "now",
                               "note", "active"}));
}

void TestActiveWindowPreservesBreadcrumbAndYieldOrder()
{
    FakeSink fake;
    fake.active_plan = {true, true, true};
    const uint8_t data[] = {7, 8, 9};
    auto frame = Frame(data, sizeof(data), FrameCallback, &fake);
    mjpeg_present_sink_result_t result{};
    CHECK(mjpeg_present_sink_present(&frame, &kOps, &fake, &result));
    CHECK((fake.effects == std::vector<std::string>{
                               "now", "active", "breadcrumb:pre_frame_cb",
                               "callback", "active", "breadcrumb:post_frame_cb",
                               "now", "note", "active", "breadcrumb:post_frame_note",
                               "yield", "breadcrumb:post_yield"}));
}

void TestWindowStateIsSampledThreeTimes()
{
    FakeSink fake;
    fake.active_plan = {true, false, true};
    const uint8_t data[] = {10};
    auto frame = Frame(data, sizeof(data), FrameCallback, &fake);
    mjpeg_present_sink_result_t result{};
    CHECK(mjpeg_present_sink_present(&frame, &kOps, &fake, &result));
    CHECK(fake.active_cursor == 3);
    CHECK((fake.effects == std::vector<std::string>{
                               "now", "active", "breadcrumb:pre_frame_cb",
                               "callback", "active", "now", "note", "active",
                               "breadcrumb:post_frame_note", "yield",
                               "breadcrumb:post_yield"}));
}

void TestInvalidFrameFailsClosedWithoutEffects()
{
    FakeSink fake;
    const uint8_t data[] = {11};
    auto frame = Frame(nullptr, sizeof(data), FrameCallback, &fake);
    mjpeg_present_sink_result_t result{};
    CHECK(!mjpeg_present_sink_present(&frame, &kOps, &fake, &result));
    CHECK(fake.effects.empty());
    CHECK(!mjpeg_present_sink_present(nullptr, &kOps, &fake, &result));
}

}  // namespace

int main()
{
    TestZeroDecodedSizeOnlyNotesTiming();
    TestMissingCallbackDoesNotPresent();
    TestInactiveWindowCallsCallbackThenNotes();
    TestActiveWindowPreservesBreadcrumbAndYieldOrder();
    TestWindowStateIsSampledThreeTimes();
    TestInvalidFrameFailsClosedWithoutEffects();
    if (failures != 0) {
        std::fprintf(stderr, "mjpeg_present_sink_production_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("mjpeg_present_sink_production_test: PASS (6 callback/WDT contracts)");
    return 0;
}
