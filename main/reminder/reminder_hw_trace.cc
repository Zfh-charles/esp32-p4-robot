#include "reminder/reminder_hw_trace.h"

#include "reminder/reminder_trace.h"

#include <cstring>

#if CONFIG_USE_REMINDER_POLL

namespace {

struct HwUiState {
    int mic = -1;
    int spk = -1;
    char route[12];
    int wake_run = -1;
    int voice_run = -1;
    char last_text_role[16];
    char last_text[64];
    char last_emotion[24];
    char last_status[32];
    char last_tts[24];
    char last_screen_via[24];
    int speaker_pcm_writes = 0;
    int tts_decode_ok = 0;
};

HwUiState g_state;

void SetRoute(HwUiState* s, const char* route) {
    if (s == nullptr || route == nullptr) {
        return;
    }
    std::strncpy(s->route, route, sizeof(s->route) - 1);
}

const char* Preview(const char* text) {
    return (text != nullptr && text[0] != '\0') ? text : "-";
}

void CopyPreview(char* dst, size_t dst_len, const char* src) {
    if (dst == nullptr || dst_len == 0) {
        return;
    }
    const char* p = Preview(src);
    std::strncpy(dst, p, dst_len - 1);
    dst[dst_len - 1] = '\0';
    for (size_t i = 0; dst[i] != '\0'; ++i) {
        if (dst[i] == '\n' || dst[i] == '\r') {
            dst[i] = ' ';
        }
    }
}

}  // namespace

void ReminderHwTraceMic(int on, const char* reason) {
    g_state.mic = on;
    REMINDER_TRACE_LOG("HW | mic=%d spk=%d route=%s reason=%s",
                       on, g_state.spk >= 0 ? g_state.spk : -1,
                       g_state.route[0] ? g_state.route : "?",
                       Preview(reason));
}

void ReminderHwTraceSpk(int on, const char* reason) {
    g_state.spk = on;
    REMINDER_TRACE_LOG("HW | mic=%d spk=%d route=%s reason=%s",
                       g_state.mic >= 0 ? g_state.mic : -1, on,
                       g_state.route[0] ? g_state.route : "?",
                       Preview(reason));
}

void ReminderHwTraceRoute(const char* route, int mic, int spk, const char* reason, int force) {
    g_state.mic = mic;
    g_state.spk = spk;
    SetRoute(&g_state, route);
    REMINDER_TRACE_LOG("HW | mic=%d spk=%d route=%s force=%d reason=%s",
                       mic, spk, Preview(route), force, Preview(reason));
}

void ReminderHwTraceWake(int enable, int running, const char* route) {
    g_state.wake_run = running;
    if (route != nullptr) {
        SetRoute(&g_state, route);
    }
    REMINDER_TRACE_LOG("HW | wake=%d running=%d route=%s event=%s",
                       enable, running,
                       g_state.route[0] ? g_state.route : "?",
                       enable ? "wake_enable" : "wake_disable");
}

void ReminderHwTraceVoice(int running) {
    g_state.voice_run = running;
    REMINDER_TRACE_LOG("HW | voice=%d route=%s event=%s",
                       running, g_state.route[0] ? g_state.route : "?",
                       running ? "voice_start" : "voice_stop");
}

void ReminderHwTraceSpeakerOp(const char* op, const char* route, int hold) {
    if (route != nullptr) {
        SetRoute(&g_state, route);
    }
    REMINDER_TRACE_LOG("HW | spk_op=%s route=%s hold=%d mic=%d spk=%d",
                       Preview(op), g_state.route[0] ? g_state.route : "?",
                       hold, g_state.mic, g_state.spk);
}

void ReminderHwTracePowerMicOff(long in_age_ms, int wake, int voice, const char* route) {
    REMINDER_TRACE_LOG("HW | mic=0 event=power_timeout in_age=%ldms wake=%d voice=%d route=%s",
                       in_age_ms, wake, voice, Preview(route));
    g_state.mic = 0;
}

void ReminderUiTraceText(const char* role, const char* preview, int applied) {
    char buf[64];
    CopyPreview(buf, sizeof(buf), preview);
    if (applied) {
        CopyPreview(g_state.last_text_role, sizeof(g_state.last_text_role), role);
        CopyPreview(g_state.last_text, sizeof(g_state.last_text), preview);
    }
    REMINDER_TRACE_LOG("UI | kind=text applied=%d role=%s preview=%.56s",
                       applied, Preview(role), buf);
}

void ReminderUiTraceEmotion(const char* name, int applied) {
    if (applied) {
        CopyPreview(g_state.last_emotion, sizeof(g_state.last_emotion), name);
    }
    REMINDER_TRACE_LOG("UI | kind=emotion applied=%d name=%s",
                       applied, Preview(name));
}

void ReminderUiTraceStatus(const char* status) {
    CopyPreview(g_state.last_status, sizeof(g_state.last_status), status);
    REMINDER_TRACE_LOG("UI | kind=status text=%s", Preview(status));
}

void ReminderUiTraceTts(const char* event, const char* detail, int ok) {
    CopyPreview(g_state.last_tts, sizeof(g_state.last_tts), event);
    REMINDER_TRACE_LOG("UI | kind=tts event=%s ok=%d detail=%s",
                       Preview(event), ok, Preview(detail));
}

void ReminderUiTraceScreen(const char* via, const char* preview, int applied) {
    char buf[64];
    CopyPreview(buf, sizeof(buf), preview);
    if (applied) {
        CopyPreview(g_state.last_text, sizeof(g_state.last_text), preview);
        CopyPreview(g_state.last_screen_via, sizeof(g_state.last_screen_via), via);
    }
    REMINDER_TRACE_LOG("UI | kind=screen applied=%d via=%s preview=%.56s",
                       applied, Preview(via), buf);
}

void ReminderHwTraceSpeakerPcm(int samples, const char* source) {
    g_state.speaker_pcm_writes++;
    const int n = g_state.speaker_pcm_writes;
    if (n == 1 || n <= 3 || n % 100 == 0) {
        REMINDER_TRACE_LOG("HW | spk_pcm=1 samples=%d write#=%d source=%s spk=%d",
                           samples, n, Preview(source),
                           g_state.spk >= 0 ? g_state.spk : -1);
    }
}

void ReminderHwTraceTtsPipeline(const char* stage, int ok, int detail) {
    if (ok && stage != nullptr && std::strcmp(stage, "decoded") == 0) {
        g_state.tts_decode_ok++;
    }
    if (detail <= 3 || detail % 50 == 0 || !ok) {
        REMINDER_TRACE_LOG("HW | tts_pipe stage=%s ok=%d detail=%d",
                           Preview(stage), ok, detail);
    }
}

void ReminderHwTraceSnapshot(const char* tag) {
    REMINDER_TRACE_LOG(
        "HW_SNAP | tag=%s mic=%d spk=%d route=%s wake=%d voice=%d "
        "spk_pcm_writes=%d tts_decoded=%d "
        "ui_text=%.32s ui_emotion=%s ui_status=%s ui_tts=%s via=%s",
        Preview(tag), g_state.mic, g_state.spk,
        g_state.route[0] ? g_state.route : "?",
        g_state.wake_run, g_state.voice_run,
        g_state.speaker_pcm_writes, g_state.tts_decode_ok,
        g_state.last_text[0] ? g_state.last_text : "-",
        g_state.last_emotion[0] ? g_state.last_emotion : "-",
        g_state.last_status[0] ? g_state.last_status : "-",
        g_state.last_tts[0] ? g_state.last_tts : "-",
        g_state.last_screen_via[0] ? g_state.last_screen_via : "-");
}

#endif
