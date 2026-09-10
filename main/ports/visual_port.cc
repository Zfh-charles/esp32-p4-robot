#include "ports/visual_port.h"

#include "display.h"

#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
#include "boards/ep-chat-p4-ml307/eezui_display_adapter.h"
#endif

VisualPort::VisualPort(Display* display) {
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
    native_display_ = dynamic_cast<EezuiDisplayAdapter*>(display);
#else
    (void)display;
#endif
}

bool VisualPort::Dispatch(Command command, uint32_t value) const {
#if CONFIG_BOARD_TYPE_EP_CHAT_P4_ML307
    auto* display = static_cast<EezuiDisplayAdapter*>(native_display_);
    if (display == nullptr) {
        return false;
    }
    switch (command) {
        case Command::kPauseMjpeg:
            display->PauseMjpegHeavyWork();
            return true;
        case Command::kResumeMjpeg:
            display->ResumeMjpegHeavyWork();
            return true;
        case Command::kResumeMjpegAtFps:
            display->ResumeMjpegHeavyWorkAtFps(value);
            return true;
        case Command::kStartDeferredPreload:
            display->StartDeferredEmotionPreload();
            return true;
        case Command::kPreloadBaseSync:
            return display->PreloadBaseEmotionsSync();
        case Command::kEnterConversation:
            display->EnterConversationPresent();
            return true;
        case Command::kLeaveConversation:
            display->LeaveConversationPresent();
            return true;
        case Command::kNotifyTtsStart:
            display->NotifyTtsStart();
            return true;
        case Command::kNotifyTtsAudioFirst:
            display->NotifyTtsAudioFirst();
            return true;
    }
#else
    (void)command;
    (void)value;
#endif
    return false;
}
