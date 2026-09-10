#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define MJPEG_DECODER_DIRECT_HOST_TEST 1
#include "mjpeg_decoder_direct_candidate.h"

namespace {

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Fake {
    size_t free_psram = 2U * 1024U * 1024U;
    bool input_alloc_ok = true;
    bool output_alloc_ok = true;
    bool open_ok = true;
    bool frame_info_ok = true;
    uint32_t output_actual_size = 480U * 480U * 2U;
    esp_video_codec_frame_info_t info{{480, 480}};
    std::vector<esp_vc_err_t> process_results{ESP_VC_ERR_OK};
    size_t process_cursor = 0;
    uint32_t decoded_size = 128;
    uint64_t clock_us = 0;
    std::vector<std::string> effects;
    uint8_t input_storage[128U * 1024U]{};
    uint8_t output_storage[480U * 480U * 2U]{};
    int decoder_token = 1;
};

Fake *g_fake = nullptr;

struct Owner {
    uint8_t *input = nullptr;
    uint32_t input_size = 0;
    uint8_t *output = nullptr;
    uint32_t output_size = 0;
    esp_video_dec_handle_t decoder = nullptr;
    esp_video_dec_cfg_t decoder_config{};
    esp_video_codec_frame_info_t frame_info{};
};

mjpeg_decoder_direct_binding_t Bind(Owner& owner)
{
    return {&owner.input, &owner.input_size, &owner.output, &owner.output_size,
            &owner.decoder, &owner.decoder_config, &owner.frame_info};
}

const mjpeg_decoder_direct_config_t kConfig{64, 64, 480, 480, 1};

esp_err_t Decode(Owner& owner, const std::vector<uint8_t>& frame,
                 mjpeg_decoder_direct_output_t& output)
{
    const auto binding = Bind(owner);
    return mjpeg_decoder_player_decode_direct(
        &binding, &kConfig, frame.data(), frame.size(), 42, &output);
}

void TestSuccessAndReuse()
{
    Fake fake;
    g_fake = &fake;
    Owner owner;
    std::vector<uint8_t> frame(1024, 0x5a);
    mjpeg_decoder_direct_output_t output{};
    CHECK(Decode(owner, frame, output) == ESP_OK);
    CHECK(output.data == fake.output_storage && output.decoded_size == 128);
    CHECK(output.width == 480 && output.height == 480);
    CHECK(output.memcpy_ms == 1 && output.decode_ms == 1);
    CHECK(std::memcmp(fake.input_storage, frame.data(), frame.size()) == 0);
    CHECK((fake.effects == std::vector<std::string>{
        "free_psram", "input_alloc", "output_alloc", "decoder_open",
        "decoder_process", "frame_info"}));

    fake.effects.clear();
    frame.assign(256, 0x2c);
    CHECK(Decode(owner, frame, output) == ESP_OK);
    CHECK((fake.effects == std::vector<std::string>{"decoder_process"}));
}

void TestLowPsramPreservesResources()
{
    Fake fake;
    g_fake = &fake;
    fake.free_psram = 1;
    Owner owner;
    owner.input = fake.input_storage;
    owner.input_size = 16;
    owner.output = fake.output_storage;
    owner.output_size = sizeof(fake.output_storage);
    std::vector<uint8_t> frame(1024, 0x11);
    mjpeg_decoder_direct_output_t output{};
    CHECK(Decode(owner, frame, output) == ESP_ERR_NO_MEM);
    CHECK(owner.input == fake.input_storage && owner.output == fake.output_storage);
    CHECK((fake.effects == std::vector<std::string>{"free_psram"}));
}

void TestInputAllocationFailureDropsOutput()
{
    Fake fake;
    g_fake = &fake;
    fake.input_alloc_ok = false;
    Owner owner;
    owner.input = fake.input_storage;
    owner.input_size = 16;
    owner.output = fake.output_storage;
    owner.output_size = sizeof(fake.output_storage);
    std::vector<uint8_t> frame(1024, 0x22);
    mjpeg_decoder_direct_output_t output{};
    CHECK(Decode(owner, frame, output) == ESP_ERR_NO_MEM);
    CHECK(owner.input == nullptr && owner.output == nullptr && owner.output_size == 0);
    CHECK((fake.effects == std::vector<std::string>{
        "free_psram", "input_free", "input_alloc", "output_free"}));
}

Owner ReusableOwner(Fake& fake)
{
    Owner owner;
    owner.input = fake.input_storage;
    owner.input_size = sizeof(fake.input_storage);
    owner.output = fake.output_storage;
    owner.output_size = 64;
    owner.decoder = &fake.decoder_token;
    return owner;
}

void TestResizeRetryAndFailures()
{
    std::vector<uint8_t> frame(256, 0x33);
    {
        Fake fake;
        g_fake = &fake;
        fake.process_results = {ESP_VC_ERR_BUF_NOT_ENOUGH, ESP_VC_ERR_OK};
        Owner owner = ReusableOwner(fake);
        mjpeg_decoder_direct_output_t output{};
        CHECK(Decode(owner, frame, output) == ESP_OK);
        CHECK((fake.effects == std::vector<std::string>{
            "decoder_process", "frame_info", "output_free", "image_size",
            "output_alloc", "decoder_process"}));
        CHECK(fake.process_cursor == 2);
    }
    {
        Fake fake;
        g_fake = &fake;
        fake.process_results = {ESP_VC_ERR_BUF_NOT_ENOUGH};
        fake.frame_info_ok = false;
        Owner owner = ReusableOwner(fake);
        mjpeg_decoder_direct_output_t output{};
        CHECK(Decode(owner, frame, output) == ESP_FAIL);
        CHECK(owner.output == fake.output_storage);
    }
    {
        Fake fake;
        g_fake = &fake;
        fake.process_results = {ESP_VC_ERR_BUF_NOT_ENOUGH};
        fake.output_alloc_ok = false;
        Owner owner = ReusableOwner(fake);
        const uint32_t old_size = owner.output_size;
        mjpeg_decoder_direct_output_t output{};
        CHECK(Decode(owner, frame, output) == ESP_ERR_NO_MEM);
        CHECK(owner.output == nullptr && owner.output_size == old_size);
    }
    {
        Fake fake;
        g_fake = &fake;
        fake.process_results = {
            ESP_VC_ERR_BUF_NOT_ENOUGH, ESP_VC_ERR_BUF_NOT_ENOUGH};
        Owner owner = ReusableOwner(fake);
        mjpeg_decoder_direct_output_t output{};
        CHECK(Decode(owner, frame, output) == ESP_FAIL);
        CHECK(fake.process_cursor == 2);
    }
}

void TestZeroDecodeAndInvalidInput()
{
    Fake fake;
    g_fake = &fake;
    fake.decoded_size = 0;
    Owner owner = ReusableOwner(fake);
    owner.frame_info = fake.info;
    std::vector<uint8_t> frame(256, 0x77);
    mjpeg_decoder_direct_output_t output{};
    CHECK(Decode(owner, frame, output) == ESP_OK);
    CHECK(output.decoded_size == 0);
    const auto binding = Bind(owner);
    CHECK(mjpeg_decoder_player_decode_direct(
              &binding, &kConfig, nullptr, 0, 0, &output) == ESP_ERR_INVALID_ARG);
}

}  // namespace

extern "C" size_t heap_caps_get_free_size(uint32_t)
{
    g_fake->effects.emplace_back("free_psram");
    return g_fake->free_psram;
}

extern "C" void *heap_caps_aligned_alloc(size_t, size_t, uint32_t)
{
    g_fake->effects.emplace_back("input_alloc");
    return g_fake->input_alloc_ok ? g_fake->input_storage : nullptr;
}

extern "C" void heap_caps_free(void *)
{
    g_fake->effects.emplace_back("input_free");
}

extern "C" int64_t esp_timer_get_time(void)
{
    g_fake->clock_us += 1000;
    return (int64_t)g_fake->clock_us;
}

extern "C" uint8_t *esp_video_codec_align_alloc(uint8_t, uint32_t, uint32_t *actual)
{
    g_fake->effects.emplace_back("output_alloc");
    if (!g_fake->output_alloc_ok) {
        return nullptr;
    }
    *actual = g_fake->output_actual_size;
    return g_fake->output_storage;
}

extern "C" void esp_video_codec_free(void *)
{
    g_fake->effects.emplace_back("output_free");
}

extern "C" uint32_t esp_video_codec_get_image_size(
    esp_video_codec_pixel_fmt_t, const esp_video_codec_resolution_t *)
{
    g_fake->effects.emplace_back("image_size");
    return g_fake->output_actual_size;
}

extern "C" esp_vc_err_t esp_video_dec_open(
    const esp_video_dec_cfg_t *, esp_video_dec_handle_t *handle)
{
    g_fake->effects.emplace_back("decoder_open");
    if (!g_fake->open_ok) {
        return ESP_VC_ERR_FAIL;
    }
    *handle = &g_fake->decoder_token;
    return ESP_VC_ERR_OK;
}

extern "C" esp_vc_err_t esp_video_dec_process(
    esp_video_dec_handle_t, esp_video_dec_in_frame_t *,
    esp_video_dec_out_frame_t *output)
{
    g_fake->effects.emplace_back("decoder_process");
    const size_t index = g_fake->process_cursor < g_fake->process_results.size()
                             ? g_fake->process_cursor
                             : g_fake->process_results.size() - 1;
    ++g_fake->process_cursor;
    output->decoded_size = g_fake->decoded_size;
    return g_fake->process_results[index];
}

extern "C" esp_vc_err_t esp_video_dec_get_frame_info(
    esp_video_dec_handle_t, esp_video_codec_frame_info_t *info)
{
    g_fake->effects.emplace_back("frame_info");
    if (!g_fake->frame_info_ok) {
        return ESP_VC_ERR_FAIL;
    }
    *info = g_fake->info;
    return ESP_VC_ERR_OK;
}

int main()
{
    TestSuccessAndReuse();
    TestLowPsramPreservesResources();
    TestInputAllocationFailureDropsOutput();
    TestResizeRetryAndFailures();
    TestZeroDecodeAndInvalidInput();
    if (failures != 0) {
        std::fprintf(stderr, "mjpeg_decoder_direct_candidate_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("mjpeg_decoder_direct_candidate_test: PASS (10 direct resource/error contracts)");
    return 0;
}
