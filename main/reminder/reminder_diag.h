#pragma once

#include "audio_service.h"
#include "device_state.h"
#include "reminder/reminder_trace.h"

#include <cstdint>

/** Bitmask returned by ReminderDiagEvaluate — grep serial for "ANOMALY". */
enum ReminderDiagAnomaly : uint32_t {
    kDiagNone              = 0,
    kDiagIdleWakeOff       = 1u << 0,  /* W001 */
    kDiagIdleMicOff        = 1u << 1,  /* W002 */
    kDiagVoiceMicOff       = 1u << 2,  /* W003 */
    kDiagWakeWrongRoute    = 1u << 3,  /* W004 */
    kDiagHoldStuckIdle     = 1u << 4,  /* W005 */
    kDiagProactiveOrphan   = 1u << 5,  /* W006 */
    kDiagListeningDead     = 1u << 6,  /* W007 */
    kDiagCaptureBothOff    = 1u << 7,  /* W008 wake+voice both off in Capture while session active */
    kDiagPlaybackNoOutput  = 1u << 8,  /* W009 speaking/playback but codec out off */
    kDiagPendingNoSession  = 1u << 9,  /* W010 pending ack id but session None */
    kDiagIdleUserStale     = 1u << 10, /* W011 idle but session still User */
};

struct ReminderDiagSnapshot {
    int session_kind = 0;
    DeviceState device_state = kDeviceStateUnknown;
    AudioRoute route = AudioRoute::Capture;
    bool channel_open = false;
    bool wake_running = false;
    bool voice_running = false;
    bool hold = false;
    bool codec_in = false;
    bool codec_out = false;
    bool alarm_ringing = false;
    bool can_deliver = false;
    bool audio_idle = true;
    bool boot_wake_deferred = false;
    uint32_t input_age_ms = 0;
    uint32_t output_age_ms = 0;
    size_t decode_q = 0;
    size_t playback_q = 0;
    size_t send_q = 0;
    uint32_t input_frames = 0;
    uint32_t playback_frames = 0;
    const char* pending_id = "-";
};

/** Record event name for post-mortem ring (call from ReminderTraceLog). */
void ReminderDiagRecordEvent(const char* event);

/** Evaluate invariants; logs ESP_LOGW lines prefixed with "ANOMALY Wxxx". Returns bitmask. */
uint32_t ReminderDiagEvaluate(const ReminderDiagSnapshot& snap, const char* context);

/** Dump last N recorded events (call when anomalies detected). */
void ReminderDiagDumpRecent(const char* reason);

/** One-line snapshot for periodic heartbeat. */
void ReminderDiagLogSnapshot(const char* tag, const ReminderDiagSnapshot& snap);

/** Human-readable heal reason from anomaly mask. */
void ReminderDiagLogAutoHeal(uint32_t anomaly_mask);
