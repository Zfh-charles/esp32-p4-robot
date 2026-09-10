#include "reminder/reminder_diag.h"

#include <deque>
#include <cstring>
#include <esp_timer.h>

#if CONFIG_USE_REMINDER_POLL

namespace {

constexpr size_t kRecentEventCap = 16;

struct DiagEventEntry {
    char name[28];
    int64_t time_us;
};

std::deque<DiagEventEntry> g_recent_events;

void LogAnomaly(const char* code, const char* msg, const ReminderDiagSnapshot& s) {
    ESP_LOGW(REMINDER_TRACE_TAG,
             "ANOMALY %s %s | session=%s state=%s route=%s ch=%d wake=%d voice=%d hold=%d "
             "in=%d out=%d in_age=%ums dq=%u pq=%u pending=%s",
             code, msg,
             ReminderTraceSessionKindName(s.session_kind),
             ReminderTraceDeviceStateName(s.device_state),
             ReminderTraceAudioRoute(s.route),
             s.channel_open ? 1 : 0,
             s.wake_running ? 1 : 0,
             s.voice_running ? 1 : 0,
             s.hold ? 1 : 0,
             s.codec_in ? 1 : 0,
             s.codec_out ? 1 : 0,
             (unsigned)s.input_age_ms,
             (unsigned)s.decode_q, (unsigned)s.playback_q,
             s.pending_id ? s.pending_id : "-");
}

}  // namespace

void ReminderDiagRecordEvent(const char* event) {
    if (event == nullptr || event[0] == '\0') {
        return;
    }
    DiagEventEntry entry{};
    std::strncpy(entry.name, event, sizeof(entry.name) - 1);
    entry.time_us = esp_timer_get_time();
    g_recent_events.push_back(entry);
    while (g_recent_events.size() > kRecentEventCap) {
        g_recent_events.pop_front();
    }
}

void ReminderDiagDumpRecent(const char* reason) {
    if (g_recent_events.empty()) {
        ESP_LOGW(REMINDER_TRACE_TAG, "RECENT (%s) | empty", reason ? reason : "?");
        return;
    }
    std::string line;
    line.reserve(256);
    line += "RECENT (";
    line += reason ? reason : "?";
    line += ") |";
    for (const auto& e : g_recent_events) {
        line += " ";
        line += e.name;
    }
    ESP_LOGW(REMINDER_TRACE_TAG, "%s", line.c_str());
}

uint32_t ReminderDiagEvaluate(const ReminderDiagSnapshot& s, const char* context) {
    uint32_t mask = kDiagNone;
    const bool idle_ready = s.device_state == kDeviceStateIdle && s.session_kind == 0;
    const bool proactive = s.session_kind == 2;
    const bool capture_session = s.voice_running || s.wake_running;

    if (idle_ready && !s.wake_running && !s.boot_wake_deferred) {
        mask |= kDiagIdleWakeOff;
        LogAnomaly("W001", "idle_wake_off", s);
    }
    if (idle_ready && !s.codec_in) {
        mask |= kDiagIdleMicOff;
        LogAnomaly("W002", "idle_mic_off", s);
    }
    if (s.device_state == kDeviceStateIdle && s.session_kind == 1) {
        mask |= kDiagIdleUserStale;
        LogAnomaly("W011", "idle_user_session_stale", s);
    }
    if (s.voice_running && !s.codec_in) {
        mask |= kDiagVoiceMicOff;
        LogAnomaly("W003", "voice_mic_off", s);
    }
    if (s.wake_running && s.route != AudioRoute::Capture && s.route != AudioRoute::Duplex) {
        mask |= kDiagWakeWrongRoute;
        LogAnomaly("W004", "wake_wrong_route", s);
    }
    if (idle_ready && s.hold) {
        mask |= kDiagHoldStuckIdle;
        LogAnomaly("W005", "hold_stuck_idle", s);
    }
    if (proactive && s.pending_id != nullptr && s.pending_id[0] == '-' &&
        s.device_state == kDeviceStateIdle) {
        mask |= kDiagProactiveOrphan;
        LogAnomaly("W006", "proactive_orphan", s);
    }
    if (s.device_state == kDeviceStateListening && s.session_kind == 0 &&
        !s.voice_running && !s.wake_running) {
        mask |= kDiagListeningDead;
        LogAnomaly("W007", "listening_dead", s);
    }
    if (s.route == AudioRoute::Capture && capture_session && !s.codec_in) {
        mask |= kDiagCaptureBothOff;
        LogAnomaly("W008", "capture_mic_off", s);
    }
    if ((s.device_state == kDeviceStateSpeaking || s.route == AudioRoute::Playback) &&
        !s.codec_out && (s.playback_q > 0 || s.decode_q > 0 || !s.audio_idle)) {
        mask |= kDiagPlaybackNoOutput;
        LogAnomaly("W009", "playback_no_output", s);
    }
    if (s.session_kind == 0 && s.pending_id != nullptr && s.pending_id[0] != '-') {
        mask |= kDiagPendingNoSession;
        LogAnomaly("W010", "pending_without_session", s);
    }

    if (mask != kDiagNone && context != nullptr) {
        ESP_LOGW(REMINDER_TRACE_TAG, "ANOMALY context=%s mask=0x%04lx", context,
                 (unsigned long)mask);
        ReminderDiagDumpRecent(context);
    }
    return mask;
}

void ReminderDiagLogSnapshot(const char* tag, const ReminderDiagSnapshot& snap) {
    ESP_LOGI(REMINDER_TRACE_TAG,
             "%s | session=%s state=%s route=%s ch=%d wake=%d voice=%d hold=%d "
             "in=%d out=%d in_age=%ums out_age=%ums dq=%u pq=%u sq=%u aud_idle=%d "
             "deliver=%d alarm=%d pending=%s ifr=%u pfr=%u",
             tag,
             ReminderTraceSessionKindName(snap.session_kind),
             ReminderTraceDeviceStateName(snap.device_state),
             ReminderTraceAudioRoute(snap.route),
             snap.channel_open ? 1 : 0,
             snap.wake_running ? 1 : 0,
             snap.voice_running ? 1 : 0,
             snap.hold ? 1 : 0,
             snap.codec_in ? 1 : 0,
             snap.codec_out ? 1 : 0,
             (unsigned)snap.input_age_ms,
             (unsigned)snap.output_age_ms,
             (unsigned)snap.decode_q,
             (unsigned)snap.playback_q,
             (unsigned)snap.send_q,
             snap.audio_idle ? 1 : 0,
             snap.can_deliver ? 1 : 0,
             snap.alarm_ringing ? 1 : 0,
             snap.pending_id ? snap.pending_id : "-",
             (unsigned)snap.input_frames,
             (unsigned)snap.playback_frames);
}

void ReminderDiagLogAutoHeal(uint32_t anomaly_mask) {
    ESP_LOGW(REMINDER_TRACE_TAG, "auto_heal | mask=0x%04lx action=RestoreIdleReady",
             (unsigned long)anomaly_mask);
}

#else

void ReminderDiagRecordEvent(const char*) {}
uint32_t ReminderDiagEvaluate(const ReminderDiagSnapshot&, const char*) { return 0; }
void ReminderDiagDumpRecent(const char*) {}
void ReminderDiagLogSnapshot(const char*, const ReminderDiagSnapshot&) {}
void ReminderDiagLogAutoHeal(uint32_t) {}

#endif
