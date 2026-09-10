#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <lvgl.h>


class IdleFlashBandOverlay {
public:
    static constexpr uint8_t kFrameCount = 4;
    static constexpr uint16_t kWidth = 480;
    static constexpr uint16_t kRows = 48;
    static constexpr uint16_t kY = 384;
    static constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kRows * 2U;

    /** Must be called while the LVGL lock is held. Missing assets fail closed. */
    bool Initialize(lv_obj_t* parent);
    /** Must be called while the LVGL lock is held. */
    bool ShowFrame(uint8_t frame);
    /** Must be called while the LVGL lock is held. */
    void Hide();
    /** Parent deletion owns the LVGL child; only forget the borrowed pointer. */
    void Detach();

    bool Ready() const { return image_ != nullptr; }

private:
    std::array<lv_image_dsc_t, kFrameCount> descriptors_{};
    lv_obj_t* image_ = nullptr;
};
