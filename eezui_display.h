#ifndef EEZUI_DISPLAY_H
#define EEZUI_DISPLAY_H

#include "display.h"
#include <lvgl.h>
#include <string>
#include <vector>

// Forward declarations
extern "C" {
    void ui_init();
    void ui_tick();
    struct _objects_t {
        lv_obj_t *main;
        lv_obj_t *main_image;
        lv_obj_t *panel_ai;
        lv_obj_t *label_ai;
        lv_obj_t *label_ai_1;
        lv_obj_t *panel_title;
        lv_obj_t *label_title;
        lv_obj_t *image_title;
        lv_obj_t *obj0;
        lv_obj_t *label_status;
        lv_obj_t *label_status_time;
        lv_obj_t *image_wifi;
    };
    extern struct _objects_t objects;
    
    // 表情视频播放器接口
    #include "emotion_video_player.h"
}

class EezuiDisplay : public Display {
private:
    // Chat message container for managing multiple messages
    struct ChatMessage {
        lv_obj_t* container;
        lv_obj_t* label;
        std::string role;
        std::string content;
    };
    
    std::vector<ChatMessage> chat_messages_;
    int max_messages_ = 20; // Maximum number of messages to keep
    
    // State tracking
    std::string current_status_;
    std::string current_emotion_;
    std::string current_theme_;
    bool delayed_init_needed_;  // 延迟初始化标志
    
    // 表情视频播放器
    emotion_video_handle_t emotion_player_;
    lv_obj_t* emotion_canvas_;  // 表情视频显示画布
    
    // Helper functions
    void AddChatMessage(const std::string& role, const std::string& content);
    void UpdateStatusBar();
    void UpdateEmotionIcon();
    void CleanupOldMessages();
    lv_obj_t* CreateMessageLabel(const std::string& role, const std::string& content);
    
    // 表情视频相关函数
    void InitEmotionSystem();  // 延迟初始化表情系统
    void CreateVideoCanvas();  // 创建视频画布
    static void EmotionFrameCallback(emotion_video_handle_t handle, uint8_t *frame_data, 
                                     uint32_t frame_size, uint32_t width, uint32_t height, void *user_data);
    
public:
    // 提前初始化表情系统（SD卡就绪后立即调用）
    void InitEmotionSystemEarly();
    

    EezuiDisplay();
    virtual ~EezuiDisplay();
    
    // Override Display interface
    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetIcon(const char* icon) override;
    virtual void SetPreviewImage(const lv_img_dsc_t* image) override;
    virtual void SetTheme(const std::string& theme_name) override;
    virtual std::string GetTheme() override { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    
    // LVGL locking implementation
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
};

#endif // EEZUI_DISPLAY_H
