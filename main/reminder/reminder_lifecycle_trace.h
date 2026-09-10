#pragma once

#include <cstdint>

/**
 * Unified lifecycle trace for one-pass serial diagnosis.
 * Filter serial: ReminderTrace
 * Phase lines:  ReminderTrace: LC | owner=... phase=... ok=... ...
 * Summary lines: ReminderTrace: LC_SUMMARY | owner=... RESULT=PASS|FAIL ...
 *
 * Owners: Standby | User | Alarm | Proactive
 */

enum class ReminderLcOwner {
    Standby,
    User,
    Alarm,
    Proactive,
};

enum class ReminderLcPhase {
    /* shared */
    Begin,
    End,
    ChannelOpen,
    ChannelClose,
    RouteChange,
    WakeArm,
    WakeDetect,
    /* audio / tts */
    TtsJsonStart,
    TtsJsonStop,
    TtsAudioFirst,
    TtsAudio,
    SpeakerIdle,
    /* ui */
    DisplayText,
    DisplayStatus,
    Emotion,
    /* proactive only */
    PollNew,
    DeliverSchedule,
    WakeSent,
    FeedbackStart,
    FeedbackEnd,
    Ack,
    /* alarm only */
    RingStart,
    RingStop,
    Preempt,
};

#if CONFIG_USE_REMINDER_POLL

const char* ReminderLcOwnerName(ReminderLcOwner owner);
const char* ReminderLcPhaseName(ReminderLcPhase phase);

/** Start a traced flow (clears per-owner counters for Proactive/User/Alarm). */
void ReminderLcBegin(ReminderLcOwner owner, const char* id_or_detail);

/** Mark a phase; ok=1 success / reached, ok=0 failed / skipped. */
void ReminderLcMark(ReminderLcOwner owner, ReminderLcPhase phase, int ok, const char* detail);

/** Shorthand helpers */
void ReminderLcDisplay(ReminderLcOwner owner, const char* role, const char* preview, int ok);
void ReminderLcEmotion(ReminderLcOwner owner, const char* emotion, int ok);
void ReminderLcTtsAudio(ReminderLcOwner owner, int packet_index, int ok);

/** Emit LC_SUMMARY with checklist; call at flow end (idle rearm, ack, cancel, alarm clear). */
void ReminderLcSummary(ReminderLcOwner owner);

/** Current proactive delivery id (for cross-module logging). */
const char* ReminderLcActiveId(ReminderLcOwner owner);

#endif
