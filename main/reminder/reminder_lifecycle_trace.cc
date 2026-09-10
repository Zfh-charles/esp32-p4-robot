#include "reminder/reminder_lifecycle_trace.h"

#include "reminder/reminder_trace.h"

#include <cstring>
#include <esp_timer.h>

#if CONFIG_USE_REMINDER_POLL

namespace {

struct LcFlowState {
    bool active = false;
    char id[48];
    /* checklist — what we expected to happen */
    bool channel_open = false;
    bool display_text = false;
    bool emotion_set = false;
    bool wake_sent = false;
    bool tts_json_start = false;
    bool tts_json_stop = false;
    bool tts_audio = false;
    int tts_audio_packets = 0;
    bool feedback = false;
    bool ack = false;
    bool wake_arm = false;
    bool wake_detect = false;
    bool ring = false;
    bool preempt = false;
    bool tts_dropped = false;
    int64_t begin_us = 0;
};

LcFlowState g_standby;
LcFlowState g_user;
LcFlowState g_alarm;
LcFlowState g_proactive;

LcFlowState* StateFor(ReminderLcOwner owner) {
    switch (owner) {
        case ReminderLcOwner::Standby:
            return &g_standby;
        case ReminderLcOwner::User:
            return &g_user;
        case ReminderLcOwner::Alarm:
            return &g_alarm;
        case ReminderLcOwner::Proactive:
            return &g_proactive;
        default:
            return nullptr;
    }
}

const char* PreviewText(const char* text) {
    return (text != nullptr && text[0] != '\0') ? text : "-";
}

void UpdateChecklist(LcFlowState* s, ReminderLcPhase phase, int ok) {
    if (s == nullptr || !ok) {
        return;
    }
    switch (phase) {
        case ReminderLcPhase::ChannelOpen:
            s->channel_open = true;
            break;
        case ReminderLcPhase::WakeSent:
            s->wake_sent = true;
            break;
        case ReminderLcPhase::TtsJsonStart:
            s->tts_json_start = true;
            break;
        case ReminderLcPhase::TtsJsonStop:
            s->tts_json_stop = true;
            break;
        case ReminderLcPhase::TtsAudioFirst:
        case ReminderLcPhase::TtsAudio:
            s->tts_audio = true;
            break;
        case ReminderLcPhase::FeedbackStart:
        case ReminderLcPhase::FeedbackEnd:
            s->feedback = true;
            break;
        case ReminderLcPhase::Ack:
            s->ack = true;
            break;
        case ReminderLcPhase::WakeArm:
            s->wake_arm = true;
            break;
        case ReminderLcPhase::WakeDetect:
            s->wake_detect = true;
            break;
        case ReminderLcPhase::RingStart:
            s->ring = true;
            break;
        case ReminderLcPhase::Preempt:
            s->preempt = true;
            break;
        case ReminderLcPhase::DisplayText:
            s->display_text = true;
            break;
        case ReminderLcPhase::Emotion:
            s->emotion_set = true;
            break;
        default:
            break;
    }
}

const char* OkFail(bool v) {
    return v ? "OK" : "MISS";
}

}  // namespace

const char* ReminderLcOwnerName(ReminderLcOwner owner) {
    switch (owner) {
        case ReminderLcOwner::Standby:
            return "Standby";
        case ReminderLcOwner::User:
            return "User";
        case ReminderLcOwner::Alarm:
            return "Alarm";
        case ReminderLcOwner::Proactive:
            return "Proactive";
        default:
            return "?";
    }
}

const char* ReminderLcPhaseName(ReminderLcPhase phase) {
    switch (phase) {
        case ReminderLcPhase::Begin:
            return "begin";
        case ReminderLcPhase::End:
            return "end";
        case ReminderLcPhase::ChannelOpen:
            return "channel_open";
        case ReminderLcPhase::ChannelClose:
            return "channel_close";
        case ReminderLcPhase::RouteChange:
            return "route";
        case ReminderLcPhase::WakeArm:
            return "wake_arm";
        case ReminderLcPhase::WakeDetect:
            return "wake_detect";
        case ReminderLcPhase::TtsJsonStart:
            return "tts_json_start";
        case ReminderLcPhase::TtsJsonStop:
            return "tts_json_stop";
        case ReminderLcPhase::TtsAudioFirst:
            return "tts_audio_first";
        case ReminderLcPhase::TtsAudio:
            return "tts_audio";
        case ReminderLcPhase::SpeakerIdle:
            return "speaker_idle";
        case ReminderLcPhase::DisplayText:
            return "display_text";
        case ReminderLcPhase::DisplayStatus:
            return "display_status";
        case ReminderLcPhase::Emotion:
            return "emotion";
        case ReminderLcPhase::PollNew:
            return "poll_new";
        case ReminderLcPhase::DeliverSchedule:
            return "deliver_schedule";
        case ReminderLcPhase::WakeSent:
            return "wake_sent";
        case ReminderLcPhase::FeedbackStart:
            return "feedback_start";
        case ReminderLcPhase::FeedbackEnd:
            return "feedback_end";
        case ReminderLcPhase::Ack:
            return "ack";
        case ReminderLcPhase::RingStart:
            return "ring_start";
        case ReminderLcPhase::RingStop:
            return "ring_stop";
        case ReminderLcPhase::Preempt:
            return "preempt";
        default:
            return "?";
    }
}

void ReminderLcBegin(ReminderLcOwner owner, const char* id_or_detail) {
    auto* s = StateFor(owner);
    if (s == nullptr) {
        return;
    }
    *s = LcFlowState{};
    s->active = true;
    s->begin_us = esp_timer_get_time();
    if (id_or_detail != nullptr) {
        std::strncpy(s->id, id_or_detail, sizeof(s->id) - 1);
    }
    REMINDER_TRACE_LOG("LC | owner=%s phase=begin ok=1 id=%s",
                       ReminderLcOwnerName(owner), PreviewText(s->id));
}

void ReminderLcMark(ReminderLcOwner owner, ReminderLcPhase phase, int ok, const char* detail) {
    auto* s = StateFor(owner);
    if (s != nullptr && phase == ReminderLcPhase::TtsAudio && !ok) {
        s->tts_dropped = true;
    }
    UpdateChecklist(s, phase, ok);
    if (detail != nullptr && detail[0] != '\0') {
        REMINDER_TRACE_LOG("LC | owner=%s phase=%s ok=%d %s",
                           ReminderLcOwnerName(owner), ReminderLcPhaseName(phase), ok, detail);
    } else {
        REMINDER_TRACE_LOG("LC | owner=%s phase=%s ok=%d",
                           ReminderLcOwnerName(owner), ReminderLcPhaseName(phase), ok);
    }
}

void ReminderLcDisplay(ReminderLcOwner owner, const char* role, const char* preview, int ok) {
    auto* s = StateFor(owner);
    if (ok) {
        UpdateChecklist(s, ReminderLcPhase::DisplayText, 1);
    }
    const char* p = preview ? preview : "";
    char buf[64];
    std::strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (size_t i = 0; buf[i] != '\0'; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = ' ';
        }
    }
    REMINDER_TRACE_LOG("LC | owner=%s phase=display_text ok=%d role=%s preview=%.48s",
                       ReminderLcOwnerName(owner), ok, role ? role : "?", buf);
}

void ReminderLcEmotion(ReminderLcOwner owner, const char* emotion, int ok) {
    auto* s = StateFor(owner);
    if (ok) {
        UpdateChecklist(s, ReminderLcPhase::Emotion, 1);
    }
    REMINDER_TRACE_LOG("LC | owner=%s phase=emotion ok=%d name=%s",
                       ReminderLcOwnerName(owner), ok, PreviewText(emotion));
}

void ReminderLcTtsAudio(ReminderLcOwner owner, int packet_index, int ok) {
    auto* s = StateFor(owner);
    if (s != nullptr && ok) {
        if (packet_index <= 0) {
            packet_index = s->tts_audio_packets + 1;
        }
        s->tts_audio_packets = packet_index;
        UpdateChecklist(s, ReminderLcPhase::TtsAudio, 1);
        if (packet_index == 1) {
            UpdateChecklist(s, ReminderLcPhase::TtsAudioFirst, 1);
            REMINDER_TRACE_LOG("LC | owner=%s phase=tts_audio_first ok=1 pkt=1",
                               ReminderLcOwnerName(owner));
        }
    }
    if (packet_index <= 3 || packet_index % 50 == 0) {
        REMINDER_TRACE_LOG("LC | owner=%s phase=tts_audio ok=%d pkt=%d",
                           ReminderLcOwnerName(owner), ok, packet_index);
    }
}

const char* ProactiveRootCause(const LcFlowState* s) {
    if (s == nullptr) {
        return "unknown";
    }
    if (!s->display_text) {
        return "ui_not_updated";
    }
    if (!s->wake_sent) {
        return "wake_not_sent";
    }
    if (!s->channel_open) {
        return "channel_not_open";
    }
    if (!s->tts_json_start) {
        return "no_tts_json_from_server";
    }
    if (!s->tts_audio && s->tts_dropped) {
        return "tts_audio_dropped_by_state_machine";
    }
    if (!s->tts_audio) {
        return "tts_json_ok_but_no_audio_packets";
    }
    if (!s->tts_json_stop) {
        return "tts_stop_missing";
    }
    if (!s->ack) {
        return "ack_missing_or_cancelled";
    }
    return "-";
}

const char* UserRootCause(const LcFlowState* s) {
    if (s == nullptr) {
        return "unknown";
    }
    if (!s->wake_detect) {
        return "wake_not_detected";
    }
    if (!s->channel_open) {
        return "channel_open_failed";
    }
    if (s->tts_json_start && !s->tts_audio && s->tts_dropped) {
        return "user_tts_audio_dropped";
    }
    return "-";
}

const char* StandbyRootCause(const LcFlowState* s) {
    if (s == nullptr) {
        return "unknown";
    }
    if (!s->wake_arm) {
        return "wake_not_armed_after_idle_rearm";
    }
    return "-";
}

const char* AlarmRootCause(const LcFlowState* s) {
    if (s == nullptr) {
        return "unknown";
    }
    if (!s->ring) {
        return "alarm_ring_not_started";
    }
    return "-";
}

void ReminderLcSummary(ReminderLcOwner owner) {
    auto* s = StateFor(owner);
    if (s == nullptr || !s->active) {
        return;
    }
    const int64_t dur_ms = (esp_timer_get_time() - s->begin_us) / 1000;
    bool pass = true;
    const char* root_cause = "-";

    switch (owner) {
        case ReminderLcOwner::Proactive:
            pass = s->display_text && s->wake_sent && s->channel_open &&
                   s->tts_json_start && s->tts_audio && s->tts_json_stop && s->ack;
            if (!pass) {
                root_cause = ProactiveRootCause(s);
            }
            REMINDER_TRACE_LOG(
                "LC_SUMMARY | owner=Proactive id=%s RESULT=%s dur=%dms "
                "display=%s emotion=%s channel=%s wake_sent=%s "
                "tts_json=%s tts_audio=%s(%dpkt) tts_drop=%d feedback=%s ack=%s "
                "ROOT_CAUSE=%s",
                PreviewText(s->id), pass ? "PASS" : "FAIL", (int)dur_ms,
                OkFail(s->display_text), OkFail(s->emotion_set), OkFail(s->channel_open),
                OkFail(s->wake_sent),
                OkFail(s->tts_json_start && s->tts_json_stop),
                OkFail(s->tts_audio), s->tts_audio_packets, s->tts_dropped ? 1 : 0,
                OkFail(s->feedback), OkFail(s->ack), root_cause);
            break;
        case ReminderLcOwner::User:
            pass = s->wake_detect && s->channel_open;
            if (!pass) {
                root_cause = UserRootCause(s);
            }
            REMINDER_TRACE_LOG(
                "LC_SUMMARY | owner=User id=%s RESULT=%s dur=%dms "
                "wake_detect=%s channel=%s tts_json=%s tts_audio=%s(%dpkt) display=%s "
                "ROOT_CAUSE=%s",
                PreviewText(s->id), pass ? "PASS" : "FAIL", (int)dur_ms,
                OkFail(s->wake_detect), OkFail(s->channel_open),
                OkFail(s->tts_json_start && s->tts_json_stop),
                OkFail(s->tts_audio), s->tts_audio_packets,
                OkFail(s->display_text), root_cause);
            break;
        case ReminderLcOwner::Alarm:
            pass = s->ring;
            if (!pass) {
                root_cause = AlarmRootCause(s);
            }
            REMINDER_TRACE_LOG(
                "LC_SUMMARY | owner=Alarm RESULT=%s dur=%dms ring=%s display=%s preempt=%s "
                "ROOT_CAUSE=%s",
                pass ? "PASS" : "FAIL", (int)dur_ms,
                OkFail(s->ring), OkFail(s->display_text), OkFail(s->preempt), root_cause);
            break;
        case ReminderLcOwner::Standby:
            pass = s->wake_arm;
            if (!pass) {
                root_cause = StandbyRootCause(s);
            }
            REMINDER_TRACE_LOG(
                "LC_SUMMARY | owner=Standby RESULT=%s dur=%dms wake_arm=%s display=%s "
                "ROOT_CAUSE=%s",
                pass ? "PASS" : "FAIL", (int)dur_ms,
                OkFail(s->wake_arm), OkFail(s->display_text), root_cause);
            break;
        default:
            break;
    }
    if (!pass && root_cause[0] != '\0' && root_cause[0] != '-') {
        REMINDER_TRACE_LOG("LC_DIAG | owner=%s ACTION=check ROOT_CAUSE=%s id=%s",
                           ReminderLcOwnerName(owner), root_cause, PreviewText(s->id));
    }
    s->active = false;
}

const char* ReminderLcActiveId(ReminderLcOwner owner) {
    auto* s = StateFor(owner);
    if (s == nullptr || !s->active) {
        return "-";
    }
    return s->id;
}

#endif
