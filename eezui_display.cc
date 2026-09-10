#include "eezui_display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "board.h"
#include "application.h"
#include "device_state.h"
#include "sd_scanner.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <algorithm>
#include <font_awesome.h>

#define TAG "EezuiDisplay"

// External references to EEZUI components
extern "C" {
    void ui_init();
    void ui_tick();
    extern struct _objects_t objects;
    extern const lv_img_dsc_t img_ai;
    extern const lv_img_dsc_t img_listening;
    extern const lv_img_dsc_t img_speaking;
    extern const lv_font_t ui_font_hs20;
}

EezuiDisplay::EezuiDisplay() : emotion_player_(nullptr), emotion_canvas_(nullptr) {
    width_ = 720;
    height_ = 720;
    ESP_LOGI(TAG, "Initialize EEZUI");
    ui_init();
    
    // Hide the reference labels - they are only for style reference
    // 隐藏参考标签 - 它们仅用于样式参考
    if (objects.label_ai != nullptr) {
        lv_obj_add_flag(objects.label_ai, LV_OBJ_FLAG_HIDDEN);
    }
    if (objects.label_ai_1 != nullptr) {
        lv_obj_add_flag(objects.label_ai_1, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Configure panel_ai container: disable scrollbars when empty
    // 配置panel_ai容器：内容为空时禁用滚动条
    if (objects.panel_ai != nullptr) {
        // 禁用滚动条显示
        lv_obj_set_scrollbar_mode(objects.panel_ai, LV_SCROLLBAR_MODE_OFF);
        // 设置滚动方向为仅垂直
        lv_obj_set_scroll_dir(objects.panel_ai, LV_DIR_VER);
    }
    
    // Load theme from settings
    Settings settings("display", false);
    current_theme_ = settings.GetString("theme", "light");
    
    // Initialize status
    current_status_ = Lang::Strings::INITIALIZING;
 //   current_emotion_ = "neutral";
   current_emotion_ = "standby";
    
    // 延迟初始化标志，等待LVGL完全初始化
    delayed_init_needed_ = true;
    
    // 表情视频播放器将在UI完全初始化后，通过单独的函数初始化
    // 避免在构造函数中进行复杂的LVGL操作
    
    ESP_LOGI(TAG, "EezuiDisplay initialized");
}

EezuiDisplay::~EezuiDisplay() {
    // Clean up chat messages
    for (auto& msg : chat_messages_) {
        if (msg.label != nullptr) {
            lv_obj_del(msg.label);
        }
        if (msg.container != nullptr) {
            lv_obj_del(msg.container);
        }
    }
    chat_messages_.clear();
    
    // 清理表情视频播放器
    if (emotion_player_ != nullptr) {
        emotion_video_player_deinit(emotion_player_);
        emotion_player_ = nullptr;
    }
    
    if (emotion_canvas_ != nullptr) {
        lv_obj_del(emotion_canvas_);
        emotion_canvas_ = nullptr;
    }
}

bool EezuiDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EezuiDisplay::Unlock() {
    lvgl_port_unlock();
}

void EezuiDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (status == nullptr) return;
    
    current_status_ = status;
    
    // 更新状态标签
    if (objects.label_status != nullptr) {
        lv_label_set_text(objects.label_status, status);
    }
}

void EezuiDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (notification == nullptr) return;
    
    DisplayLockGuard lock(this);
    
    // 对于EEZUI，我们将通知显示为临时状态更新
    SetStatus(notification);
    
    // 在持续时间后安排状态重置
    // 注意：在实际实现中，您可能想要使用定时器
}

void EezuiDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void EezuiDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return;
    
    // 首次调用时初始化表情系统
    if (emotion_player_ == nullptr) {
        InitEmotionSystem();
    }
    
    DisplayLockGuard lock(this);
    current_emotion_ = emotion;
    UpdateEmotionIcon();
    
    // 播放对应的表情视频
    if (emotion_player_ != nullptr) {
        esp_err_t ret = emotion_video_player_play_emotion(emotion_player_, emotion);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "播放表情视频失败: %s, 错误: %s", emotion, esp_err_to_name(ret));
        }
    }
}

void EezuiDisplay::UpdateEmotionIcon() {
    if (objects.image_title == nullptr) return;
    
    const lv_img_dsc_t* img_src = nullptr;
    bool should_blink = false;
    
    // 根据设备状态更新图标，而不是根据情感字符串
    auto& app = Application::GetInstance();
    DeviceState device_state = app.GetDeviceState();
    
    switch (device_state) {
        case kDeviceStateListening:
            img_src = &img_listening;  // 监听状态显示监听图标
            should_blink = true;       // 监听状态需要闪烁
            break;
        case kDeviceStateSpeaking:
            img_src = &img_speaking;   // 说话状态显示说话图标
            should_blink = true;       // 说话状态需要闪烁
            break;
        default:
            img_src = &img_ai;         // 其他状态（待机、空闲等）显示AI图标
            should_blink = false;      // 其他状态不闪烁
            break;
    }
    
    if (img_src != nullptr) {
        lv_image_set_src(objects.image_title, img_src);
        
        // 为listening和speaking状态添加闪烁效果
        if (should_blink) {
            // 创建闪烁动画：透明度从255到128再到255，循环
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, objects.image_title);
            lv_anim_set_values(&a, 255, 128);
            lv_anim_set_time(&a, 500);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_repeat_delay(&a, 0);
            lv_anim_set_playback_time(&a, 500);
            lv_anim_set_playback_delay(&a, 0);
            lv_anim_set_exec_cb(&a, [](void* var, int32_t val) {
                lv_obj_set_style_img_opa((lv_obj_t*)var, val, 0);
            });
            lv_anim_start(&a);
        } else {
            // 停止闪烁动画，恢复正常透明度
            lv_anim_del(objects.image_title, nullptr);
            lv_obj_set_style_img_opa(objects.image_title, 255, 0);
        }
        
        // 设备状态图标已更新
    }
}

void EezuiDisplay::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr) return;
    
    // 系统消息为空字符串时，直接忽略（保留现有系统消息）
    if (strcmp(role, "system") == 0 && strlen(content) == 0) {
        return;
    }
    
    if (strlen(content) == 0) return; // 避免其他空消息
    
    // 检查是否需要延迟初始化
    if (delayed_init_needed_) {
        // 如果容器尺寸有效，完成延迟初始化
        if (objects.panel_ai != nullptr && 
            lv_obj_get_width(objects.panel_ai) > 0 && 
            lv_obj_get_height(objects.panel_ai) > 0) {
            delayed_init_needed_ = false;
        } else {
            return; // LVGL容器未就绪，跳过消息添加
        }
    }
    
    DisplayLockGuard lock(this);
    AddChatMessage(role, content);
}

void EezuiDisplay::AddChatMessage(const std::string& role, const std::string& content) {
    if (objects.panel_ai == nullptr) {
        ESP_LOGE(TAG, "panel_ai为空，无法添加消息");
        return;
    }
    
    // 计算容器高度和每条消息的高度
    int container_height = lv_obj_get_height(objects.panel_ai);
    int message_height = 30;  // 每条消息的高度（包含间距）
    int max_messages = container_height / message_height;
    
    // 如果消息数量达到容器容量，清空所有消息重新开始
    if (chat_messages_.size() >= max_messages) {
        // 清空容器内所有标签
        for (auto& msg : chat_messages_) {
            if (msg.label != nullptr) {
                lv_obj_del(msg.label);
            }
        }
        chat_messages_.clear();
    }
    
    // 创建新消息
    ChatMessage new_msg;
    new_msg.role = role;
    new_msg.content = content;
    new_msg.label = CreateMessageLabel(role, content);
    new_msg.container = nullptr;
    
    if (new_msg.label == nullptr) {
        ESP_LOGE(TAG, "创建标签失败！角色: %s", role.c_str());
        return;
    }
    
    // 添加到消息列表
    chat_messages_.push_back(new_msg);
    
    // 顺序排列消息（所有消息从顶部开始，不区分系统消息和对话消息）
    if (new_msg.label != nullptr) {
        // 获取参考位置
        int ai_x = -10;      // label_ai x位置
        int ai_1_x = -10;    // label_ai_1 x位置（与label_ai相同）

        // 计算当前消息的索引（从0开始）
        int message_index = chat_messages_.size() - 1;
        
        // 所有消息统一从顶部开始排列，y = -5 + 索引 * 高度
        int y_position = -5 + message_index * message_height;

        // 根据角色设置位置
        if (role == "user") {
            // 用户消息使用label_ai_1样式位置
            lv_obj_set_pos(new_msg.label, ai_1_x, y_position);
        } else if (role == "assistant") {
            // 助手消息使用label_ai样式位置
            lv_obj_set_pos(new_msg.label, ai_x, y_position);
        } else {
            // 系统消息也使用ai样式位置
            lv_obj_set_pos(new_msg.label, ai_x, y_position);
        }
    }
}

lv_obj_t* EezuiDisplay::CreateMessageLabel(const std::string& role, const std::string& content) {
    if (objects.panel_ai == nullptr) return nullptr;
    
    lv_obj_t* label = lv_label_create(objects.panel_ai);
    if (label == nullptr) return nullptr;
    
    lv_label_set_text(label, content.c_str());
    
    // 获取容器宽度，设置标签最大宽度
    int container_width = lv_obj_get_width(objects.panel_ai);
    int max_width = container_width - 25;  // 留出边距
    
    // 设置标签大小：宽度自适应但不超过容器宽度，高度自适应
    lv_obj_set_size(label, max_width, LV_SIZE_CONTENT);
    
    // 设置文本自动换行
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    
    // 根据角色应用样式
    if (role == "user") {
        // 用户消息样式（label_ai_1样式）
        lv_obj_set_style_text_color(label, lv_color_hex(0xff525963), 0);
        lv_obj_set_style_text_font(label, &ui_font_hs20, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(label, 5, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(0xffffefce), 0);
        lv_obj_set_style_bg_opa(label, 255, 0);
        lv_obj_set_style_pad_all(label, 5, 0);  // 添加内边距
    } else if (role == "assistant") {
        // 助手消息样式（label_ai样式）
        lv_obj_set_style_bg_color(label, lv_color_hex(0xff525963), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xffffffff), 0);
        lv_obj_set_style_text_font(label, &ui_font_hs20, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(label, 255, 0);
        lv_obj_set_style_radius(label, 5, 0);
        lv_obj_set_style_pad_all(label, 5, 0);  // 添加内边距
    } else {
        // 系统消息样式（类似助手但颜色不同）
        lv_obj_set_style_bg_color(label, lv_color_hex(0xffe0e0e0), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xff333333), 0);
        lv_obj_set_style_text_font(label, &ui_font_hs20, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(label, 255, 0);
        lv_obj_set_style_radius(label, 5, 0);
        lv_obj_set_style_pad_all(label, 5, 0);  // 添加内边距
    }
    
    return label;
}

void EezuiDisplay::CleanupOldMessages() {
    if (chat_messages_.size() >= max_messages_) {
        // Remove oldest messages
        int to_remove = chat_messages_.size() - max_messages_ + 1;
        for (int i = 0; i < to_remove; i++) {
            if (chat_messages_[i].label != nullptr) {
                lv_obj_del(chat_messages_[i].label);
            }
            if (chat_messages_[i].container != nullptr) {
                lv_obj_del(chat_messages_[i].container);
            }
        }
        chat_messages_.erase(chat_messages_.begin(), chat_messages_.begin() + to_remove);
    }
}

void EezuiDisplay::SetIcon(const char* icon) {
    // 对于EEZUI，我们将使用SetEmotion代替
    SetEmotion(icon);
}

void EezuiDisplay::SetPreviewImage(const lv_img_dsc_t* image) {
    // EEZUI没有预览图像概念，所以我们将忽略这个
}

void EezuiDisplay::SetTheme(const std::string& theme_name) {
    DisplayLockGuard lock(this);
    current_theme_ = theme_name;
    
    // 保存主题到设置
    Settings settings("display", true);
    settings.SetString("theme", theme_name);
}

void EezuiDisplay::UpdateStatusBar(bool update_all) {
    DisplayLockGuard lock(this);
    
    // 更新时间显示
    if (objects.label_status_time != nullptr) {
        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        if (tm->tm_year >= 2025 - 1900) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", tm);
            lv_label_set_text(objects.label_status_time, time_str);
        }
    }
    
    // 参考display.cc的刷新频率：每10秒更新一次WiFi图标

}

void EezuiDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetEmotion("sleepy");
        SetStatus("节能模式");
    } else {
    //    SetEmotion("neutral");
    SetEmotion("standby");
        SetStatus("待机中");
    }
}

// 提前初始化表情系统（SD卡就绪后立即调用）
void EezuiDisplay::InitEmotionSystemEarly() {
    if (emotion_player_ != nullptr) {
        return;  // 已经初始化
    }
    
    ESP_LOGI(TAG, "🚀 提前初始化表情视频系统（SD卡就绪）...");
    
    // 检查SD卡是否已挂载
    if (!sd_scanner_is_mounted()) {
        ESP_LOGE(TAG, "❌ SD卡未挂载，无法初始化表情系统");
        return;
    }
    
    // 硬件解码器要求16字节对齐：200->208, 650->656
    emotion_video_config_t config = {
        .output_format = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE,
        .frame_rate = 30,
        .canvas_width = 208,   // 硬件对齐要求
        .canvas_height = 656   // 硬件对齐要求
    };
    
    esp_err_t ret = emotion_video_player_init(&config, &emotion_player_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 表情视频播放器初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    
    // 注册回调
    emotion_video_player_register_frame_callback(emotion_player_, EmotionFrameCallback, this);
    
    // 创建视频画布
    CreateVideoCanvas();
    
    ESP_LOGI(TAG, "✅ 表情视频系统提前初始化成功");
}

// 延迟初始化表情系统（保留用于兼容性）
void EezuiDisplay::InitEmotionSystem() {
    if (emotion_player_ != nullptr) {
        return;  // 已经初始化
    }
    
    ESP_LOGI(TAG, "🎭 延迟初始化表情视频系统...");
    
    // 检查SD卡是否已挂载
    if (!sd_scanner_is_mounted()) {
        ESP_LOGE(TAG, "❌ SD卡未挂载，无法初始化表情系统");
        return;
    }
    
    // 硬件解码器要求16字节对齐：200->208, 650->656
    emotion_video_config_t config = {
        .output_format = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE,
        .frame_rate = 30,
        .canvas_width = 208,   // 硬件对齐要求
        .canvas_height = 656   // 硬件对齐要求
    };
    
    esp_err_t ret = emotion_video_player_init(&config, &emotion_player_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 表情视频播放器初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    
    // 注册回调
    emotion_video_player_register_frame_callback(emotion_player_, EmotionFrameCallback, this);
    
    // 创建视频画布
    CreateVideoCanvas();
    
    ESP_LOGI(TAG, "✅ 表情视频系统初始化成功");
}

// 创建视频画布
void EezuiDisplay::CreateVideoCanvas() {
    if (emotion_canvas_ != nullptr) {
        return;  // 已经创建
    }
    
    ESP_LOGI(TAG, "🎬 创建视频画布");
    
    // 获取LVGL锁
    if (!Lock(200)) {
        ESP_LOGE(TAG, "❌ 无法获取LVGL锁，无法创建画布");
        return;
    }
    
    // 获取父容器
    lv_obj_t* parent_screen = lv_screen_active();
    if (!parent_screen) {
        ESP_LOGE(TAG, "❌ 无法获取活动屏幕");
        Unlock();
        return;
    }
    
    // 创建画布对象
    emotion_canvas_ = lv_canvas_create(parent_screen);
    if (!emotion_canvas_) {
        ESP_LOGE(TAG, "❌ 创建画布失败");
        Unlock();
        return;
    }
    
    // 画布使用对齐尺寸208x656，匹配硬件解码器输出
    // 虽然视频实际内容是200x650，但解码器输出stride是208
    const uint32_t canvas_width = 208;   // 匹配硬件解码器对齐
    const uint32_t canvas_height = 656;  // 匹配硬件解码器对齐
    const uint32_t buf_size = canvas_width * canvas_height * 2; // RGB565 = 2字节/像素
    
    // 在PSRAM中分配缓冲区（64字节对齐）
    void* canvas_buf = heap_caps_aligned_calloc(64, 1, buf_size, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        ESP_LOGE(TAG, "❌ 分配画布缓冲区失败 (%lu bytes)", (unsigned long)buf_size);
        lv_obj_del(emotion_canvas_);
        emotion_canvas_ = nullptr;
        Unlock();
        return;
    }
    
    // 设置画布缓冲区
    lv_canvas_set_buffer(emotion_canvas_, canvas_buf, canvas_width, canvas_height, LV_COLOR_FORMAT_RGB565);
    
    // 基础设置（与范例保持一致）
    lv_obj_add_flag(emotion_canvas_, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    lv_obj_clear_flag(emotion_canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(emotion_canvas_, lv_color_black(), 0);
    lv_obj_set_style_border_width(emotion_canvas_, 0, 0);
    lv_obj_set_style_pad_all(emotion_canvas_, 0, 0);
    lv_obj_set_size(emotion_canvas_, canvas_width, canvas_height);
    
    // 设置位置与main_image相同 (520, 70)
    lv_obj_set_pos(emotion_canvas_, 520, 70);
    
    // 保存缓冲区指针用于清理
    lv_obj_set_user_data(emotion_canvas_, canvas_buf);
    
    // 添加删除事件回调来清理缓冲区
    lv_obj_add_event_cb(emotion_canvas_, [](lv_event_t* e) {
        lv_obj_t* target = lv_event_get_target_obj(e);
        void* canvas_buf = lv_obj_get_user_data(target);
        if (canvas_buf) {
            heap_caps_free(canvas_buf);
            ESP_LOGI("EezuiDisplay", "🗑️ 画布缓冲区已释放");
        }
    }, LV_EVENT_DELETE, nullptr);
    
    Unlock();
    
    ESP_LOGI(TAG, "✅ 视频画布创建成功 (%lux%lu, %lu bytes)", 
             (unsigned long)canvas_width, (unsigned long)canvas_height, (unsigned long)buf_size);
}

// 表情视频帧回调函数
void EezuiDisplay::EmotionFrameCallback(emotion_video_handle_t handle, uint8_t *frame_data, 
                                       uint32_t frame_size, uint32_t width, uint32_t height, void *user_data)
{
    if (!frame_data || !user_data) return;
    
    EezuiDisplay* display = static_cast<EezuiDisplay*>(user_data);
    if (!display->emotion_canvas_) return;
    
    // 第一帧打印调试信息
    static bool first_frame = true;
    if (first_frame) {
        ESP_LOGI(TAG, "📊 首帧信息: frame_size=%lu, width=%lu, height=%lu, 对齐尺寸=208x656", 
                 frame_size, width, height);
        first_frame = false;
    }
    
    // 使用非阻塞式锁，避免阻塞高频视频解码任务（与范例保持一致）
    if (!display->Lock(5)) {
        // 静默跳过本帧
        return;
    }
    
    // 获取画布图像描述符 (LVGL 9.x API)
    const lv_image_dsc_t* img_dsc = lv_canvas_get_image(display->emotion_canvas_);
    if (!img_dsc || !img_dsc->data) {
        display->Unlock();
        return;
    }
    uint8_t* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img_dsc->data));
    
    // 获取画布信息
    lv_coord_t canvas_width = lv_obj_get_width(display->emotion_canvas_);
    lv_coord_t canvas_height = lv_obj_get_height(display->emotion_canvas_);
    
    // 关键：硬件解码器输出stride是对齐后的宽度（208），而不是实际分辨率（200）
    // frame_data的每行像素数 = canvas_width（对齐后的），而不是width（实际的）
    // 所以即使width < canvas_width，也可以直接memcpy整个缓冲区
    uint32_t expected_size = canvas_width * canvas_height * 2; // RGB565 = 2字节/像素
    
    // 验证帧大小（frame_size应该等于对齐后的尺寸）
    if (frame_size < expected_size) {
        ESP_LOGW(TAG, "⚠️ 帧大小不匹配: %lu < %lu (期望%lux%lu)", 
                 frame_size, expected_size, canvas_width, canvas_height);
        display->Unlock();
        return;
    }
    
    // 直接复制整个缓冲区（包括对齐的边缘像素）
    // 虽然width=200 < canvas_width=208，但frame_data的stride就是208
    memcpy(canvas_buf, frame_data, expected_size);
    
    // 显示画布（首次显示）
    if (lv_obj_has_flag(display->emotion_canvas_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(display->emotion_canvas_, LV_OBJ_FLAG_HIDDEN);
        
        // 隐藏背景图片避免闪烁
        extern struct _objects_t objects;
        if (objects.main_image) {
            lv_obj_add_flag(objects.main_image, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // 标记画布需要重绘
    lv_obj_invalidate(display->emotion_canvas_);
    
    display->Unlock();
}
