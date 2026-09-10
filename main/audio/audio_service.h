#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "protocol.h"


/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 6
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3
#define PROACTIVE_PLAYBACK_PREBUFFER_FRAMES 4
#define PLAYBACK_PREBUFFER_TIMEOUT_MS 420

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000


#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    /** Fired after local OGG playback restores Capture route (boot chime, alerts). */
    std::function<void(void)> on_capture_restored;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

/** Hardware audio path — only one active at a time on BoxAudioCodec (ES7210/ES8311). */
enum class AudioRoute {
    Capture,   /* mic on,  speaker off — wake word / listening */
    Playback,  /* mic off, speaker on  — TTS / alarm / reminder */
    Duplex,    /* mic + speaker      — realtime AEC conversation */
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

struct AudioServiceDiag {
    size_t decode_queue = 0;
    size_t playback_queue = 0;
    size_t send_queue = 0;
    uint32_t input_age_ms = 0;
    uint32_t output_age_ms = 0;
    uint32_t input_frames = 0;
    uint32_t playback_frames = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsWakeWordInitialized() const { return wake_word_initialized_; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

    /** Single entry for I2S/capture/playback switching. force=true re-inits hardware (multi-round). */
    void SetAudioRoute(AudioRoute route, bool force = false);
    AudioRoute GetAudioRoute() const { return audio_route_; }

    void PrepareSpeakerPlayback();
    void EndSpeakerPlayback();
    void BeginPlaybackPrebuffer(size_t target_frames = PROACTIVE_PLAYBACK_PREBUFFER_FRAMES);
    void ReleasePlaybackPrebuffer();
    void DrainLocalPlayback(int timeout_ms = 2000);
    bool IsSpeakerPlaybackHeld() const { return speaker_playback_hold_; }
    void RestoreCaptureForWakeWord();
    void EnsureIdleCapture();
    /** Block 15s mic power-down during sessions / idle rearm transitions. */
    void SetCapturePowerHold(bool hold);
    bool IsCapturePowerHeld() const { return capture_power_hold_; }
    AudioServiceDiag GetDiagnosticState();

private:
    DebugStatistics debug_statistics_;
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;
    bool speaker_playback_hold_ = false;
    bool capture_power_hold_ = false;
    AudioRoute audio_route_ = AudioRoute::Capture;
    bool restore_capture_after_local_playback_ = false;
    size_t playback_prebuffer_target_ = 0;
    bool playback_prebuffer_filling_ = false;
    bool playback_prebuffer_initial_fill_ = false;
    bool playback_prebuffer_deadline_started_ = false;
    uint32_t playback_rebuffer_count_ = 0;
    std::chrono::steady_clock::time_point playback_prebuffer_deadline_;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
