#include "wdt_contention_diag.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "Contend"

static const uint32_t kIntWdtMs = 300;
static const uint64_t kWindowUs = 15000000ULL;
static const uint64_t kHbIntervalUs = 500000ULL;
static const uint64_t kStarveAfeUs = 250000ULL;
static const uint64_t kStarveMjpegUs = 1500000ULL;
static const uint32_t kStallSoftMs = 50;
static const uint32_t kStallHardMs = 150;

typedef struct {
    uint64_t window_until_us;
    uint64_t armed_at_us;
    uint64_t last_hb_us;

    uint64_t last_afe_us;
    uint32_t last_afe_fetch_ms;
    uint32_t max_afe_fetch_ms;
    uint32_t afe_stall_n;

    uint64_t last_mjpeg_us;
    uint32_t last_memcpy_ms;
    uint32_t last_hw_ms;
    uint32_t last_cb_ms;
    uint32_t max_memcpy_ms;
    uint32_t max_hw_ms;
    uint32_t max_cb_ms;
    uint32_t mjpeg_frames;
    uint32_t mjpeg_stall_n;

    uint32_t lvgl_busy_n;
    uint32_t lvgl_hold_stall_n;
    uint32_t last_lvgl_wait_ms;
    uint32_t last_lvgl_hold_ms;
    char last_lvgl_holder[16];

    uint32_t flush_n;
    uint32_t max_flush_ms;
    uint64_t last_flush_start_us;
    uint32_t render_n;
    uint32_t max_render_ms;
    uint64_t last_render_start_us;
    uint32_t tmr_hb_n;
    char last_bc[24];
} wdt_contend_state_t;

static wdt_contend_state_t g_st;
static esp_timer_handle_t s_tmr = NULL;

static uint64_t now_us(void) {
    return (uint64_t)esp_timer_get_time();
}

static void snap_heap(unsigned* free_int, unsigned* largest_int, unsigned* free_psram) {
    *free_int = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *largest_int = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    *free_psram = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static void copy_name(char* dst, size_t dst_sz, const char* name) {
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!name || !name[0]) {
        name = "?";
    }
    strncpy(dst, name, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

bool WdtContendWindowActive(void) {
    return now_us() < g_st.window_until_us;
}

static void contend_tmr_cb(void* arg) {
    (void)arg;
    if (!WdtContendWindowActive()) {
        return;
    }
    g_st.tmr_hb_n++;
    esp_rom_printf("CONTEND_TMR_HB n=%u since_arm_ms=%u core=%d frames=%u flush_n=%u render_n=%u last_bc=%s\n",
                   (unsigned)g_st.tmr_hb_n,
                   (unsigned)((now_us() - g_st.armed_at_us) / 1000ULL),
                   (int)xPortGetCoreID(),
                   (unsigned)g_st.mjpeg_frames,
                   (unsigned)g_st.flush_n,
                   (unsigned)g_st.render_n,
                   g_st.last_bc[0] ? g_st.last_bc : "-");
}

static void ensure_tmr_started(void) {
    if (s_tmr == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &contend_tmr_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "contend_tmr",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_tmr) != ESP_OK) {
            s_tmr = NULL;
            return;
        }
    }
    esp_timer_stop(s_tmr);
    /* 20ms during kill window — denser proof interrupts still run. */
    esp_timer_start_periodic(s_tmr, 20000);
}

static void contend_arm_common(const char* reason) {
    const uint64_t t = now_us();
    memset(&g_st, 0, sizeof(g_st));
    g_st.armed_at_us = t;
    g_st.window_until_us = t + kWindowUs;
    g_st.last_hb_us = t;
    g_st.last_afe_us = t;
    copy_name(g_st.last_lvgl_holder, sizeof(g_st.last_lvgl_holder), "-");
    copy_name(g_st.last_bc, sizeof(g_st.last_bc), reason ? reason : "arm");

    ensure_tmr_started();

    unsigned free_int = 0, largest_int = 0, free_psram = 0;
    snap_heap(&free_int, &largest_int, &free_psram);
    ESP_LOGW(TAG,
             "CONTEND_ARM reason=%s window=%ums int_wdt_ms=%u | H1=flush H2=unlock_includes_flush "
             "H3=unlock_wo_flush H4=first_frame_only | free_int=%u largest_int=%u free_psram=%u",
             reason ? reason : "?",
             (unsigned)(kWindowUs / 1000ULL),
             (unsigned)kIntWdtMs,
             free_int,
             largest_int,
             free_psram);
    esp_rom_printf("CONTEND_BC phase=%s t_ms=%u hyp=H1,H2,H3,H4\n",
                   reason ? reason : "arm", (unsigned)(t / 1000ULL));
}

void WdtContendArmMjpegResume(void) {
    contend_arm_common("mjpeg_resume");
}

void WdtContendArmFaceSpeak(void) {
    /* Extend if already armed (settle→MID); else fresh 15s speak-face window. */
    if (WdtContendWindowActive()) {
        g_st.window_until_us = now_us() + kWindowUs;
        copy_name(g_st.last_bc, sizeof(g_st.last_bc), "face_rearm");
        esp_rom_printf("CONTEND_BC phase=face_rearm t_ms=%u\n", (unsigned)(now_us() / 1000ULL));
        ESP_LOGW(TAG, "CONTEND_ARM reason=face_speak rearm=1");
        return;
    }
    contend_arm_common("face_speak");
}

void WdtContendLogStall(const char* where, uint32_t cost_ms) {
    unsigned free_int = 0, largest_int = 0, free_psram = 0;
    snap_heap(&free_int, &largest_int, &free_psram);
    const uint64_t t = now_us();
    const unsigned afe_ago =
        g_st.last_afe_us ? (unsigned)((t - g_st.last_afe_us) / 1000ULL) : 999999u;
    const unsigned mjpeg_ago =
        g_st.last_mjpeg_us ? (unsigned)((t - g_st.last_mjpeg_us) / 1000ULL) : 999999u;
    const char* task = pcTaskGetName(NULL);
    ESP_LOGW(TAG,
             "SAD_DIAG CONTEND_STALL where=%s cost_ms=%u task=%s core=%d | "
             "afe_ago_ms=%u afe_fetch_ms=%u | mjpeg_ago_ms=%u memcpy_ms=%u hw_ms=%u cb_ms=%u | "
             "lvgl_busy_n=%u last_holder=%s last_bc=%s | free_int=%u largest_int=%u free_psram=%u",
             where ? where : "?",
             (unsigned)cost_ms,
             task ? task : "?",
             (int)xPortGetCoreID(),
             afe_ago,
             (unsigned)g_st.last_afe_fetch_ms,
             mjpeg_ago,
             (unsigned)g_st.last_memcpy_ms,
             (unsigned)g_st.last_hw_ms,
             (unsigned)g_st.last_cb_ms,
             (unsigned)g_st.lvgl_busy_n,
             g_st.last_lvgl_holder,
             g_st.last_bc,
             free_int,
             largest_int,
             free_psram);
}

void WdtContendNoteAfeFetch(uint32_t fetch_ms) {
    const uint64_t t = now_us();
    g_st.last_afe_us = t;
    g_st.last_afe_fetch_ms = fetch_ms;
    if (fetch_ms > g_st.max_afe_fetch_ms) {
        g_st.max_afe_fetch_ms = fetch_ms;
    }

    if (!WdtContendWindowActive()) {
        if (fetch_ms >= kStallHardMs) {
            WdtContendLogStall("afe_fetch", fetch_ms);
        }
        return;
    }

    if (fetch_ms >= kStallSoftMs) {
        g_st.afe_stall_n++;
        WdtContendLogStall("afe_fetch", fetch_ms);
    }
}

void WdtContendNoteMjpegFrame(uint32_t memcpy_ms, uint32_t hw_ms, uint32_t cb_ms) {
    const uint64_t t = now_us();
    g_st.last_mjpeg_us = t;
    g_st.last_memcpy_ms = memcpy_ms;
    g_st.last_hw_ms = hw_ms;
    g_st.last_cb_ms = cb_ms;
    g_st.mjpeg_frames++;
    if (memcpy_ms > g_st.max_memcpy_ms) {
        g_st.max_memcpy_ms = memcpy_ms;
    }
    if (hw_ms > g_st.max_hw_ms) {
        g_st.max_hw_ms = hw_ms;
    }
    if (cb_ms > g_st.max_cb_ms) {
        g_st.max_cb_ms = cb_ms;
    }

    const uint32_t total = memcpy_ms + hw_ms + cb_ms;
    const bool active = WdtContendWindowActive();

    if (active && g_st.mjpeg_frames <= 8) {
        ESP_LOGI(TAG,
                 "CONTEND_FRAME n=%u core=%d memcpy_ms=%u hw_ms=%u cb_ms=%u total_ms=%u",
                 (unsigned)g_st.mjpeg_frames,
                 (int)xPortGetCoreID(),
                 (unsigned)memcpy_ms,
                 (unsigned)hw_ms,
                 (unsigned)cb_ms,
                 (unsigned)total);
        esp_rom_printf("CONTEND_BC phase=frame_done n=%u cb_ms=%u flush_n=%u\n",
                       (unsigned)g_st.mjpeg_frames,
                       (unsigned)cb_ms,
                       (unsigned)g_st.flush_n);
        copy_name(g_st.last_bc, sizeof(g_st.last_bc), "frame_done");
    }

    if (total >= (active ? kStallSoftMs : kStallHardMs)) {
        g_st.mjpeg_stall_n++;
        if (memcpy_ms >= hw_ms && memcpy_ms >= cb_ms) {
            WdtContendLogStall("mjpeg_memcpy_psram", memcpy_ms);
        } else if (hw_ms >= cb_ms) {
            WdtContendLogStall("mjpeg_hw_decode", hw_ms);
        } else {
            WdtContendLogStall("mjpeg_frame_cb", cb_ms);
        }
    }
}

void WdtContendNoteLvglBusy(uint32_t wait_ms, const char* holder_name) {
    g_st.lvgl_busy_n++;
    g_st.last_lvgl_wait_ms = wait_ms;
    copy_name(g_st.last_lvgl_holder, sizeof(g_st.last_lvgl_holder), holder_name);
    if (WdtContendWindowActive() || wait_ms >= 5) {
        ESP_LOGW(TAG,
                 "CONTEND_LVGL event=lock_busy wait_ms=%u holder=%s skip_frame=1 busy_n=%u",
                 (unsigned)wait_ms,
                 g_st.last_lvgl_holder,
                 (unsigned)g_st.lvgl_busy_n);
    }
}

void WdtContendNoteLvglHold(uint32_t hold_ms, uint32_t bytes, const char* task_name) {
    g_st.last_lvgl_hold_ms = hold_ms;
    if (hold_ms < (WdtContendWindowActive() ? kStallSoftMs : 120u)) {
        return;
    }
    g_st.lvgl_hold_stall_n++;
    ESP_LOGW(TAG,
             "CONTEND_LVGL event=hold_long hold_ms=%u bytes=%u task=%s | resource=canvas_memcpy+invalidate",
             (unsigned)hold_ms,
             (unsigned)bytes,
             task_name && task_name[0] ? task_name : "?");
    WdtContendLogStall("lvgl_hold", hold_ms);
}

void WdtContendBreadcrumb(const char* phase) {
    const uint64_t t = now_us();
    copy_name(g_st.last_bc, sizeof(g_st.last_bc), phase ? phase : "?");
    esp_rom_printf("CONTEND_BC phase=%s t_ms=%u core=%d frames=%u flush_n=%u\n",
                   g_st.last_bc,
                   (unsigned)(t / 1000ULL),
                   (int)xPortGetCoreID(),
                   (unsigned)g_st.mjpeg_frames,
                   (unsigned)g_st.flush_n);
    if (WdtContendWindowActive()) {
    }
}

void WdtContendNoteCbPhases(uint32_t wait_ms,
                            uint32_t getbuf_ms,
                            uint32_t canvas_memcpy_ms,
                            uint32_t invalidate_ms,
                            uint32_t unlock_ms,
                            uint32_t bytes) {
    if (!WdtContendWindowActive()) {
        return;
    }
    ESP_LOGW(TAG,
             "CONTEND_CB wait_ms=%u getbuf_ms=%u canvas_memcpy_ms=%u invalidate_ms=%u "
             "unlock_ms=%u bytes=%u flush_n=%u render_n=%u | resource=lvgl_canvas_path",
             (unsigned)wait_ms,
             (unsigned)getbuf_ms,
             (unsigned)canvas_memcpy_ms,
             (unsigned)invalidate_ms,
             (unsigned)unlock_ms,
             (unsigned)bytes,
             (unsigned)g_st.flush_n,
             (unsigned)g_st.render_n);
    esp_rom_printf("CONTEND_BC phase=cb_phases canvas_ms=%u inv_ms=%u unlock_ms=%u flush_n=%u\n",
                   (unsigned)canvas_memcpy_ms,
                   (unsigned)invalidate_ms,
                   (unsigned)unlock_ms,
                   (unsigned)g_st.flush_n);
    copy_name(g_st.last_bc, sizeof(g_st.last_bc), "cb_phases");
}

uint32_t WdtContendFlushCount(void) {
    return g_st.flush_n;
}

void WdtContendNoteUnlock(uint32_t unlock_wall_ms, uint32_t flush_n_before) {
    if (!WdtContendWindowActive()) {
        return;
    }
    const uint32_t flush_n_after = g_st.flush_n;
    const uint32_t flush_delta = (flush_n_after >= flush_n_before)
                                    ? (flush_n_after - flush_n_before)
                                    : 0;
    const char* task = pcTaskGetName(NULL);
    esp_rom_printf(
        "CONTEND_UNLOCK wall_ms=%u flush_before=%u flush_after=%u delta=%u "
        "render_n=%u task=%s core=%d\n",
        (unsigned)unlock_wall_ms,
        (unsigned)flush_n_before,
        (unsigned)flush_n_after,
        (unsigned)flush_delta,
        (unsigned)g_st.render_n,
        task ? task : "?",
        (int)xPortGetCoreID());
    copy_name(g_st.last_bc, sizeof(g_st.last_bc), "unlock");

    /* Explicit hypothesis verdicts for the unlock window. */
    if (unlock_wall_ms >= 20 && flush_delta > 0) {
        esp_rom_printf(
            "CONTEND_HYP id=H2 result=SUPPORTED unlock_ms=%u flush_delta=%u "
            "=> unlock_wall_includes_LVGL_flush_preempt\n",
            (unsigned)unlock_wall_ms,
            (unsigned)flush_delta);
        ESP_LOGW(TAG,
                 "CONTEND_HYP id=H2 SUPPORTED unlock_ms=%u flush_delta=%u "
                 "(Give returned after LVGL flush ran)",
                 (unsigned)unlock_wall_ms,
                 (unsigned)flush_delta);
    } else if (unlock_wall_ms >= 20 && flush_delta == 0) {
        esp_rom_printf(
            "CONTEND_HYP id=H3 result=SUPPORTED unlock_ms=%u flush_delta=0 "
            "=> unlock_cost_without_flush_count (bus/cache or unhooked flush)\n",
            (unsigned)unlock_wall_ms);
        ESP_LOGW(TAG,
                 "CONTEND_HYP id=H3 SUPPORTED unlock_ms=%u flush_delta=0 "
                 "(large unlock without FLUSH_START/FINISH count)",
                 (unsigned)unlock_wall_ms);
    } else if (unlock_wall_ms < 5 && flush_delta == 0) {
        esp_rom_printf(
            "CONTEND_HYP id=H2 result=NOT_IN_UNLOCK unlock_ms=%u "
            "=> flush_not_folded_into_unlock_measurement\n",
            (unsigned)unlock_wall_ms);
    }
}

void WdtContendNoteFlush(bool is_start, uint32_t area_px) {
    /* In window: rom_printf + ESP_LOG — last line before WDT is smoking gun for H1.
     * Outside window: still bump last_bc lightly so BootTrace-adjacent deaths leave a clue. */
    const char* task = pcTaskGetName(NULL);
    if (!WdtContendWindowActive()) {
        if (is_start) {
            copy_name(g_st.last_bc, sizeof(g_st.last_bc), "flush_x");
        }
        return;
    }
    if (is_start) {
        g_st.last_flush_start_us = now_us();
        g_st.flush_n++;
        esp_rom_printf("CONTEND_FLUSH event=start n=%u area_px=%u core=%d task=%s\n",
                       (unsigned)g_st.flush_n,
                       (unsigned)area_px,
                       (int)xPortGetCoreID(),
                       task ? task : "?");
        ESP_LOGW(TAG, "FACE_FLUSH start n=%u area_px=%u core=%d task=%s s1ao",
                 (unsigned)g_st.flush_n, (unsigned)area_px, (int)xPortGetCoreID(),
                 task ? task : "?");
        copy_name(g_st.last_bc, sizeof(g_st.last_bc), "flush_start");
        return;
    }
    uint32_t flush_ms = 0;
    if (g_st.last_flush_start_us) {
        flush_ms = (uint32_t)((now_us() - g_st.last_flush_start_us) / 1000ULL);
    }
    if (flush_ms > g_st.max_flush_ms) {
        g_st.max_flush_ms = flush_ms;
    }
    esp_rom_printf(
        "CONTEND_FLUSH event=finish n=%u cost_ms=%u area_px=%u max_ms=%u core=%d task=%s\n",
        (unsigned)g_st.flush_n,
        (unsigned)flush_ms,
        (unsigned)area_px,
        (unsigned)g_st.max_flush_ms,
        (int)xPortGetCoreID(),
        task ? task : "?");
    ESP_LOGW(TAG, "FACE_FLUSH finish n=%u flush_ms=%u area_px=%u max_ms=%u core=%d s1ao",
             (unsigned)g_st.flush_n, (unsigned)flush_ms, (unsigned)area_px,
             (unsigned)g_st.max_flush_ms, (int)xPortGetCoreID());
    copy_name(g_st.last_bc, sizeof(g_st.last_bc), "flush_done");
    if (flush_ms >= kStallSoftMs) {
        esp_rom_printf("CONTEND_HYP id=H1 result=FLUSH_SLOW cost_ms=%u\n", (unsigned)flush_ms);
        WdtContendLogStall("lvgl_flush", flush_ms);
    }
}

void WdtContendNoteRender(bool is_start, uint32_t area_px) {
    if (!WdtContendWindowActive()) {
        return;
    }
    const char* task = pcTaskGetName(NULL);
    if (is_start) {
        g_st.last_render_start_us = now_us();
        g_st.render_n++;
        esp_rom_printf("CONTEND_RENDER event=start n=%u area_px=%u core=%d task=%s\n",
                       (unsigned)g_st.render_n,
                       (unsigned)area_px,
                       (int)xPortGetCoreID(),
                       task ? task : "?");
        copy_name(g_st.last_bc, sizeof(g_st.last_bc), "render_start");
        return;
    }
    uint32_t render_ms = 0;
    if (g_st.last_render_start_us) {
        render_ms = (uint32_t)((now_us() - g_st.last_render_start_us) / 1000ULL);
    }
    if (render_ms > g_st.max_render_ms) {
        g_st.max_render_ms = render_ms;
    }
    esp_rom_printf(
        "CONTEND_RENDER event=finish n=%u cost_ms=%u area_px=%u max_ms=%u core=%d task=%s\n",
        (unsigned)g_st.render_n,
        (unsigned)render_ms,
        (unsigned)area_px,
        (unsigned)g_st.max_render_ms,
        (int)xPortGetCoreID(),
        task ? task : "?");
    copy_name(g_st.last_bc, sizeof(g_st.last_bc), "render_done");
}

void WdtContendMainTick(void) {
    if (!WdtContendWindowActive()) {
        return;
    }

    const uint64_t t = now_us();
    const unsigned since_arm_ms = (unsigned)((t - g_st.armed_at_us) / 1000ULL);
    const unsigned afe_ago =
        g_st.last_afe_us ? (unsigned)((t - g_st.last_afe_us) / 1000ULL) : 999999u;
    const unsigned mjpeg_ago =
        g_st.last_mjpeg_us ? (unsigned)((t - g_st.last_mjpeg_us) / 1000ULL) : 999999u;

    if (g_st.mjpeg_frames > 0 && (t - g_st.last_afe_us) >= kStarveAfeUs) {
        unsigned free_int = 0, largest_int = 0, free_psram = 0;
        snap_heap(&free_int, &largest_int, &free_psram);
        ESP_LOGE(TAG,
                 "CONTEND_STARVE side=AFE ago_ms=%u while mjpeg_frames=%u mjpeg_ago_ms=%u "
                 "since_arm_ms=%u | free_int=%u free_psram=%u",
                 afe_ago,
                 (unsigned)g_st.mjpeg_frames,
                 mjpeg_ago,
                 since_arm_ms,
                 free_int,
                 free_psram);
    }

    if (g_st.mjpeg_frames == 0 && (t - g_st.armed_at_us) >= kStarveMjpegUs) {
        ESP_LOGE(TAG,
                 "CONTEND_STARVE side=MJPEG frames=0 since_arm_ms=%u afe_ago_ms=%u",
                 since_arm_ms,
                 afe_ago);
    }

    /* H4: first frame done, still in window — note for reboot-after correlation. */
    if (g_st.mjpeg_frames == 1 && since_arm_ms < 2000) {
        static uint32_t s_h4_noted;
        if (s_h4_noted != g_st.armed_at_us) {
            s_h4_noted = (uint32_t)g_st.armed_at_us;
            esp_rom_printf(
                "CONTEND_HYP id=H4 result=FIRST_FRAME_DONE frames=1 since_arm_ms=%u "
                "last_bc=%s flush_n=%u — if rst follows with no more frames, H4 supported\n",
                since_arm_ms,
                g_st.last_bc,
                (unsigned)g_st.flush_n);
        }
    }

    if ((t - g_st.last_hb_us) < kHbIntervalUs) {
        return;
    }
    g_st.last_hb_us = t;

    unsigned free_int = 0, largest_int = 0, free_psram = 0;
    snap_heap(&free_int, &largest_int, &free_psram);
    ESP_LOGI(TAG,
             "CONTEND_HB since_arm_ms=%u | "
             "afe_ago_ms=%u fetch_ms=%u | frames=%u flush_n=%u max_flush_ms=%u "
             "render_n=%u last_bc=%s tmr_hb_n=%u | free_int=%u free_psram=%u",
             since_arm_ms,
             afe_ago,
             (unsigned)g_st.last_afe_fetch_ms,
             (unsigned)g_st.mjpeg_frames,
             (unsigned)g_st.flush_n,
             (unsigned)g_st.max_flush_ms,
             (unsigned)g_st.render_n,
             g_st.last_bc,
             (unsigned)g_st.tmr_hb_n,
             free_int,
             free_psram);
}
