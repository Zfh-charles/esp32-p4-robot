#pragma once

/**
 * Hardware + UI state trace — logs at the actual driver/display boundary.
 * Serial filter: ReminderTrace
 *
 * HW lines:  ReminderTrace: HW | mic=0|1 spk=0|1 route=Capture|Playback|Duplex ...
 * UI lines:  ReminderTrace: UI | kind=text|emotion|status|tts ...
 *
 * All state changes go through here; no guessing from scattered logs.
 */

#if CONFIG_USE_REMINDER_POLL

void ReminderHwTraceMic(int on, const char* reason);
void ReminderHwTraceSpk(int on, const char* reason);
void ReminderHwTraceRoute(const char* route, int mic, int spk, const char* reason, int force);
void ReminderHwTraceWake(int enable, int running, const char* route);
void ReminderHwTraceVoice(int running);
void ReminderHwTraceSpeakerOp(const char* op, const char* route, int hold);
void ReminderHwTracePowerMicOff(long in_age_ms, int wake, int voice, const char* route);

void ReminderUiTraceText(const char* role, const char* preview, int applied);
void ReminderUiTraceEmotion(const char* name, int applied);
void ReminderUiTraceStatus(const char* status);
void ReminderUiTraceTts(const char* event, const char* detail, int ok);

/** Final execution layer — actual LVGL / MJPEG render (bypasses SetChatMessage). */
void ReminderUiTraceScreen(const char* via, const char* preview, int applied);

/** Speaker DAC write (BoxAudioCodec::Write) — proves audio reached hardware. */
void ReminderHwTraceSpeakerPcm(int samples, const char* source);

/** Opus packet queued / decoded to PCM. */
void ReminderHwTraceTtsPipeline(const char* stage, int ok, int detail);

/** One-line correlated snapshot (mic/spk/route/wake/voice + last UI). */
void ReminderHwTraceSnapshot(const char* tag);

#endif
