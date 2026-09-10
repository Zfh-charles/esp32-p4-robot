#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mjpeg_decoder_stage.h"

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

struct FakeBackend {
    size_t free_psram = 2 * 1024 * 1024;
    bool input_alloc_ok = true;
    bool output_alloc_ok = true;
    bool open_ok = true;
    bool frame_info_ok = true;
    size_t output_actual_size = 480 * 480 * 2;
    mjpeg_decoder_frame_info_t supplied_info{480, 480};
    std::vector<mjpeg_decoder_backend_result_t> process_results{
        MJPEG_DECODER_BACKEND_OK};
    size_t process_cursor = 0;
    size_t decoded_size = 128;
    uint64_t clock_us = 0;
    size_t now_calls = 0;
    std::vector<std::string> effects;
    uint8_t input_storage[128 * 1024]{};
    uint8_t output_storage[480 * 480 * 2]{};
    int decoder_token = 1;
};

uint64_t NowUs(void* context)
{
    auto* fake = static_cast<FakeBackend*>(context);
    ++fake->now_calls;
    fake->clock_us += 1000;
    return fake->clock_us;
}

size_t GetFreePsram(void* context)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("free_psram");
    return fake->free_psram;
}

uint8_t* AllocInput(void* context, size_t, size_t)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("input_alloc");
    return fake->input_alloc_ok ? fake->input_storage : nullptr;
}

void FreeInput(void* context, uint8_t*)
{
    static_cast<FakeBackend*>(context)->effects.emplace_back("input_free");
}

uint8_t* AllocOutput(void* context, size_t, size_t, size_t* actual_size)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("output_alloc");
    if (!fake->output_alloc_ok) {
        return nullptr;
    }
    *actual_size = fake->output_actual_size;
    return fake->output_storage;
}

void FreeOutput(void* context, uint8_t*)
{
    static_cast<FakeBackend*>(context)->effects.emplace_back("output_free");
}

bool OpenDecoder(void* context, void** handle)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("decoder_open");
    if (!fake->open_ok) {
        return false;
    }
    *handle = &fake->decoder_token;
    return true;
}

mjpeg_decoder_backend_result_t Process(
    void* context,
    void*,
    const uint8_t*,
    size_t,
    uint64_t,
    uint8_t*,
    size_t,
    size_t* decoded_size)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("decoder_process");
    const size_t index = fake->process_cursor < fake->process_results.size()
                             ? fake->process_cursor
                             : fake->process_results.size() - 1;
    ++fake->process_cursor;
    *decoded_size = fake->decoded_size;
    return fake->process_results[index];
}

bool GetFrameInfo(void* context, void*, mjpeg_decoder_frame_info_t* info)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("frame_info");
    if (!fake->frame_info_ok) {
        return false;
    }
    *info = fake->supplied_info;
    return true;
}

size_t GetImageSize(void* context, uint32_t, const mjpeg_decoder_frame_info_t*)
{
    auto* fake = static_cast<FakeBackend*>(context);
    fake->effects.emplace_back("image_size");
    return fake->output_actual_size;
}

const mjpeg_decoder_backend_ops_t kOps = {
    GetFreePsram,
    NowUs,
    AllocInput,
    FreeInput,
    AllocOutput,
    FreeOutput,
    OpenDecoder,
    Process,
    GetFrameInfo,
    GetImageSize,
};

mjpeg_decoder_resources_t EmptyResources()
{
    mjpeg_decoder_resources_t resources{};
    resources.input_alignment = 64;
    resources.output_alignment = 64;
    resources.canvas_width = 480;
    resources.canvas_height = 480;
    resources.output_format = 1;
    return resources;
}

mjpeg_decoder_request_t Request(const std::vector<uint8_t>& frame)
{
    return {frame.data(), frame.size(), 42};
}

mjpeg_decoder_owner_state_t EmptyOwner()
{
    mjpeg_decoder_owner_state_t owner{};
    owner.input_alignment = 64;
    owner.output_alignment = 64;
    owner.canvas_width = 480;
    owner.canvas_height = 480;
    owner.output_format = 1;
    return owner;
}

void TestSuccessOwnsDecodeButNotPresent()
{
    FakeBackend fake;
    auto resources = EmptyResources();
    const std::vector<uint8_t> frame(1024, 0x5a);
    mjpeg_decoder_output_t output{};

    const auto status = mjpeg_decoder_stage_decode(
        &resources, Request(frame), &kOps, &fake, &output);

    CHECK(status == MJPEG_DECODER_OK);
    CHECK(output.data == fake.output_storage);
    CHECK(output.decoded_size == fake.decoded_size);
    CHECK(output.width == 480 && output.height == 480);
    CHECK(output.memcpy_ms == 1);
    CHECK(output.decode_ms == 1);
    CHECK(fake.now_calls == 4);
    CHECK(std::memcmp(fake.input_storage, frame.data(), frame.size()) == 0);
    CHECK((fake.effects == std::vector<std::string>{
                               "free_psram", "input_alloc", "output_alloc",
                               "decoder_open", "decoder_process", "frame_info"}));
}

void TestExistingResourcesAreReused()
{
    FakeBackend fake;
    auto resources = EmptyResources();
    resources.input_buffer = fake.input_storage;
    resources.input_buffer_size = sizeof(fake.input_storage);
    resources.output_buffer = fake.output_storage;
    resources.output_buffer_size = sizeof(fake.output_storage);
    resources.decoder_handle = &fake.decoder_token;
    resources.frame_info = fake.supplied_info;
    const std::vector<uint8_t> frame(256, 0x2c);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_decode(
              &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_OK);
    CHECK((fake.effects == std::vector<std::string>{"decoder_process"}));
}

void TestLowPsramLeavesExistingResourcesUntouched()
{
    FakeBackend fake;
    fake.free_psram = 1;
    auto resources = EmptyResources();
    resources.input_buffer = fake.input_storage;
    resources.input_buffer_size = 16;
    resources.output_buffer = fake.output_storage;
    resources.output_buffer_size = sizeof(fake.output_storage);
    const std::vector<uint8_t> frame(1024, 0x11);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_decode(
              &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_NO_MEM);
    CHECK(resources.input_buffer == fake.input_storage);
    CHECK(resources.output_buffer == fake.output_storage);
    CHECK((fake.effects == std::vector<std::string>{"free_psram"}));
}

void TestInputAllocFailureDropsOutputLikeLegacy()
{
    FakeBackend fake;
    fake.input_alloc_ok = false;
    auto resources = EmptyResources();
    resources.input_buffer = fake.input_storage;
    resources.input_buffer_size = 16;
    resources.output_buffer = fake.output_storage;
    resources.output_buffer_size = sizeof(fake.output_storage);
    const std::vector<uint8_t> frame(1024, 0x22);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_decode(
              &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_NO_MEM);
    CHECK(resources.input_buffer == nullptr);
    CHECK(resources.output_buffer == nullptr);
    CHECK(resources.output_buffer_size == 0);
    CHECK((fake.effects == std::vector<std::string>{
                               "free_psram", "input_free", "input_alloc", "output_free"}));
}

void TestResizeRetriesExactlyOnce()
{
    FakeBackend fake;
    fake.process_results = {
        MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH,
        MJPEG_DECODER_BACKEND_OK,
    };
    auto resources = EmptyResources();
    resources.input_buffer = fake.input_storage;
    resources.input_buffer_size = sizeof(fake.input_storage);
    resources.output_buffer = fake.output_storage;
    resources.output_buffer_size = 64;
    resources.decoder_handle = &fake.decoder_token;
    const std::vector<uint8_t> frame(256, 0x33);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_decode(
              &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_OK);
    CHECK((fake.effects == std::vector<std::string>{
                               "decoder_process", "frame_info", "output_free",
                               "image_size", "output_alloc", "decoder_process"}));
    CHECK(fake.process_cursor == 2);
}

void TestFrameInfoFailurePreservesOldOutput()
{
    FakeBackend fake;
    fake.process_results = {MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH};
    fake.frame_info_ok = false;
    auto resources = EmptyResources();
    resources.input_buffer = fake.input_storage;
    resources.input_buffer_size = sizeof(fake.input_storage);
    resources.output_buffer = fake.output_storage;
    resources.output_buffer_size = 64;
    resources.decoder_handle = &fake.decoder_token;
    const std::vector<uint8_t> frame(256, 0x44);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_decode(
              &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_FAIL);
    CHECK(resources.output_buffer == fake.output_storage);
    CHECK((fake.effects == std::vector<std::string>{"decoder_process", "frame_info"}));
}

void TestResizeAllocFailureLeavesNoOutput()
{
    FakeBackend fake;
    fake.process_results = {MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH};
    fake.output_alloc_ok = false;
    auto resources = EmptyResources();
    resources.input_buffer = fake.input_storage;
    resources.input_buffer_size = sizeof(fake.input_storage);
    resources.output_buffer = fake.output_storage;
    resources.output_buffer_size = 64;
    resources.decoder_handle = &fake.decoder_token;
    const std::vector<uint8_t> frame(256, 0x55);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_decode(
              &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_NO_MEM);
    CHECK(resources.output_buffer == nullptr);
}

void TestSecondShortageIsFailureAndZeroOutputIsSuccess()
{
    {
        FakeBackend fake;
        fake.process_results = {
            MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH,
            MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH,
        };
        auto resources = EmptyResources();
        resources.input_buffer = fake.input_storage;
        resources.input_buffer_size = sizeof(fake.input_storage);
        resources.output_buffer = fake.output_storage;
        resources.output_buffer_size = 64;
        resources.decoder_handle = &fake.decoder_token;
        const std::vector<uint8_t> frame(256, 0x66);
        mjpeg_decoder_output_t output{};
        CHECK(mjpeg_decoder_stage_decode(
                  &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_FAIL);
        CHECK(fake.process_cursor == 2);
    }
    {
        FakeBackend fake;
        fake.decoded_size = 0;
        auto resources = EmptyResources();
        resources.input_buffer = fake.input_storage;
        resources.input_buffer_size = sizeof(fake.input_storage);
        resources.output_buffer = fake.output_storage;
        resources.output_buffer_size = sizeof(fake.output_storage);
        resources.decoder_handle = &fake.decoder_token;
        const std::vector<uint8_t> frame(256, 0x77);
        mjpeg_decoder_output_t output{};
        CHECK(mjpeg_decoder_stage_decode(
                  &resources, Request(frame), &kOps, &fake, &output) == MJPEG_DECODER_OK);
        CHECK(output.decoded_size == 0);
        CHECK((fake.effects == std::vector<std::string>{"decoder_process"}));
    }
}

void TestOwnerSyncOnInputAllocationFailure()
{
    FakeBackend fake;
    fake.input_alloc_ok = false;
    auto owner = EmptyOwner();
    owner.output_buffer = fake.output_storage;
    owner.output_buffer_size = sizeof(fake.output_storage);
    const std::vector<uint8_t> frame(1024, 0x91);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_run(
              &owner, Request(frame), &kOps, &fake, &output) ==
          MJPEG_DECODER_NO_MEM);
    CHECK(owner.input_buffer == nullptr);
    CHECK(owner.output_buffer == nullptr);
    CHECK(owner.output_buffer_size == 0);
}

void TestOwnerSyncOnResizeAllocationFailure()
{
    FakeBackend fake;
    fake.process_results = {MJPEG_DECODER_BACKEND_BUFFER_NOT_ENOUGH};
    fake.output_alloc_ok = false;
    auto owner = EmptyOwner();
    owner.input_buffer = fake.input_storage;
    owner.input_buffer_size = sizeof(fake.input_storage);
    owner.output_buffer = fake.output_storage;
    owner.output_buffer_size = sizeof(fake.output_storage);
    const size_t old_output_size = owner.output_buffer_size;
    const std::vector<uint8_t> frame(1024, 0x92);
    mjpeg_decoder_output_t output{};

    CHECK(mjpeg_decoder_stage_run(
              &owner, Request(frame), &kOps, &fake, &output) ==
          MJPEG_DECODER_NO_MEM);
    CHECK(owner.input_buffer == fake.input_storage);
    CHECK(owner.output_buffer == nullptr);
    CHECK(owner.output_buffer_size == old_output_size);
    CHECK(owner.frame_width == 480);
    CHECK(owner.frame_height == 480);
}

}  // namespace

int main()
{
    TestSuccessOwnsDecodeButNotPresent();
    TestExistingResourcesAreReused();
    TestLowPsramLeavesExistingResourcesUntouched();
    TestInputAllocFailureDropsOutputLikeLegacy();
    TestResizeRetriesExactlyOnce();
    TestFrameInfoFailurePreservesOldOutput();
    TestResizeAllocFailureLeavesNoOutput();
    TestSecondShortageIsFailureAndZeroOutputIsSuccess();
    TestOwnerSyncOnInputAllocationFailure();
    TestOwnerSyncOnResizeAllocationFailure();
    if (failures != 0) {
        std::fprintf(stderr, "mjpeg_decoder_stage_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("mjpeg_decoder_stage_test: PASS (10 production resource/sync contracts)");
    return 0;
}
