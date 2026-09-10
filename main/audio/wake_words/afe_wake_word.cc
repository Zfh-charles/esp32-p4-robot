#include "afe_wake_word.h"
#include "audio_service.h"
#include "wdt_contention_diag.h"
#include "idle_wdt_diag.h"
#include "afe_fetch_gate.h"
#include "stack_diag.h"
#if CONFIG_USE_REMINDER_POLL
#include "reminder/boot_trace.h"
#endif

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <sstream>

#define DETECTION_RUNNING_EVENT 1

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    // Keep AFE work on core0 to avoid competing with MJPEG decode on core1.
    afe_config->afe_perferred_core = 0;
    afe_config->afe_perferred_priority = 1;
    // WakeNet inference runs in the ESP-DL quantized conv1d assembly kernels
    // (libdl_lib.a dl_esp32p4_pointwise_conv1d_qacc_*). With MORE_PSRAM those
    // kernels do their SIMD loads straight out of PSRAM, which faults once the
    // emotion cache and MJPEG decode saturate the same bus — the crash lands
    // inside the kernel with MTVAL pointing just below the feature pointer.
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    BootTraceMarkHeap("AFE_CREATE_BEGIN");
    afe_data_ = afe_iface_->create_from_config(afe_config);
    BootTraceMarkHeap("AFE_CREATE_OK");
    if (afe_data_ == nullptr) {
        ESP_LOGE(TAG, "AFE_MEM alloc_mode=balance create FAILED — internal RAM exhausted");
        return false;
    }
    ESP_LOGW(TAG, "AFE_MEM alloc_mode=balance create ok s1ba");

    // s1bh: fetch() runs the FFT and the WakeNet conv1d kernels on the *caller's*
    // stack, so 4096 overflowed here — an interrupt taken during inference faulted
    // while pushing its frame (_interrupt_handler at vectors.S, MTVAL just past SP).
    // The hwm sampled after fetch() returns looks healthy and hides the real peak.
    xTaskCreate([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 12288, this, 3, nullptr);

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    // s1br: callers live on other tasks (main event loop). Resetting AFE ring state
    // while audio_detection is mid-fetch corrupts the WakeNet feature pointers — the
    // fault then lands inside dl_*_conv1d reading PSRAM (cache_on=1, mtval != sp).
    if (afe_data_ != nullptr) {
        afe_reset_pending_.store(true, std::memory_order_release);
        ESP_LOGW(TAG, "AFE_RESET defer core=%d s1br", xPortGetCoreID());
    }
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    // s1bu: lock across check+feed so reset_buffer cannot land mid-feed (s1bt TOCTOU).
    std::lock_guard<std::mutex> lock(afe_ops_mutex_);
    if ((xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);
    BootTraceMark("AFE_TASK", "started");

    bool first_fetch = true;
    uint64_t last_diag_us = esp_timer_get_time();
    while (true) {
        // Drain deferred reset before sleep — Stop may have raced the previous fetch.
        if (afe_reset_pending_.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(afe_ops_mutex_);
            afe_iface_->reset_buffer(afe_data_);
            ESP_LOGW(TAG, "AFE_RESET apply core=%d s1bu", xPortGetCoreID());
        }
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);
        if (afe_reset_pending_.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(afe_ops_mutex_);
            afe_iface_->reset_buffer(afe_data_);
            ESP_LOGW(TAG, "AFE_RESET apply core=%d s1bu", xPortGetCoreID());
        }

        uint64_t fetch_begin = esp_timer_get_time();
        // s1cl: advertise occupancy so idle breathe can defer canvas write/read
        // (MSPI-751 write-then-read vs WakeNet PSRAM). Leave always, even on abort.
        AfeFetchGateEnter();
        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        AfeFetchGateLeave();
        uint64_t fetch_cost_ms = (esp_timer_get_time() - fetch_begin) / 1000ULL;
        WdtContendNoteAfeFetch((uint32_t)fetch_cost_ms);
        IdleWdtNoteAfeFetch((uint32_t)fetch_cost_ms);
        {
            UBaseType_t hwm_now = uxTaskGetStackHighWaterMark(NULL);
            StackDiagNoteAfeHwm((uint32_t)hwm_now);
            // During emotion preload, densify HWM so we can see stack shrink before overflow.
            if (StackDiagPreloadActive()) {
                static uint64_t s_last_stack_hb_us = 0;
                uint64_t hb_now = esp_timer_get_time();
                if (hb_now - s_last_stack_hb_us >= 200000ULL) {
                    s_last_stack_hb_us = hb_now;
                    ESP_LOGW(TAG,
                             "STACK_DIAG stage=afe_during_preload core=%d fetch_ms=%u hwm=%u",
                             xPortGetCoreID(),
                             (unsigned)fetch_cost_ms,
                             (unsigned)hwm_now);
                    if (hwm_now < 256) {
                        ESP_LOGE(TAG,
                                 "STACK_DIAG WARN afe hwm=%u words during preload — overflow risk",
                                 (unsigned)hwm_now);
                    }
                }
            }
        }
        if (WdtContendWindowActive()) {
            static uint64_t s_last_afe_hb_us = 0;
            uint64_t hb_now = esp_timer_get_time();
            if (hb_now - s_last_afe_hb_us >= 250000ULL) {
                s_last_afe_hb_us = hb_now;
                ESP_LOGI(TAG,
                         "CONTEND_AFE_HB core=%d fetch_ms=%u hwm=%u",
                         xPortGetCoreID(),
                         (unsigned)fetch_cost_ms,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        uint64_t now_us = esp_timer_get_time();
        if (now_us - last_diag_us >= 5000000ULL) {
            last_diag_us = now_us;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            // Keep format to %u only — avoid %llu argument shift on RV32.
            const unsigned free_int =
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            const unsigned min_int =
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            const unsigned largest_int =
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            const unsigned free_psram =
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            const unsigned min_psram =
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG,
                     "AFE_DIAG core=%d fetch_ms=%u hwm=%u wake_pcm_q=%u | free_int=%u min_int=%u largest_int=%u | free_psram=%u min_psram=%u",
                     xPortGetCoreID(),
                     (unsigned)fetch_cost_ms,
                     (unsigned)hwm,
                     (unsigned)wake_word_pcm_.size(),
                     free_int,
                     min_int,
                     largest_int,
                     free_psram,
                     min_psram);
        }
        if (first_fetch) {
            BootTraceMarkHeap("AFE_FIRST_FETCH");
            first_fetch = false;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;;
        }
        // s1br: detection may have been disabled while we were blocked in fetch().
        if ((xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0) {
            continue;
        }

        // Store the wake word data for voice recognition, like who is speaking
        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            // s1bt: only pause fetch here — do NOT reset_buffer yet. AudioService will
            // gate Feed (clear AS_EVENT) then call Stop() which defers reset to this task.
            xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
            last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
            encoder->SetComplexity(0); // 0 is the fastest

            int packets = 0;
            for (auto& pcm: this_->wake_word_pcm_) {
                encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(std::move(opus));
                    this_->wake_word_cv_.notify_all();
                });
                packets++;
            }
            this_->wake_word_pcm_.clear();

            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
