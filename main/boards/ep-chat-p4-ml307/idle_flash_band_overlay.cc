#include "idle_flash_band_overlay.h"

#include "assets.h"


namespace {

}  // namespace


bool IdleFlashBandOverlay::Initialize(lv_obj_t* parent) {
    if (image_ != nullptr) {
        return true;
    }
    if (parent == nullptr) {
        return false;
    }

    auto& assets = Assets::GetInstance();
    for (uint8_t i = 0; i < kFrameCount; ++i) {
        // Avoid permanent pointer/string tables in the P4 pre-IROM window.
        char frame_name[] = {'i', 'd', 'l', '0', '.', 'r', 'g', 'b', '\0'};
        frame_name[3] = static_cast<char>('0' + i);
        void* data = nullptr;
        size_t size = 0;
        if (!assets.GetAssetData(frame_name, data, size) || data == nullptr ||
            size != kFrameBytes) {
            return false;
        }
        auto& descriptor = descriptors_[i];
        descriptor = {};
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        descriptor.header.w = kWidth;
        descriptor.header.h = kRows;
        descriptor.header.stride = kWidth * 2U;
        descriptor.data_size = size;
        descriptor.data = static_cast<const uint8_t*>(data);
    }

    image_ = lv_image_create(parent);
    if (image_ == nullptr) {
        return false;
    }
    lv_image_set_src(image_, &descriptors_[kFrameCount - 1]);
    lv_obj_set_pos(image_, 0, kY);
    lv_obj_set_size(image_, kWidth, kRows);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_image_opa(image_, LV_OPA_COVER, 0);
    return true;
}


bool IdleFlashBandOverlay::ShowFrame(uint8_t frame) {
    if (image_ == nullptr || frame >= kFrameCount) {
        return false;
    }
    lv_image_set_src(image_, &descriptors_[frame]);
    lv_obj_clear_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(image_);
    return true;
}


void IdleFlashBandOverlay::Hide() {
    if (image_ != nullptr) {
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }
}


void IdleFlashBandOverlay::Detach() {
    image_ = nullptr;
}
