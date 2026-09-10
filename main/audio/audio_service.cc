#include "audio_service.h"
#include <esp_log.h>
#include <algorithm>
#include <cstring>

#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
#include "face_mouth_layer.h"
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#if CONFIG_USE_REMINDER_POLL
#include "reminder/reminder_trace.h"
#include "reminder/reminder_hw_trace.h"
#include "reminder/boot_trace.h"
#endif

#define TAG "AudioService"

static constexpr uint32_t kAfeAudioInputTaskStackSize = 16 * 1024;


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", kAfeAudioInputTaskStackSize, this, 8, &audio_input_task_handle_, 0);
    ESP_LOGI(TAG, "audio_input task started (stack: %u, core: 0)",
             static_cast<unsigned>(kAfeAudioInputTaskStackSize));

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 13, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    playback_prebuffer_target_ = 0;
    playback_prebuffer_filling_ = false;
    playback_prebuffer_initial_fill_ = false;
    playback_prebuffer_deadline_started_ = false;
    audio_queue_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (speaker_playback_hold_ && audio_route_ == AudioRoute::Playback) {
        return false;
    }
    if (!codec_->input_enabled()) {
        if (speaker_playback_hold_ && audio_route_ == AudioRoute::Playback) {
            return false;
        }
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        ESP_LOGE(TAG, "Should not be here, bits: %lx", bits);
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        while (!service_stopped_) {
            audio_queue_cv_.wait(lock, [this]() {
                return !audio_playback_queue_.empty() || service_stopped_;
            });
            if (service_stopped_ || playback_prebuffer_target_ == 0 ||
                !playback_prebuffer_filling_) {
                break;
            }
            if (audio_playback_queue_.size() >= playback_prebuffer_target_) {
#if CONFIG_USE_REMINDER_POLL
                REMINDER_TRACE_LOG("playback_prebuffer_ready | frames=%u target=%u refill=%u",
                                   static_cast<unsigned>(audio_playback_queue_.size()),
                                   static_cast<unsigned>(playback_prebuffer_target_),
                                   static_cast<unsigned>(playback_rebuffer_count_));
#endif
                playback_prebuffer_filling_ = false;
                playback_prebuffer_initial_fill_ = false;
                playback_prebuffer_deadline_started_ = false;
                break;
            }
            if (!playback_prebuffer_deadline_started_) {
                playback_prebuffer_deadline_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(PLAYBACK_PREBUFFER_TIMEOUT_MS);
                playback_prebuffer_deadline_started_ = true;
                if (!playback_prebuffer_initial_fill_) {
                    ++playback_rebuffer_count_;
#if CONFIG_USE_REMINDER_POLL
                    REMINDER_TRACE_LOG("playback_rebuffer_begin | count=%u queued=%u target=%u",
                                       static_cast<unsigned>(playback_rebuffer_count_),
                                       static_cast<unsigned>(audio_playback_queue_.size()),
                                       static_cast<unsigned>(playback_prebuffer_target_));
#endif
                }
            }
            const bool ready = audio_queue_cv_.wait_until(
                lock, playback_prebuffer_deadline_, [this]() {
                    return service_stopped_ || playback_prebuffer_target_ == 0 ||
                           audio_playback_queue_.size() >= playback_prebuffer_target_;
                });
            if (service_stopped_) {
                break;
            }
            if (playback_prebuffer_target_ == 0) {
                playback_prebuffer_filling_ = false;
                playback_prebuffer_initial_fill_ = false;
                playback_prebuffer_deadline_started_ = false;
                break;
            }
            if (!ready) {
#if CONFIG_USE_REMINDER_POLL
                REMINDER_TRACE_LOG("playback_prebuffer_timeout | frames=%u target=%u refill=%u",
                                   static_cast<unsigned>(audio_playback_queue_.size()),
                                   static_cast<unsigned>(playback_prebuffer_target_),
                                   static_cast<unsigned>(playback_rebuffer_count_));
#endif
                playback_prebuffer_filling_ = false;
                playback_prebuffer_initial_fill_ = false;
                playback_prebuffer_deadline_started_ = false;
                break;
            }
        }
        if (service_stopped_) {
            break;
        }
        if (audio_playback_queue_.empty()) {
            continue;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            if (speaker_playback_hold_) {
                if (audio_route_ == AudioRoute::Duplex) {
                    SetAudioRoute(AudioRoute::Duplex, true);
                } else {
                    SetAudioRoute(AudioRoute::Playback, true);
                }
            } else if (IsAudioProcessorRunning()) {
                /* User realtime AEC: both mic and speaker without RX-off hard switch */
                SetAudioRoute(AudioRoute::Duplex, false);
            } else {
                codec_->EnableOutput(true);
            }
        }
        codec_->OutputData(task->pcm);
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
        // s1cr-h: publish mouth_level only — no LVGL from audio task.
        FaceMouth_PublishFromPcm(task->pcm.data(), task->pcm.size());
#endif
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceSpeakerPcm((int)task->pcm.size(), "output_task");
#endif

        {
            std::lock_guard<std::mutex> qlock(audio_queue_mutex_);
            if (playback_prebuffer_target_ > 0 && !playback_prebuffer_filling_ &&
                audio_playback_queue_.empty() && audio_decode_queue_.empty()) {
                playback_prebuffer_filling_ = true;
                playback_prebuffer_initial_fill_ = false;
                playback_prebuffer_deadline_started_ = false;
            }
        }

        if (restore_capture_after_local_playback_) {
            bool queues_empty = false;
            {
                std::lock_guard<std::mutex> qlock(audio_queue_mutex_);
                queues_empty = audio_playback_queue_.empty() && audio_decode_queue_.empty();
            }
            if (queues_empty) {
                restore_capture_after_local_playback_ = false;
                SetAudioRoute(AudioRoute::Capture);
                if (callbacks_.on_capture_restored) {
                    callbacks_.on_capture_restored();
                }
            }
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_->Decode(std::move(packet->payload), task->pcm)) {
                // Resample if the sample rate is different
                if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                    int target_size = output_resampler_.GetOutputSamples(task->pcm.size());
                    std::vector<int16_t> resampled(target_size);
                    output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                    task->pcm = std::move(resampled);
                }
#if CONFIG_USE_REMINDER_POLL
                ReminderHwTraceTtsPipeline("decoded", 1, (int)task->pcm.size());
#endif

                lock.lock();
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            } else {
                ESP_LOGE(TAG, "Failed to decode audio");
#if CONFIG_USE_REMINDER_POLL
                ReminderHwTraceTtsPipeline("decoded", 0, 0);
#endif
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            if (!opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
                ESP_LOGE(TAG, "Failed to encode audio");
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    audio_send_queue_.push_back(std::move(packet));
                }
                if (callbacks_.on_send_queue_available) {
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
#if CONFIG_USE_REMINDER_POLL
    ReminderHwTraceTtsPipeline("enqueue", 1, (int)audio_decode_queue_.size());
#endif
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
#if CONFIG_USE_REMINDER_POLL
    REMINDER_TRACE_LOG("wake_word_%s | route=%s running=%d",
                       enable ? "enable" : "disable",
                       ReminderTraceAudioRoute(audio_route_),
                       IsWakeWordRunning() ? 1 : 0);
#endif
    if (enable) {
        if (!wake_word_initialized_) {
            BootTraceMarkHeap("WAKE_INIT_BEGIN");
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                BootTraceMark("WAKE_INIT", "fail");
                return;
            }
            wake_word_initialized_ = true;
            BootTraceMarkHeap("WAKE_INIT_OK");
        }
        // s1bt: stop Feed first, then AFE reset on fetch owner — never reset while Feed in flight.
        if (IsWakeWordRunning()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            vTaskDelay(pdMS_TO_TICKS(20));
            wake_word_->Stop();
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        audio_input_need_warmup_ = true;
        last_input_time_ = std::chrono::steady_clock::now();
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceWake(1, 1, ReminderTraceAudioRoute(audio_route_));
#endif
    } else {
        // s1bt: clear input Feed gate before Stop/reset (was reverse — Feed∩reset_buffer race).
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        vTaskDelay(pdMS_TO_TICKS(20));
        wake_word_->Stop();
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceWake(0, 0, ReminderTraceAudioRoute(audio_route_));
#endif
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        if (codec_ != nullptr && !codec_->input_enabled()) {
            if (!speaker_playback_hold_ && codec_->output_enabled()) {
                /* User soft path: TTS may have left TX on — release before reopening RX */
                codec_->EnableOutput(false);
            }
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableInput(true);
        }
        last_input_time_ = std::chrono::steady_clock::now();
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceVoice(1);
#endif
    } else {
        // s1bt: stop Feed before processor Stop/reset (same ownership as wake path).
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        vTaskDelay(pdMS_TO_TICKS(20));
        audio_processor_->Stop();
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceVoice(0);
#endif
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::SetAudioRoute(AudioRoute route, bool force) {
    if (codec_ == nullptr) {
        return;
    }
    if (!force && route == audio_route_) {
        if (route == AudioRoute::Playback) {
            speaker_playback_hold_ = true;
            last_output_time_ = std::chrono::steady_clock::now();
        } else if (route == AudioRoute::Capture) {
            speaker_playback_hold_ = false;
        }
#if CONFIG_USE_REMINDER_POLL
        REMINDER_TRACE_LOG("route_noop | route=%s hold=%d in=%d out=%d",
                           ReminderTraceAudioRoute(route),
                           speaker_playback_hold_ ? 1 : 0,
                           codec_->input_enabled() ? 1 : 0,
                           codec_->output_enabled() ? 1 : 0);
#endif
        return;
    }

    if (route == AudioRoute::Playback) {
        if (IsWakeWordRunning()) {
            EnableWakeWordDetection(false);
        }
        if (IsAudioProcessorRunning()) {
            EnableVoiceProcessing(false);
        }
        vTaskDelay(pdMS_TO_TICKS(80));
        esp_timer_stop(audio_power_timer_);
        codec_->EnterPlaybackMode();
        speaker_playback_hold_ = true;
        restore_capture_after_local_playback_ = false;
        last_output_time_ = std::chrono::steady_clock::now();
        audio_route_ = AudioRoute::Playback;
        ESP_LOGI(TAG, "AudioRoute -> Playback (in=%d out=%d force=%d)",
                 codec_->input_enabled(), codec_->output_enabled(), force);
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceRoute("Playback", codec_->input_enabled() ? 1 : 0,
                             codec_->output_enabled() ? 1 : 0, "SetAudioRoute", force ? 1 : 0);
        ReminderHwTraceSpeakerOp("route_playback", "Playback", 1);
#endif
        return;
    }

    if (route == AudioRoute::Capture) {
        speaker_playback_hold_ = false;
        restore_capture_after_local_playback_ = false;
        codec_->EnterCaptureMode();
        last_input_time_ = std::chrono::steady_clock::now();
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        audio_route_ = AudioRoute::Capture;
        ESP_LOGI(TAG, "AudioRoute -> Capture (in=%d out=%d force=%d)",
                 codec_->input_enabled(), codec_->output_enabled(), force);
#if CONFIG_USE_REMINDER_POLL
        ReminderHwTraceRoute("Capture", codec_->input_enabled() ? 1 : 0,
                             codec_->output_enabled() ? 1 : 0, "SetAudioRoute", force ? 1 : 0);
        ReminderHwTraceSpeakerOp("route_capture", "Capture", 0);
#endif
        return;
    }

    /* Duplex — conversation realtime AEC */
    if (IsWakeWordRunning()) {
        EnableWakeWordDetection(false);
    }
    codec_->EnterDuplexMode();
    speaker_playback_hold_ = false;
    restore_capture_after_local_playback_ = false;
    esp_timer_stop(audio_power_timer_);
    esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    last_input_time_ = std::chrono::steady_clock::now();
    last_output_time_ = std::chrono::steady_clock::now();
    audio_route_ = AudioRoute::Duplex;
    ESP_LOGI(TAG, "AudioRoute -> Duplex (in=%d out=%d force=%d)",
             codec_->input_enabled(), codec_->output_enabled(), force);
#if CONFIG_USE_REMINDER_POLL
    ReminderHwTraceRoute("Duplex", codec_->input_enabled() ? 1 : 0,
                         codec_->output_enabled() ? 1 : 0, "SetAudioRoute", force ? 1 : 0);
#endif
}

void AudioService::PlaySound(const std::string_view& ogg) {
#if CONFIG_USE_REMINDER_POLL
    ReminderHwTraceSpeakerOp("play_sound", ReminderTraceAudioRoute(audio_route_), speaker_playback_hold_ ? 1 : 0);
    ReminderUiTraceTts("local_sound", "ogg", 1);
#endif
    if (audio_route_ == AudioRoute::Capture) {
        restore_capture_after_local_playback_ = true;
        SetAudioRoute(AudioRoute::Playback);
    } else if (audio_route_ == AudioRoute::Duplex) {
        restore_capture_after_local_playback_ = false;
    }

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    auto find_page = [&](size_t start)->size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') return i;
        }
        return static_cast<size_t>(-1);
    };

    bool seen_head = false;
    bool seen_tags = false;
    int sample_rate = 16000; // 默认值

    while (true) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) break;
        offset = pos;
        if (offset + 27 > size) break;

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) break;

        // Parse packets using lacing
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                // 解析OpusHead包
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                    
                    // OpusHead结构：[0-7] "OpusHead", [8] version, [9] channel_count, [10-11] pre_skip
                    // [12-15] input_sample_rate, [16-17] output_gain, [18] mapping_family
                    if (pkt_len >= 12) {
                        uint8_t version = pkt_ptr[8];
                        uint8_t channel_count = pkt_ptr[9];
                        
                        if (pkt_len >= 16) {
                            // 读取输入采样率 (little-endian)
                            sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | 
                                        (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            ESP_LOGI(TAG, "OpusHead: version=%d, channels=%d, sample_rate=%d", 
                                   version, channel_count, sample_rate);
                        }
                    }
                }
                continue;
            }
            if (!seen_tags) {
                // Expect OpusTags in second packet
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // Audio packet (Opus)
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = 60;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), pkt_ptr, pkt_len);
            PushPacketToDecodeQueue(std::move(packet), true);
        }

        offset = body_off + body_size;
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    playback_prebuffer_target_ = 0;
    playback_prebuffer_filling_ = false;
    playback_prebuffer_initial_fill_ = false;
    playback_prebuffer_deadline_started_ = false;
    audio_queue_cv_.notify_all();
}

void AudioService::PrepareSpeakerPlayback() {
#if CONFIG_USE_REMINDER_POLL
    ReminderHwTraceSpeakerOp("prepare", ReminderTraceAudioRoute(audio_route_), speaker_playback_hold_ ? 1 : 0);
#endif
    if (codec_ != nullptr && audio_route_ == AudioRoute::Playback &&
        speaker_playback_hold_ && codec_->output_enabled()) {
        // One proactive round may prepare at delivery, tts:start and first UDP.
        // Preserve the ready route instead of rebuilding I2S each time.
        last_output_time_ = std::chrono::steady_clock::now();
#if CONFIG_USE_REMINDER_POLL
        REMINDER_TRACE_LOG("speaker_prepare_noop | route=Playback hold=1 out=1");
#endif
        return;
    }
    /* A new reminder/TTS round still force-initializes TX once. */
    SetAudioRoute(AudioRoute::Playback, true);
}

void AudioService::BeginPlaybackPrebuffer(size_t target_frames) {
    if (target_frames == 0) {
        ReleasePlaybackPrebuffer();
        return;
    }
    target_frames = std::min(target_frames, static_cast<size_t>(MAX_PLAYBACK_TASKS_IN_QUEUE));
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    playback_prebuffer_target_ = target_frames;
    playback_prebuffer_filling_ = true;
    playback_prebuffer_initial_fill_ = true;
    playback_prebuffer_deadline_started_ = false;
    playback_rebuffer_count_ = 0;
    audio_queue_cv_.notify_all();
#if CONFIG_USE_REMINDER_POLL
    REMINDER_TRACE_LOG("playback_prebuffer_arm | target=%u max=%u timeout=%ums",
                       static_cast<unsigned>(playback_prebuffer_target_),
                       static_cast<unsigned>(MAX_PLAYBACK_TASKS_IN_QUEUE),
                       static_cast<unsigned>(PLAYBACK_PREBUFFER_TIMEOUT_MS));
#endif
}

void AudioService::ReleasePlaybackPrebuffer() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
#if CONFIG_USE_REMINDER_POLL
    if (playback_prebuffer_target_ > 0) {
        REMINDER_TRACE_LOG("playback_prebuffer_release | queued=%u decode=%u refills=%u",
                           static_cast<unsigned>(audio_playback_queue_.size()),
                           static_cast<unsigned>(audio_decode_queue_.size()),
                           static_cast<unsigned>(playback_rebuffer_count_));
    }
#endif
    playback_prebuffer_target_ = 0;
    playback_prebuffer_filling_ = false;
    playback_prebuffer_initial_fill_ = false;
    playback_prebuffer_deadline_started_ = false;
    audio_queue_cv_.notify_all();
}

void AudioService::RestoreCaptureForWakeWord() {
    SetAudioRoute(AudioRoute::Capture, true);
}

void AudioService::EnsureIdleCapture() {
    if (speaker_playback_hold_ && audio_route_ == AudioRoute::Playback) {
        EndSpeakerPlayback();
        return;
    }
    SetAudioRoute(AudioRoute::Capture, true);
}

void AudioService::SetCapturePowerHold(bool hold) {
    capture_power_hold_ = hold;
    if (hold) {
        last_input_time_ = std::chrono::steady_clock::now();
    }
}

void AudioService::EndSpeakerPlayback() {
    ReleasePlaybackPrebuffer();
#if CONFIG_USE_REMINDER_POLL
    ReminderHwTraceSpeakerOp("end", ReminderTraceAudioRoute(audio_route_), speaker_playback_hold_ ? 1 : 0);
#endif
    if (audio_route_ == AudioRoute::Playback || speaker_playback_hold_) {
        SetAudioRoute(AudioRoute::Capture);
    } else {
        speaker_playback_hold_ = false;
        if (!service_stopped_) {
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        }
    }
}

void AudioService::DrainLocalPlayback(int timeout_ms) {
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        if (IsIdle()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (restore_capture_after_local_playback_ || speaker_playback_hold_ ||
        audio_route_ == AudioRoute::Playback) {
        restore_capture_after_local_playback_ = false;
        EndSpeakerPlayback();
        if (callbacks_.on_capture_restored) {
            callbacks_.on_capture_restored();
        }
    }
}

void AudioService::CheckAndUpdateAudioPowerState() {
    if (speaker_playback_hold_ && audio_route_ == AudioRoute::Playback) {
        if (restore_capture_after_local_playback_) {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            if (audio_decode_queue_.empty() && audio_playback_queue_.empty()) {
                restore_capture_after_local_playback_ = false;
                SetAudioRoute(AudioRoute::Capture, true);
                if (callbacks_.on_capture_restored) {
                    callbacks_.on_capture_restored();
                }
                return;
            }
        }
        return;
    }
    if (capture_power_hold_) {
        last_input_time_ = std::chrono::steady_clock::now();
        return;
    }
    if (audio_route_ == AudioRoute::Capture &&
        (IsWakeWordRunning() || IsAudioProcessorRunning())) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        const bool capture_active = audio_route_ == AudioRoute::Capture &&
            (IsWakeWordRunning() || IsAudioProcessorRunning());
        if (!capture_active) {
#if CONFIG_USE_REMINDER_POLL
            ReminderHwTracePowerMicOff((long)input_elapsed,
                                       IsWakeWordRunning() ? 1 : 0,
                                       IsAudioProcessorRunning() ? 1 : 0,
                                       ReminderTraceAudioRoute(audio_route_));
#endif
            codec_->EnableInput(false);
        }
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (!audio_decode_queue_.empty() || !audio_playback_queue_.empty()) {
            last_output_time_ = now;
        } else if (audio_route_ == AudioRoute::Playback) {
            SetAudioRoute(AudioRoute::Capture, false);
        } else if (audio_route_ == AudioRoute::Duplex) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}

AudioServiceDiag AudioService::GetDiagnosticState() {
    AudioServiceDiag diag;
    auto now = std::chrono::steady_clock::now();
    diag.input_age_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count());
    diag.output_age_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count());
    diag.input_frames = debug_statistics_.input_count;
    diag.playback_frames = debug_statistics_.playback_count;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    diag.decode_queue = audio_decode_queue_.size();
    diag.playback_queue = audio_playback_queue_.size();
    diag.send_queue = audio_send_queue_.size();
    return diag;
}
