#pragma once

#include <cstdint>

class Display;

// Non-owning legacy facade. It keeps board-specific display capabilities out of
// Application while preserving the existing synchronous call order.
class VisualPort final {
public:
    explicit VisualPort(Display* display);

    bool Available() const { return native_display_ != nullptr; }
    void PauseMjpegHeavyWork() const { (void)Dispatch(Command::kPauseMjpeg); }
    void ResumeMjpegHeavyWork() const { (void)Dispatch(Command::kResumeMjpeg); }
    void ResumeMjpegHeavyWorkAtFps(uint32_t fps) const {
        (void)Dispatch(Command::kResumeMjpegAtFps, fps);
    }
    void StartDeferredEmotionPreload() const { (void)Dispatch(Command::kStartDeferredPreload); }
    bool PreloadBaseEmotionsSync() const { return Dispatch(Command::kPreloadBaseSync); }
    void EnterConversationPresent() const { (void)Dispatch(Command::kEnterConversation); }
    void LeaveConversationPresent() const { (void)Dispatch(Command::kLeaveConversation); }
    void NotifyTtsStart() const { (void)Dispatch(Command::kNotifyTtsStart); }
    void NotifyTtsAudioFirst() const { (void)Dispatch(Command::kNotifyTtsAudioFirst); }

private:
    enum class Command : uint8_t {
        kPauseMjpeg,
        kResumeMjpeg,
        kResumeMjpegAtFps,
        kStartDeferredPreload,
        kPreloadBaseSync,
        kEnterConversation,
        kLeaveConversation,
        kNotifyTtsStart,
        kNotifyTtsAudioFirst,
    };

    bool Dispatch(Command command, uint32_t value = 0) const;
    void* native_display_ = nullptr;
};
