#include "screen_presenter.h"

#include "eezui_display_adapter.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include "lvgl.h"
#include "reminder/boot_trace.h"
#include "src/draw/lv_draw_buf.h"
#include "src/others/snapshot/lv_snapshot.h"

#define TAG "ScreenPresenter"

bool ScreenPresenter::EnsurePresentBuf() {
    if (host_ == nullptr) return false;
    const uint32_t px = (uint32_t)host_->PresenterWidth() * (uint32_t)host_->PresenterHeight();
    if (px == 0) return false;
    if (present_buf_ != nullptr && present_px_ == px) return true;
    FreePresentBuf();
    present_buf_ = static_cast<uint16_t*>(
        heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!present_buf_) present_buf_ = static_cast<uint16_t*>(malloc(px * sizeof(uint16_t)));
    if (!present_buf_) return false;
    present_px_ = px;
    memset(present_buf_, 0, px * sizeof(uint16_t));
    return true;
}

void ScreenPresenter::FreePresentBuf() {
    if (present_buf_ != nullptr) {
        heap_caps_free(present_buf_);
        present_buf_ = nullptr;
        present_px_ = 0;
    }
}

void ScreenPresenter::FreeOverlayCache() {
    if (overlay_rgb_ != nullptr) {
        heap_caps_free(overlay_rgb_);
        overlay_rgb_ = nullptr;
    }
    overlay_cap_px_ = 0;
    overlay_w_ = 0;
    overlay_h_ = 0;
    overlay_valid_ = false;
}

void ScreenPresenter::ApplyOverlayCache(int W, int H) {
    if (!overlay_valid_ || !overlay_rgb_ || overlay_w_ <= 0 || overlay_h_ <= 0 || !present_buf_) return;
    for (int y = 0; y < overlay_h_; y++) {
        const int dy = overlay_y_ + y;
        if (dy < 0 || dy >= H) continue;
        for (int x = 0; x < overlay_w_; x++) {
            const int dx = overlay_x_ + x;
            if (dx >= 0 && dx < W) present_buf_[dy * W + dx] = overlay_rgb_[y * overlay_w_ + x];
        }
    }
}

bool ScreenPresenter::StoreOverlayCache(const uint16_t* src, int stride_px, int x0, int y0,
                                        int sw, int sh) {
    if (!src || sw <= 0 || sh <= 0 || stride_px <= 0) {
        overlay_valid_ = false;
        return false;
    }
    const uint32_t need = (uint32_t)sw * (uint32_t)sh;
    if (overlay_rgb_ == nullptr || overlay_cap_px_ < need) {
        FreeOverlayCache();
        overlay_rgb_ = static_cast<uint16_t*>(
            heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!overlay_rgb_) overlay_rgb_ = static_cast<uint16_t*>(malloc(need * sizeof(uint16_t)));
        if (!overlay_rgb_) return false;
        overlay_cap_px_ = need;
    }
    for (int y = 0; y < sh; y++) memcpy(overlay_rgb_ + y * sw, src + y * stride_px, (size_t)sw * sizeof(uint16_t));
    overlay_x_ = x0;
    overlay_y_ = y0;
    overlay_w_ = sw;
    overlay_h_ = sh;
    overlay_valid_ = true;
    return true;
}

bool ScreenPresenter::CacheFace(const uint8_t* rgb, uint32_t size, uint32_t w, uint32_t h) {
    if (!rgb || size == 0 || w == 0 || h == 0) return false;
    if (face_rgb_ == nullptr || face_cap_ < size) {
        if (face_rgb_ != nullptr) heap_caps_free(face_rgb_);
        face_rgb_ = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!face_rgb_) face_rgb_ = static_cast<uint8_t*>(malloc(size));
        if (!face_rgb_) {
            face_cap_ = 0;
            return false;
        }
        face_cap_ = size;
    }
    memcpy(face_rgb_, rgb, size);
    face_size_ = size;
    face_w_ = w;
    face_h_ = h;
    return true;
}

esp_err_t ScreenPresenter::RenderCompose() {
    if (!present_buf_ || host_ == nullptr) return ESP_ERR_INVALID_STATE;
    const int W = host_->PresenterWidth();
    const int H = host_->PresenterHeight();
    if (W <= 0 || H <= 0) return ESP_ERR_INVALID_STATE;

    const int64_t t0 = esp_timer_get_time();
    if (face_rgb_ && face_size_ > 0 && face_w_ > 0 && face_h_ > 0) {
        const uint32_t copy_w = (face_w_ < (uint32_t)W) ? face_w_ : (uint32_t)W;
        const uint32_t copy_h = (face_h_ < (uint32_t)H) ? face_h_ : (uint32_t)H;
        const auto* src = reinterpret_cast<const uint16_t*>(face_rgb_);
        if (copy_w == (uint32_t)W && copy_h == (uint32_t)H) {
            memcpy(present_buf_, src, (size_t)W * (size_t)H * sizeof(uint16_t));
        } else {
            memset(present_buf_, 0, (size_t)W * (size_t)H * sizeof(uint16_t));
            for (uint32_t y = 0; y < copy_h; y++) memcpy(present_buf_ + y * W, src + y * face_w_, copy_w * sizeof(uint16_t));
        }
    } else {
        memset(present_buf_, 0, (size_t)W * (size_t)H * sizeof(uint16_t));
    }
    const int64_t face_ms = (esp_timer_get_time() - t0) / 1000;

#if LV_USE_SNAPSHOT
    if (!dirty_text_ && overlay_valid_) {
        ApplyOverlayCache(W, H);
        ESP_LOGW(TAG, "CTRL PRESENT DIAG render face_ms=%d snap=reuse box=%dx%d@%d,%d",
                 (int)face_ms, overlay_w_, overlay_h_, overlay_x_, overlay_y_);
        return ESP_OK;
    }
    lv_obj_t* box = host_->PresenterDialogueBox();
    if (box == nullptr) {
        ESP_LOGW(TAG, "CTRL PRESENT DIAG render face_ms=%d snap=skip_no_box", (int)face_ms);
        return ESP_OK;
    }
    BootTraceMark("PRESENT_SNAP", "lock");
    esp_rom_printf("!!PRESENT_SNAP lock\n");
    const int64_t tl = esp_timer_get_time();
    if (!host_->PresenterSafeLVGLLock(80)) {
        esp_rom_printf("!!PRESENT_SNAP lock_fail\n");
        if (overlay_valid_) ApplyOverlayCache(W, H);
        ESP_LOGW(TAG, "CTRL PRESENT DIAG snapshot skip=lock_fail face_ms=%d reuse=%d",
                 (int)face_ms, overlay_valid_ ? 1 : 0);
        return ESP_OK;
    }
    const int64_t lock_ms = (esp_timer_get_time() - tl) / 1000;
    BootTraceMark("PRESENT_SNAP", "take");
    esp_rom_printf("!!PRESENT_SNAP take\n");
    const int64_t ts = esp_timer_get_time();
    lv_draw_buf_t* snap = lv_snapshot_take(box, LV_COLOR_FORMAT_RGB565);
    const int64_t snap_ms = (esp_timer_get_time() - ts) / 1000;
    esp_rom_printf("!!PRESENT_SNAP done ms=%d\n", (int)snap_ms);
    if (snap == nullptr || snap->data == nullptr) {
        host_->PresenterSafeLVGLUnlock();
        if (overlay_valid_) ApplyOverlayCache(W, H);
        ESP_LOGW(TAG, "CTRL PRESENT DIAG snapshot miss face_ms=%d lock_ms=%d snap_ms=%d",
                 (int)face_ms, (int)lock_ms, (int)snap_ms);
        return ESP_OK;
    }
    BootTraceMark("PRESENT_SNAP", "overlay");
    const int64_t to = esp_timer_get_time();
    lv_area_t coords;
    lv_obj_get_coords(box, &coords);
    const int x0 = coords.x1;
    const int y0 = coords.y1;
    const int sw = (int)snap->header.w;
    const int sh = (int)snap->header.h;
    const int stride_px = (int)(snap->header.stride / 2);
    const auto* s = reinterpret_cast<const uint16_t*>(snap->data);
    StoreOverlayCache(s, stride_px, x0, y0, sw, sh);
    ApplyOverlayCache(W, H);
    lv_draw_buf_destroy(snap);
    host_->PresenterSafeLVGLUnlock();
    const int64_t overlay_ms = (esp_timer_get_time() - to) / 1000;
    ESP_LOGW(TAG,
             "CTRL PRESENT DIAG render face_ms=%d lock_ms=%d snap_ms=%d "
             "overlay_ms=%d box=%dx%d@%d,%d",
             (int)face_ms, (int)lock_ms, (int)snap_ms, (int)overlay_ms, sw, sh, x0, y0);
#else
    ESP_LOGW(TAG, "CTRL PRESENT snapshot disabled face_ms=%d", (int)face_ms);
#endif
    return ESP_OK;
}
