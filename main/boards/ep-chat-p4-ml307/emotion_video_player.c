#include "emotion_video_player.h"
#include "mjpeg_frame_source.h"
#include "mjpeg_decoder_player_adapter.h"
#include "mjpeg_index_reader.h"
#include "mjpeg_playback_clock_player_adapter.h"
#include "mjpeg_present_sink_esp.h"
#include "mjpeg_runtime_selection.h"
#include "wdt_contention_diag.h"
#include "stack_diag.h"
#include "face_route_v2.h"

// 优化：定义安全的信号量操作宏，使用更短的超时时间
#define SAFE_SEMAPHORE_TAKE(sem, timeout_ms) \
    (xSemaphoreTake((sem), pdMS_TO_TICKS(timeout_ms)) == pdTRUE)

#define SAFE_SEMAPHORE_TAKE_CRITICAL(sem) \
    (xSemaphoreTake((sem), pdMS_TO_TICKS(2000)) == pdTRUE)

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_video_dec.h"
#include "esp_video_dec_mjpeg.h"
#include "esp_video_codec_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>

static const char *TAG = "EmotionVideoPlayer";

// 强制使用ESP32-P4硬件MJPEG解码器
static const uint32_t g_hw_decoder_cc = ESP_VIDEO_DEC_HW_MJPEG_TAG;

// SD卡挂载点常量
#define MOUNT_POINT "/sdcard"

// FreeRTOS事件定义
#define PLAYER_EVENT_SWITCH     BIT0
#define PLAYER_EVENT_EXIT       BIT1

// Legacy MJPEG decoding still owns hardware decode plus the frame callback.
// Keep it above the project 8 KiB deep-work floor; do not move cores as part
// of this independent stack correction.
#define EMOTION_DECODE_TASK_STACK_SIZE (12 * 1024)

// 表情定义和映射
#define BASE_EMOTIONS_COUNT     6
#define TOTAL_EMOTIONS_COUNT    6

// 默认的待机表情名称
#define DEFAULT_STANDBY_EMOTION "standby"

// 🚀 新增：帧索引表支持
#define MAX_FRAMES_PER_VIDEO    300  // 每个视频最多300帧（10秒@30fps）
// L3: 解码循环周期性让步，给 idle/系统任务让出时间片
#define MJPEG_L3_YIELD_INTERVAL 10

/**
 * @brief 帧索引条目（关键优化：避免运行时扫描）
 */
typedef mjpeg_index_entry_t frame_index_entry_t;

_Static_assert(MAX_FRAMES_PER_VIDEO == MJPEG_INDEX_READER_MAX_FRAMES,
               "legacy and IndexReader frame limits must match");
_Static_assert(sizeof(frame_index_entry_t) == sizeof(mjpeg_index_entry_t),
               "IndexReader entry layout must remain compatible");

/**
 * @brief 表情定义结构体
 */
typedef struct {
    const char* name;
    const char* file;
    emotion_type_t type;
    int cache_index;
} emotion_def_t;

/**
 * @brief 表情映射定义
 * 🔥 所有表情默认循环播放，只有收到切换信号才切换
 */
static const emotion_def_t g_emotion_defs[] = {
    {"standby",    "standby.mjpeg",   EMOTION_TYPE_BASE,    0},
    {"neutral",    "neutral.mjpeg",   EMOTION_TYPE_BASE,    1},
    {"happy",      "happy.mjpeg",     EMOTION_TYPE_BASE,    2},
    {"sad",        "sad.mjpeg",       EMOTION_TYPE_BASE,    3},
    {"angry",      "angry.mjpeg",     EMOTION_TYPE_BASE,    4},
    {"loving",     "loving.mjpeg",    EMOTION_TYPE_BASE,    5},
};

/**
 * @brief 表情别名映射表
 */
typedef struct {
    const char* alias;
    const char* target;
} emotion_alias_t;

static const emotion_alias_t g_emotion_aliases[] = {
    // 待机表情组
    {"idle",            "standby"},
    {"waiting",         "standby"},
    {"ready",           "standby"},
    
    // 开心表情组
    {"laughing",        "happy"},
    {"funny",           "happy"},
    {"shocked",         "happy"},
    {"confident",       "happy"},
    {"delicious",       "happy"},
    {"excited",         "happy"},
    
    // 悲伤表情组
    {"crying",          "sad"},
    {"silly",           "sad"},
    {"confused",        "sad"},
    
    // 语义不确定/自省类：不得误投到强烈负面表情
    {"embarrassed",     "neutral"},
    
    // 爱意表情组
    {"surprised",       "loving"},
    {"thinking",        "neutral"},
    {"winking",         "loving"},
    {"cool",            "loving"},
    {"relaxed",         "loving"},
    {"kissy",           "loving"},
    {"sleepy",          "neutral"},
    
    // 其他别名映射到基础表情
    {"gear",            "neutral"},
    {"config",          "neutral"},
    {"settings",        "neutral"},
    {"playing_music",   "happy"},
    {"listening",       "neutral"},
    {"charging",        "standby"},
    {"microchip_ai",    "standby"},
};

#define ALIAS_COUNT (sizeof(g_emotion_aliases) / sizeof(emotion_alias_t))

/**
 * @brief 视频缓存结构体（优化版：增加帧索引表）
 */
typedef struct {
    uint8_t *buffer;                                    // 视频数据缓存
    size_t size;                                        // 缓存大小
    bool ready;                                         // 是否已加载就绪
    char file_name[32];                                 // 文件名
    
    // 🚀 关键优化：帧索引表
    frame_index_entry_t frame_index[MAX_FRAMES_PER_VIDEO];
    int frame_count;                                    // 帧总数
    bool index_built;                                   // 索引表是否已建立
} video_cache_t;

/**
 * @brief 表情视频播放器内部结构（优化版本 + 🔥立即切换支持 + 循环播放修复）
 */
typedef struct emotion_video_player_t {
    // 基础配置
    emotion_video_config_t config;
    emotion_video_state_t state;
    
    // ESP32-P4硬件MJPEG解码器
    esp_video_dec_handle_t hw_dec_handle;
    esp_video_dec_cfg_t hw_dec_cfg;
    esp_video_dec_caps_t hw_caps;
    
    // 硬件解码缓冲区
    uint8_t *input_buffer;
    uint32_t input_buffer_size;
    uint8_t *output_buffer;
    uint32_t output_buffer_size;
    
    // 6个基础表情的缓存
    video_cache_t video_caches[TOTAL_EMOTIONS_COUNT];
    int current_cache_index;
    int current_frame_index;        // 🚀 新增：当前播放的帧索引
    bool base_emotions_loaded;
    
    // FreeRTOS同步和任务
    TaskHandle_t decode_task_handle;
    EventGroupHandle_t event_group;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t cache_mutex;
    
    // 帧信息和统计
    esp_video_codec_frame_info_t frame_info;
    uint32_t current_frame;
    uint32_t decode_error_count;
    
    // 回调函数
    emotion_video_event_cb_t event_cb;
    emotion_video_frame_cb_t frame_cb;
    void *event_user_data;
    void *frame_user_data;
    
    // 当前播放状态
    char current_emotion[32];
    char requested_emotion[32];
    bool switch_requested;
    
    // 🔥 新增：立即切换控制
    bool immediate_switch;          // 是否需要立即切换（打断当前播放）
    bool allow_interrupt;           // 是否允许被打断（某些特殊表情可能不允许）
    
    // 表情队列系统
    emotion_queue_item_t emotion_queue[EMOTION_QUEUE_MAX_SIZE];
    int queue_head;
    int queue_tail;
    int queue_size;
    uint32_t next_queue_id;
    SemaphoreHandle_t queue_mutex;
    
    // 🚀 优化：高精度帧率控制
    uint64_t frame_deadline_us;
    uint32_t frame_interval_us;
    uint32_t normal_frame_interval_us;
    uint64_t last_frame_time_us;
    uint64_t last_decode_success_us;
    uint64_t last_resume_us;
    uint64_t resume_warmup_until_us;
    uint64_t soft_start_until_us;
    uint64_t last_stall_diag_us;
    
    volatile bool decode_paused;
    volatile bool preload_started;

    /* S1: one RGB565 still per base emotion (boot seed; conversation blit-only). */
    struct {
        uint8_t *rgb;
        uint32_t size;
        uint32_t w;
        uint32_t h;
        bool ready;
    } seed_stills[TOTAL_EMOTIONS_COUNT];
    bool seed_stills_ready;
    
} emotion_video_player_t;

// 异步加载任务的参数结构
typedef struct {
    emotion_video_player_t *player;
    emotion_video_event_cb_t event_cb;
    void *user_data;
} async_load_params_t;

// ==================== 内部函数声明 ====================

static void decode_task(void *arg);
static esp_err_t hw_decode_frame_optimized(emotion_video_player_t *player);
static esp_err_t init_hw_decoder(emotion_video_player_t *player);
static esp_err_t deinit_hw_decoder(emotion_video_player_t *player);
static void async_load_task(void *arg);
static esp_err_t load_video_to_cache(emotion_video_player_t *player, int cache_index);
static esp_err_t build_frame_index(emotion_video_player_t *player, int cache_index);
static esp_err_t validate_mjpeg_frame(const uint8_t *frame_data, size_t frame_size);
static void change_state(emotion_video_player_t *player, emotion_video_state_t new_state);
static void notify_event(emotion_video_player_t *player, uint32_t event_bits);
static const emotion_def_t* find_emotion_def(const char *emotion_name);
static const char* resolve_emotion_alias(const char *emotion_name);
static esp_err_t switch_to_emotion(emotion_video_player_t *player, const char *emotion_name);

// 🔥 新增：预加载任务和循环播放处理
static void preload_base_emotions(emotion_video_player_t *player);
static void preload_emotions_task(void *arg);
static void handle_emotion_playback_complete(emotion_video_player_t *player);
static void reset_current_emotion_position(emotion_video_player_t *player);

// 队列管理内部函数
static esp_err_t queue_add_emotion_internal(emotion_video_player_t *player, const char *emotion_name);
static esp_err_t queue_get_next_emotion(emotion_video_player_t *player, char *emotion_name, size_t name_size);
static void queue_clear_internal(emotion_video_player_t *player);
static bool queue_is_empty(emotion_video_player_t *player);
static bool queue_is_full(emotion_video_player_t *player);
static bool queue_contains_emotion(emotion_video_player_t *player, const char *emotion_name);

// 🚀 新增：高精度延迟函数
static inline void precise_delay_us(uint64_t delay_us) {
    if (delay_us > 1000) {
        // 大于1ms，使用系统延迟
        vTaskDelay(pdMS_TO_TICKS((delay_us + 500) / 1000));
    } else if (delay_us > 100) {
        // 100us-1ms，使用yield
        uint64_t start = esp_timer_get_time();
        while ((esp_timer_get_time() - start) < delay_us) {
            taskYIELD();
        }
    } else if (delay_us > 0) {
        // 小于100us，忙等待
        uint64_t start = esp_timer_get_time();
        while ((esp_timer_get_time() - start) < delay_us) {
            __asm__ __volatile__("nop");
        }
    }
}

// ==================== 公共函数实现 ====================

esp_err_t emotion_video_player_init(const emotion_video_config_t *config, emotion_video_handle_t *handle)
{
    if (!config || !handle) {
        ESP_LOGE(TAG, "Invalid arguments for player init");
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)calloc(1, sizeof(emotion_video_player_t));
    if (!player) {
        ESP_LOGE(TAG, "Failed to allocate memory for player");
        return ESP_ERR_NO_MEM;
    }

    // 复制配置
    player->config = *config;
    player->state = EMOTION_VIDEO_STATE_IDLE;
    player->decode_error_count = 0;
    player->frame_interval_us = 1000000 / config->frame_rate;
    player->normal_frame_interval_us = player->frame_interval_us;
    
    // 初始化播放状态
    player->current_cache_index = 0;
    player->current_frame_index = 0;
    player->base_emotions_loaded = false;
    player->switch_requested = false;
    strcpy(player->current_emotion, DEFAULT_STANDBY_EMOTION);
    memset(player->requested_emotion, 0, sizeof(player->requested_emotion));
    
    // 🔥 初始化立即切换控制
    player->immediate_switch = false;
    player->allow_interrupt = true;
    
    // 初始化表情队列系统
    memset(player->emotion_queue, 0, sizeof(player->emotion_queue));
    player->queue_head = 0;
    player->queue_tail = 0;
    player->queue_size = 0;
    player->next_queue_id = 0;
    
    // 初始化缓存和帧索引表
    for (int i = 0; i < TOTAL_EMOTIONS_COUNT; i++) {
        player->video_caches[i].buffer = NULL;
        player->video_caches[i].size = 0;
        player->video_caches[i].ready = false;
        player->video_caches[i].frame_count = 0;
        player->video_caches[i].index_built = false;
        strncpy(player->video_caches[i].file_name, g_emotion_defs[i].file, 
                sizeof(player->video_caches[i].file_name) - 1);
        player->video_caches[i].file_name[sizeof(player->video_caches[i].file_name) - 1] = '\0';
    }

    // 创建FreeRTOS同步对象
    player->state_mutex = xSemaphoreCreateMutex();
    player->cache_mutex = xSemaphoreCreateMutex();
    player->queue_mutex = xSemaphoreCreateMutex();
    player->event_group = xEventGroupCreate();
    
    if (!player->state_mutex || !player->cache_mutex || !player->queue_mutex || !player->event_group) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS synchronization objects");
        if (player->state_mutex) vSemaphoreDelete(player->state_mutex);
        if (player->cache_mutex) vSemaphoreDelete(player->cache_mutex);
        if (player->queue_mutex) vSemaphoreDelete(player->queue_mutex);
        if (player->event_group) vEventGroupDelete(player->event_group);
        free(player);
        return ESP_ERR_NO_MEM;
    }

    // 注册ESP32-P4硬件MJPEG解码器
    esp_vc_err_t vc_ret = esp_video_dec_register_mjpeg();
    if (vc_ret != ESP_VC_ERR_OK) {
        ESP_LOGE(TAG, "❌ 硬件MJPEG解码器注册失败: %d", vc_ret);
        goto cleanup_and_fail;
    }

    // 初始化硬件解码器
    esp_err_t ret = init_hw_decoder(player);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ 硬件解码器初始化失败");
        esp_video_dec_unregister_mjpeg();
        goto cleanup_and_fail;
    }

    // 🚀 关键优化：提高任务优先级到6，确保视频解码优先执行
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        decode_task, "emotion_decode", 
        EMOTION_DECODE_TASK_STACK_SIZE, player,
        2,
        &player->decode_task_handle,
        1
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create decode task");
        deinit_hw_decoder(player);
        esp_video_dec_unregister_mjpeg();
        goto cleanup_and_fail;
    }
    
    ESP_LOGI(TAG, "✅ Emotion decode task started (priority: 2, stack: %u, core: 1)",
             (unsigned)EMOTION_DECODE_TASK_STACK_SIZE);

    *handle = player;

    // 🔥 修复1：初始化时立即加载并播放standby表情，避免黑屏
    ESP_LOGI(TAG, "🎬 初始化：加载并播放standby表情...");
    
    const emotion_def_t *standby_def = find_emotion_def(DEFAULT_STANDBY_EMOTION);
    if (standby_def) {
        int standby_index = standby_def->cache_index;
        
        esp_err_t ret = load_video_to_cache(player, standby_index);
        if (ret == ESP_OK) {
            ret = build_frame_index(player, standby_index);
            if (ret == ESP_OK) {
                player->current_cache_index = standby_index;
                player->current_frame_index = 0;
                player->video_caches[standby_index].ready = true;
                change_state(player, EMOTION_VIDEO_STATE_PLAYING);
                ESP_LOGI(TAG, "✅ Standby表情加载并开始播放");
            } else {
                ESP_LOGW(TAG, "⚠️ Standby表情帧索引建立失败");
            }
        } else {
            ESP_LOGW(TAG, "⚠️ Standby表情加载失败: %s", esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "📝 其他基础表情将在唤醒稳定后预加载");

    return ESP_OK;

cleanup_and_fail:
    if (player->state_mutex) vSemaphoreDelete(player->state_mutex);
    if (player->cache_mutex) vSemaphoreDelete(player->cache_mutex);
    if (player->queue_mutex) vSemaphoreDelete(player->queue_mutex);
    if (player->event_group) vEventGroupDelete(player->event_group);
    free(player);
    return ESP_FAIL;
}

esp_err_t emotion_video_player_deinit(emotion_video_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    notify_event(player, PLAYER_EVENT_EXIT);
    
    if (player->decode_task_handle) {
        vTaskDelete(player->decode_task_handle);
        player->decode_task_handle = NULL;
    }

    deinit_hw_decoder(player);

    for (int i = 0; i < TOTAL_EMOTIONS_COUNT; i++) {
        if (player->video_caches[i].buffer) {
            heap_caps_free(player->video_caches[i].buffer);
            player->video_caches[i].buffer = NULL;
        }
        if (player->seed_stills[i].rgb) {
            heap_caps_free(player->seed_stills[i].rgb);
            player->seed_stills[i].rgb = NULL;
            player->seed_stills[i].ready = false;
        }
    }
    player->seed_stills_ready = false;

    esp_video_dec_unregister_mjpeg();

    if (player->event_group) vEventGroupDelete(player->event_group);
    if (player->state_mutex) vSemaphoreDelete(player->state_mutex);
    if (player->cache_mutex) vSemaphoreDelete(player->cache_mutex);
    if (player->queue_mutex) vSemaphoreDelete(player->queue_mutex);
    
    free(player);

    return ESP_OK;
}

// 🔥 新增：扩展的播放表情函数（支持立即切换控制）
esp_err_t emotion_video_player_play_emotion_ex(emotion_video_handle_t handle, 
                                                const char *emotion_name,
                                                bool immediate)
{
    if (!handle || !emotion_name) {
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved_name);
    
    if (!def) {
        ESP_LOGW(TAG, "⚠️ 未找到表情定义: %s，使用neutral", resolved_name);
        resolved_name = "neutral";
        def = find_emotion_def(resolved_name);
    }
    
    if (strcmp(resolved_name, DEFAULT_STANDBY_EMOTION) == 0 && 
        strcmp(player->current_emotion, DEFAULT_STANDBY_EMOTION) == 0 &&
        player->state == EMOTION_VIDEO_STATE_PLAYING) {
        ESP_LOGW(TAG, "CTRL EMO_REQ skip=already_standby paused=%d",
                 (int)player->decode_paused);
        return ESP_OK;
    }
    
    if (strcmp(player->current_emotion, resolved_name) == 0 && queue_is_empty(player)) {
        ESP_LOGW(TAG, "CTRL EMO_REQ skip=same_playing name=%s paused=%d",
                 resolved_name, (int)player->decode_paused);
        return ESP_OK;
    }

    bool cache_ready = false;
    bool index_built = false;
    int cache_index = def ? def->cache_index : -1;
    if (cache_index >= 0 && cache_index < TOTAL_EMOTIONS_COUNT) {
        xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
        cache_ready = player->video_caches[cache_index].ready;
        index_built = player->video_caches[cache_index].index_built;
        xSemaphoreGive(player->cache_mutex);
    }
    ESP_LOGW(TAG,
             "CTRL EMO_REQ raw=%s resolved=%s cur=%s paused=%d cache_idx=%d ready=%d index=%d",
             emotion_name, resolved_name, player->current_emotion,
             (int)player->decode_paused, cache_index, (int)cache_ready, (int)index_built);
    
    if (immediate) {
        xSemaphoreTake(player->state_mutex, portMAX_DELAY);
        bool can_interrupt = player->allow_interrupt;
        xSemaphoreGive(player->state_mutex);
        
        if (!can_interrupt) {
            ESP_LOGW(TAG, "⚠️ 当前表情不允许被打断: %s，将加入队列", player->current_emotion);
            return queue_add_emotion_internal(player, resolved_name);
        }
        
        queue_clear_internal(player);
        
        esp_err_t ret = queue_add_emotion_internal(player, resolved_name);
        if (ret != ESP_OK) {
            return ret;
        }
        
        xSemaphoreTake(player->state_mutex, portMAX_DELAY);
        player->immediate_switch = true;
        strncpy(player->requested_emotion, resolved_name, sizeof(player->requested_emotion) - 1);
        player->requested_emotion[sizeof(player->requested_emotion) - 1] = '\0';
        player->switch_requested = true;
        xSemaphoreGive(player->state_mutex);
        
        notify_event(player, PLAYER_EVENT_SWITCH);
        
        ESP_LOGI(TAG, "🔥 立即切换表情: %s -> %s", player->current_emotion, resolved_name);
        return ESP_OK;
    }
    
    if (queue_contains_emotion(player, resolved_name)) {
        ESP_LOGD(TAG, "表情 %s 已在队列中，跳过重复添加", resolved_name);
        return ESP_OK;
    }
    
    esp_err_t ret = queue_add_emotion_internal(player, resolved_name);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ 添加表情到队列失败: %s", resolved_name);
        return ret;
    }
    
    ESP_LOGI(TAG, "📥 表情已加入队列: %s (队列大小: %d)", resolved_name, player->queue_size);
    
    if (player->state != EMOTION_VIDEO_STATE_PLAYING) {
        char next_emotion[32];
        if (queue_get_next_emotion(player, next_emotion, sizeof(next_emotion)) == ESP_OK) {
            xSemaphoreTake(player->state_mutex, portMAX_DELAY);
            strncpy(player->requested_emotion, next_emotion, sizeof(player->requested_emotion) - 1);
            player->requested_emotion[sizeof(player->requested_emotion) - 1] = '\0';
            player->switch_requested = true;
            xSemaphoreGive(player->state_mutex);
            
            notify_event(player, PLAYER_EVENT_SWITCH);
        }
    }
    
    return ESP_OK;
}

esp_err_t emotion_video_player_play_emotion(emotion_video_handle_t handle, const char *emotion_name)
{
    return emotion_video_player_play_emotion_ex(handle, emotion_name, true);
}

esp_err_t emotion_video_player_set_interruptible(emotion_video_handle_t handle, bool allow_interrupt)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    xSemaphoreTake(player->state_mutex, portMAX_DELAY);
    player->allow_interrupt = allow_interrupt;
    xSemaphoreGive(player->state_mutex);
    
    ESP_LOGI(TAG, "表情打断设置: %s", allow_interrupt ? "允许" : "不允许");
    return ESP_OK;
}

bool emotion_video_player_is_interruptible(emotion_video_handle_t handle)
{
    if (!handle) {
        return false;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    xSemaphoreTake(player->state_mutex, portMAX_DELAY);
    bool result = player->allow_interrupt;
    xSemaphoreGive(player->state_mutex);
    
    return result;
}

emotion_video_state_t emotion_video_player_get_state(emotion_video_handle_t handle)
{
    if (!handle) {
        return EMOTION_VIDEO_STATE_ERROR;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    emotion_video_state_t state;

    xSemaphoreTake(player->state_mutex, portMAX_DELAY);
    state = player->state;
    xSemaphoreGive(player->state_mutex);

    return state;
}

const char* emotion_video_player_get_current_emotion(emotion_video_handle_t handle)
{
    if (!handle) {
        return NULL;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    return player->current_emotion;
}

esp_err_t emotion_video_player_register_event_callback(emotion_video_handle_t handle, 
                                                      emotion_video_event_cb_t event_cb, void *user_data)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    player->event_cb = event_cb;
    player->event_user_data = user_data;

    return ESP_OK;
}

esp_err_t emotion_video_player_register_frame_callback(emotion_video_handle_t handle, 
                                                      emotion_video_frame_cb_t frame_cb, void *user_data)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    player->frame_cb = frame_cb;
    player->frame_user_data = user_data;

    return ESP_OK;
}

esp_err_t emotion_video_player_release_cache(emotion_video_handle_t handle, const char *emotion_name)
{
    (void)handle;
    (void)emotion_name;
    ESP_LOGW(TAG, "⚠️ 所有表情常驻内存，不支持释放缓存");
    return ESP_ERR_NOT_SUPPORTED;
}

bool emotion_video_player_has_emotion(const char *emotion_name)
{
    if (!emotion_name) {
        return false;
    }
    
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    return find_emotion_def(resolved_name) != NULL;
}

int emotion_video_player_get_emotion_type(const char *emotion_name)
{
    if (!emotion_name) {
        return -1;
    }
    
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved_name);
    
    return def ? def->type : -1;
}

const char* emotion_video_player_get_video_file(const char *emotion_name)
{
    if (!emotion_name) {
        return NULL;
    }
    
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved_name);
    
    return def ? def->file : NULL;
}

esp_err_t emotion_video_player_load_all_async(emotion_video_handle_t handle, 
                                               emotion_video_event_cb_t event_cb, 
                                               void *user_data)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    async_load_params_t *params = (async_load_params_t *)malloc(sizeof(async_load_params_t));
    if (!params) {
        ESP_LOGE(TAG, "❌ 分配异步加载参数内存失败");
        return ESP_ERR_NO_MEM;
    }
    
    params->player = player;
    params->event_cb = event_cb;
    params->user_data = user_data;
    
    BaseType_t ret = xTaskCreate(
        async_load_task,
        "emotion_loader",
        4096,
        params,
        2,
        NULL
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "❌ 创建异步加载任务失败");
        free(params);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ 异步加载任务创建成功");
    return ESP_OK;
}

/**
 * @brief 设置当前表情是否循环播放（保留API兼容性）
 * 🔥 注意：当前实现中所有表情默认循环，此函数保留但不起作用
 */
esp_err_t emotion_video_player_set_loop(emotion_video_handle_t handle, bool loop)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 🔥 所有表情默认循环播放，忽略此设置
    (void)loop;
    ESP_LOGD(TAG, "🔁 所有表情默认循环播放，忽略set_loop调用");
    return ESP_OK;
}

/**
 * @brief 获取当前表情是否循环播放（保留API兼容性）
 * 🔥 注意：当前实现中所有表情默认循环，始终返回true
 */
bool emotion_video_player_get_loop(emotion_video_handle_t handle)
{
    if (!handle) {
        return false;
    }
    
    // 🔥 所有表情默认循环播放
    return true;
}

// ==================== 内部函数实现 ====================

/**
 * @brief 🔥 重置当前表情的播放位置到开头（用于循环播放）
 * 
 * 这个函数用于在表情需要循环播放时，直接重置帧索引，
 * 而不需要触发完整的切换流程，从而减少开销。
 */
static void reset_current_emotion_position(emotion_video_player_t *player)
{
    if (!SAFE_SEMAPHORE_TAKE(player->cache_mutex, 50)) {
        ESP_LOGW(TAG, "⚠️ 重置位置时获取缓存锁超时");
        return;
    }
    
    if (player->current_cache_index >= 0 && 
        player->current_cache_index < TOTAL_EMOTIONS_COUNT) {
        player->current_frame_index = 0;  // 🔥 使用帧索引表的索引
        player->current_frame = 0;
        ESP_LOGD(TAG, "🔄 重置表情播放位置: %s", player->current_emotion);
    }
    
    xSemaphoreGive(player->cache_mutex);
}

/**
 * @brief 🔥 处理表情播放完成事件
 * 
 * 简化逻辑：所有表情默认循环播放，只有收到切换信号才切换
 * - 播放完成后直接重置位置继续播放
 * - 不再自动切换到standby
 */
static void handle_emotion_playback_complete(emotion_video_player_t *player)
{
    // 🔥 简化逻辑：直接重置播放位置，继续循环播放当前表情
    reset_current_emotion_position(player);
    
    // 重置帧率控制的deadline
    player->frame_deadline_us = esp_timer_get_time();
    player->last_decode_success_us = player->frame_deadline_us;
    
    ESP_LOGD(TAG, "🔁 表情循环播放: %s", player->current_emotion);
}

// 🔥 核心优化：解码任务支持立即切换检测和SWITCHING状态
static void decode_task(void *arg)
{
    emotion_video_player_t *player = (emotion_video_player_t *)arg;
    EventBits_t event_bits;
    uint64_t last_diag_us = esp_timer_get_time();
    ESP_LOGI(TAG, "🎬 解码任务启动");
    
    player->frame_deadline_us = esp_timer_get_time();
    player->last_decode_success_us = player->frame_deadline_us;
    player->last_stall_diag_us = 0;

    while (1) {
        event_bits = xEventGroupWaitBits(
            player->event_group,
            PLAYER_EVENT_SWITCH | PLAYER_EVENT_EXIT,
            pdFALSE,
            pdFALSE,
            0
        );

        if (event_bits & PLAYER_EVENT_EXIT) {
            ESP_LOGI(TAG, "🛑 解码任务收到退出信号");
            break;
        }

        // 🔥 修复2：检查是否处于切换状态
        xSemaphoreTake(player->state_mutex, pdMS_TO_TICKS(5));
        emotion_video_state_t current_state = player->state;
        xSemaphoreGive(player->state_mutex);
        
        // 🔥 如果正在切换，暂停解码，等待切换完成
        if (current_state == EMOTION_VIDEO_STATE_SWITCHING) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // 🔥 检查立即切换请求
        if (player->state == EMOTION_VIDEO_STATE_PLAYING) {
            xSemaphoreTake(player->state_mutex, pdMS_TO_TICKS(5));
            bool need_immediate_switch = player->immediate_switch;
            xSemaphoreGive(player->state_mutex);
            
            if (need_immediate_switch) {
                event_bits |= PLAYER_EVENT_SWITCH;
                ESP_LOGI(TAG, "🔥 检测到立即切换请求，打断当前播放");
            }
        }

        // 处理表情切换事件
        if (event_bits & PLAYER_EVENT_SWITCH) {
            if (xSemaphoreTake(player->state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (player->switch_requested) {
                    char emotion_to_switch[32];
                    strncpy(emotion_to_switch, player->requested_emotion, sizeof(emotion_to_switch));
                    emotion_to_switch[sizeof(emotion_to_switch) - 1] = '\0';
                    player->switch_requested = false;
                    player->immediate_switch = false;
                    xSemaphoreGive(player->state_mutex);
                    
                    // 🔥 修复：切换前检查是否与当前表情相同
                    if (strcmp(emotion_to_switch, player->current_emotion) != 0) {
                        ESP_LOGI(TAG, "🔄 切换表情: %s -> %s", 
                                player->current_emotion, emotion_to_switch);
                        
                        // 🔥 修复2：切换前先设置为 SWITCHING 状态
                        change_state(player, EMOTION_VIDEO_STATE_SWITCHING);
                        
                        esp_err_t ret = switch_to_emotion(player, emotion_to_switch);
                        if (ret != ESP_OK) {
                            ESP_LOGW(TAG, "CTRL EMO_SWITCH fail name=%s err=%s paused=%d",
                                    emotion_to_switch, esp_err_to_name(ret),
                                    (int)player->decode_paused);
                            change_state(player, EMOTION_VIDEO_STATE_PLAYING);
                        } else if (player->decode_paused) {
                            // v9-r0: keep paused; no LVGL force-paint (bypass path comes later).
                            ESP_LOGW(TAG,
                                     "CTRL EMO_SWITCH ok name=%s cache_idx=%d paused=1 will_paint=0",
                                     emotion_to_switch, player->current_cache_index);
                        } else {
                            ESP_LOGW(TAG,
                                     "CTRL EMO_SWITCH ok name=%s cache_idx=%d paused=0 will_paint=loop",
                                     emotion_to_switch, player->current_cache_index);
                        }
                    } else {
                        // 🔥 目标表情与当前相同，只需重置播放位置
                        ESP_LOGD(TAG, "📍 表情相同，重置播放位置: %s", emotion_to_switch);
                        reset_current_emotion_position(player);
                    }

                    player->frame_deadline_us = esp_timer_get_time();
                } else {
                    xSemaphoreGive(player->state_mutex);
                }
            } else {
                ESP_LOGW(TAG, "⚠️ 获取状态锁超时，跳过表情切换");
            }
            xEventGroupClearBits(player->event_group, PLAYER_EVENT_SWITCH);
            continue;
        }

        if (player->state == EMOTION_VIDEO_STATE_PLAYING) {
            // v8/v9-r0: pause decode during wake/conversation (no force burst).
            if (player->decode_paused) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            uint64_t current_time = esp_timer_get_time();
            const mjpeg_playback_clock_player_binding_t clock_binding = {
                &player->frame_interval_us, &player->normal_frame_interval_us,
                &player->frame_deadline_us, &player->resume_warmup_until_us,
                &player->soft_start_until_us, &player->last_frame_time_us,
                &player->last_decode_success_us, &player->last_stall_diag_us,
                &player->current_frame, &player->decode_error_count,
            };
#if EMOTION_VIDEO_USE_PLAYBACK_CLOCK
            const mjpeg_playback_clock_poll_t clock_poll =
                mjpeg_playback_clock_player_poll_direct(
                    &clock_binding, current_time);
#else
            const mjpeg_playback_clock_poll_t clock_poll =
                mjpeg_playback_clock_player_poll(
                    &clock_binding, current_time, false);
#endif
            if (clock_poll.soft_start_finished) {
                ESP_LOGI(TAG, "MJPEG soft-start finished, restore %lu fps",
                         (unsigned long)(1000000U / (player->normal_frame_interval_us ? player->normal_frame_interval_us : 1U)));
            }
            if (clock_poll.stall_diag) {
                ESP_LOGW(TAG, "STALL_DIAG core=%d no decoded frame for %ums (cache_idx=%d frame_idx=%d state=%d free_int=%u free_psram=%u)",
                         xPortGetCoreID(),
                         (unsigned)((current_time - player->last_decode_success_us) / 1000ULL),
                         player->current_cache_index, player->current_frame_index,
                         (int)player->state,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }
            if (clock_poll.yield_now) {
                taskYIELD();
            }
            if (clock_poll.action == MJPEG_CLOCK_ACTION_WARMUP) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (clock_poll.action == MJPEG_CLOCK_ACTION_DECODE) {
                esp_err_t ret = hw_decode_frame_optimized(player);
                const mjpeg_playback_decode_result_t result =
                    ret == ESP_OK ? MJPEG_CLOCK_DECODE_OK
                                  : (ret == ESP_ERR_NOT_FOUND
                                         ? MJPEG_CLOCK_DECODE_EOF
                                         : MJPEG_CLOCK_DECODE_ERROR);
#if EMOTION_VIDEO_USE_PLAYBACK_CLOCK
                const mjpeg_playback_clock_effect_t effect =
                    mjpeg_playback_clock_player_on_decode_result_direct(
                        &clock_binding, result, current_time,
                        MJPEG_L3_YIELD_INTERVAL);
#else
                const mjpeg_playback_clock_effect_t effect =
                    mjpeg_playback_clock_player_on_decode_result(
                        &clock_binding, result, current_time,
                        MJPEG_L3_YIELD_INTERVAL, false);
#endif
                if (ret == ESP_OK) {
                    if (effect.delay_one_tick) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                } else if (ret == ESP_ERR_NOT_FOUND) {
                    handle_emotion_playback_complete(player);
                } else {
                    ESP_LOGW(TAG, "⚠️ 解码错误 %" PRIu32 "/10: %s",
                            player->decode_error_count, esp_err_to_name(ret));
                    if (effect.enter_error_state) {
                        ESP_LOGE(TAG, "❌ 连续解码错误过多，进入错误状态");
                        change_state(player, EMOTION_VIDEO_STATE_ERROR);
                    }
                }
            } else if (clock_poll.action == MJPEG_CLOCK_ACTION_WAIT) {
                precise_delay_us(clock_poll.wait_us);
            } else if (clock_poll.action == MJPEG_CLOCK_ACTION_PAUSED) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        uint64_t now_us = esp_timer_get_time();
        if (now_us - last_diag_us >= 5000000ULL) {
            last_diag_us = now_us;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            const unsigned min_int =
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            const unsigned largest_int =
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            ESP_LOGI(TAG,
                     "MJPEG_DIAG core=%d state=%d frame_idx=%d cache_idx=%d err=%" PRIu32
                     " hwm=%u | free_int=%u min_int=%u largest_int=%u | free_psram=%u",
                     xPortGetCoreID(),
                     (int)player->state,
                     player->current_frame_index,
                     player->current_cache_index,
                     player->decode_error_count,
                     (unsigned)hwm,
                     (unsigned)free_int,
                     min_int,
                     largest_int,
                     (unsigned)free_psram);
        }
    }

    ESP_LOGI(TAG, "🎬 解码任务退出");
    vTaskDelete(NULL);
}

// 🚀 核心优化：使用帧索引表的高性能解码函数
static esp_err_t hw_decode_frame_optimized(emotion_video_player_t *player)
{
    if (!SAFE_SEMAPHORE_TAKE(player->cache_mutex, 20)) {
        return ESP_ERR_TIMEOUT;
    }
    
    if (player->current_cache_index < 0 || player->current_cache_index >= TOTAL_EMOTIONS_COUNT) {
        xSemaphoreGive(player->cache_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    video_cache_t *current_cache = &player->video_caches[player->current_cache_index];
    
    if (!current_cache->ready || !current_cache->buffer || current_cache->size == 0) {
        xSemaphoreGive(player->cache_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!current_cache->index_built) {
        xSemaphoreGive(player->cache_mutex);
        ESP_LOGE(TAG, "❌ 帧索引表未建立");
        return ESP_ERR_INVALID_STATE;
    }
    
    const mjpeg_frame_source_view_t frame_source = {
        .buffer = current_cache->buffer,
        .buffer_size = current_cache->size,
        .entries = current_cache->frame_index,
        .frame_count = (size_t)current_cache->frame_count,
        .ready = current_cache->ready,
        .index_built = current_cache->index_built,
    };
    size_t frame_cursor = (size_t)player->current_frame_index;
    mjpeg_frame_claim_t frame_claim = {0};
#if EMOTION_VIDEO_USE_FRAME_SOURCE
    const mjpeg_frame_source_result_t claim_result = mjpeg_frame_source_claim_next(
        &frame_source, &frame_cursor, &frame_claim);
#else
    const mjpeg_frame_source_result_t claim_result = mjpeg_frame_source_claim_next_legacy(
        &frame_source, &frame_cursor, &frame_claim);
#endif
    if (claim_result != MJPEG_FRAME_SOURCE_OK) {
        xSemaphoreGive(player->cache_mutex);
        if (claim_result == MJPEG_FRAME_SOURCE_INVALID_SIZE) {
            ESP_LOGE(TAG, "❌ 帧边界溢出");
            return ESP_ERR_INVALID_SIZE;
        }
        if (claim_result == MJPEG_FRAME_SOURCE_NOT_FOUND) {
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_ERR_INVALID_STATE;
    }

    player->current_frame_index = (int)frame_cursor;
    uint8_t *frame_data = (uint8_t *)frame_claim.data;
    size_t frame_size = frame_claim.size;
    
    xSemaphoreGive(player->cache_mutex);

#if EMOTION_VIDEO_USE_DECODER_STAGE
    mjpeg_decoder_player_adapter_t adapter = {
        .input_buffer = &player->input_buffer,
        .input_buffer_size = &player->input_buffer_size,
        .output_buffer = &player->output_buffer,
        .output_buffer_size = &player->output_buffer_size,
        .decoder_handle = &player->hw_dec_handle,
        .decoder_config = &player->hw_dec_cfg,
        .frame_info = &player->frame_info,
        .input_alignment = player->hw_caps.in_frame_align,
        .output_alignment = player->hw_caps.out_frame_align,
        .canvas_width = player->config.canvas_width,
        .canvas_height = player->config.canvas_height,
        .output_format = player->config.output_format,
        .pts = player->current_frame * (1000 / player->config.frame_rate),
        .public_handle = (emotion_video_handle_t)player,
        .frame_cb = player->frame_cb,
        .frame_user_data = player->frame_user_data,
    };
    return mjpeg_decoder_player_adapter_run(&adapter, frame_data, frame_size);
#else
    if (!player->input_buffer || frame_size > player->input_buffer_size) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t required_size = frame_size > 64*1024 ? frame_size : 64*1024;
        
        if (free_psram < required_size + 512*1024) {
            return ESP_ERR_NO_MEM;
        }
        
        if (player->input_buffer) {
            heap_caps_free(player->input_buffer);
        }
        
        player->input_buffer = heap_caps_aligned_alloc(
            player->hw_caps.in_frame_align > 64 ? player->hw_caps.in_frame_align : 64, 
            required_size, MALLOC_CAP_SPIRAM);
        if (!player->input_buffer) {
            if (player->output_buffer) {
                esp_video_codec_free(player->output_buffer);
                player->output_buffer = NULL;
                player->output_buffer_size = 0;
            }
            return ESP_ERR_NO_MEM;
        }
        player->input_buffer_size = required_size;
    }

    if (frame_size > player->input_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    uint64_t t_memcpy0 = esp_timer_get_time();
    memcpy(player->input_buffer, frame_data, frame_size);
    uint32_t memcpy_ms = (uint32_t)((esp_timer_get_time() - t_memcpy0) / 1000ULL);

    if (!player->output_buffer) {
        uint32_t needed_size = player->config.canvas_width * player->config.canvas_height * 2;
        uint32_t actual_size = 0;
        player->output_buffer = esp_video_codec_align_alloc(
            player->hw_caps.out_frame_align, needed_size, &actual_size);
        if (!player->output_buffer) {
            return ESP_ERR_NO_MEM;
        }
        player->output_buffer_size = actual_size;
    }

    if (!player->hw_dec_handle) {
        esp_vc_err_t vc_ret = esp_video_dec_open(&player->hw_dec_cfg, &player->hw_dec_handle);
        if (vc_ret != ESP_VC_ERR_OK) {
            return ESP_FAIL;
        }
    }

    esp_video_dec_in_frame_t in_frame = {
        .pts = player->current_frame * (1000 / player->config.frame_rate),
        .dts = player->current_frame * (1000 / player->config.frame_rate),
        .data = player->input_buffer,
        .size = frame_size,
        .consumed = 0
    };

    esp_video_dec_out_frame_t out_frame = {
        .data = player->output_buffer,
        .size = player->output_buffer_size,
        .decoded_size = 0
    };

    uint64_t t_hw0 = esp_timer_get_time();
    esp_vc_err_t vc_ret = esp_video_dec_process(player->hw_dec_handle, &in_frame, &out_frame);
    
    if (vc_ret == ESP_VC_ERR_BUF_NOT_ENOUGH) {
        esp_vc_err_t info_ret = esp_video_dec_get_frame_info(player->hw_dec_handle, &player->frame_info);
        if (info_ret != ESP_VC_ERR_OK) {
            return ESP_FAIL;
        }

        if (player->output_buffer) {
            esp_video_codec_free(player->output_buffer);
        }

        uint32_t needed_size = esp_video_codec_get_image_size(player->config.output_format, &player->frame_info.res);
        uint32_t actual_size = 0;
        player->output_buffer = esp_video_codec_align_alloc(
            player->hw_caps.out_frame_align, needed_size, &actual_size);
        if (!player->output_buffer) {
            return ESP_ERR_NO_MEM;
        }
        player->output_buffer_size = actual_size;

        out_frame.data = player->output_buffer;
        out_frame.size = player->output_buffer_size;
        
        vc_ret = esp_video_dec_process(player->hw_dec_handle, &in_frame, &out_frame);
    }

    if (vc_ret != ESP_VC_ERR_OK) {
        return ESP_FAIL;
    }

    if (out_frame.decoded_size > 0 &&
        (player->frame_info.res.width == 0 || player->frame_info.res.height == 0)) {
        esp_video_dec_get_frame_info(player->hw_dec_handle, &player->frame_info);
    }
    if (!mjpeg_present_sink_esp_present(
            (emotion_video_handle_t)player, out_frame.data,
            out_frame.decoded_size, player->frame_info.res.width,
            player->frame_info.res.height, memcpy_ms, t_hw0,
            player->frame_cb, player->frame_user_data)) {
        return ESP_FAIL;
    }

    return ESP_OK;
#endif
}

static esp_err_t init_hw_decoder(emotion_video_player_t *player)
{
    esp_video_codec_query_t query = {
        .codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG,
        .codec_cc = g_hw_decoder_cc
    };
    
    esp_vc_err_t ret = esp_video_dec_query_caps(&query, &player->hw_caps);
    if (ret != ESP_VC_ERR_OK) {
        ESP_LOGE(TAG, "❌ 查询硬件MJPEG解码器能力失败: %d", ret);
        return ESP_FAIL;
    }
    
    player->hw_dec_cfg.codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG;
    player->hw_dec_cfg.codec_cc = g_hw_decoder_cc;
    player->hw_dec_cfg.out_fmt = player->config.output_format;
    player->hw_dec_cfg.codec_spec_info = NULL;
    player->hw_dec_cfg.codec_spec_info_size = 0;
    
    player->input_buffer = NULL;
    player->input_buffer_size = 0;
    player->output_buffer = NULL;
    player->output_buffer_size = 0;

    ESP_LOGI(TAG, "✅ 硬件MJPEG解码器初始化成功");
    return ESP_OK;
}

static esp_err_t deinit_hw_decoder(emotion_video_player_t *player)
{
    if (player->hw_dec_handle) {
        esp_video_dec_close(player->hw_dec_handle);
        player->hw_dec_handle = NULL;
    }
    
    if (player->input_buffer) {
        heap_caps_free(player->input_buffer);
        player->input_buffer = NULL;
        player->input_buffer_size = 0;
    }
    
    if (player->output_buffer) {
        esp_video_codec_free(player->output_buffer);
        player->output_buffer = NULL;
        player->output_buffer_size = 0;
    }
    
    ESP_LOGI(TAG, "✅ 硬件解码器资源已释放");
    return ESP_OK;
}

// P2: load happy/sad/angry/loving/neutral into PSRAM+index (no decode/paint).
static void preload_base_emotions(emotion_video_player_t *player)
{
    uint64_t preload_begin_us = esp_timer_get_time();
    StackDiagSetPreloadActive(true);
    StackDiagLog("preload_task_begin", "before_any_emotion");
    ESP_LOGW(TAG, "CTRL P2 preload_base begin");
    ESP_LOGI(TAG, "🚀 开始预加载基础表情...");

    const char *preload_order[] = {"happy", "sad", "angry", "loving", "neutral"};
    int preload_count = sizeof(preload_order) / sizeof(preload_order[0]);

    for (int i = 0; i < preload_count; i++) {
        const emotion_def_t *def = find_emotion_def(preload_order[i]);
        if (!def) continue;

        int cache_index = def->cache_index;

        xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
        bool already_loaded = player->video_caches[cache_index].ready &&
                             player->video_caches[cache_index].index_built;
        xSemaphoreGive(player->cache_mutex);

        if (already_loaded) {
            ESP_LOGI(TAG, "⏭️ 跳过已加载: %s", preload_order[i]);
            StackDiagLog("preload_skip", preload_order[i]);
            continue;
        }

        size_t heap_int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t heap_psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint64_t load_begin_us = esp_timer_get_time();
        StackDiagLog("preload_before", preload_order[i]);
        ESP_LOGI(TAG, "📂 预加载表情: %s free_int=%u free_psram=%u",
                 preload_order[i], (unsigned)heap_int_before, (unsigned)heap_psram_before);

        esp_err_t ret = load_video_to_cache(player, cache_index);
        StackDiagLog("preload_after_load", preload_order[i]);
        if (ret == ESP_OK) {
            uint64_t index_begin_us = esp_timer_get_time();
            ret = build_frame_index(player, cache_index);
            uint64_t index_cost_ms = (esp_timer_get_time() - index_begin_us) / 1000ULL;
            StackDiagLog("preload_after_index", preload_order[i]);
            if (ret == ESP_OK) {
                size_t heap_int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t heap_psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                uint64_t total_cost_ms = (esp_timer_get_time() - load_begin_us) / 1000ULL;
                ESP_LOGI(TAG,
                         "✅ 预加载成功: %s total=%ums index=%ums free_int=%u->%u free_psram=%u->%u",
                         preload_order[i],
                         (unsigned)total_cost_ms,
                         (unsigned)index_cost_ms,
                         (unsigned)heap_int_before, (unsigned)heap_int_after,
                         (unsigned)heap_psram_before, (unsigned)heap_psram_after);
            } else {
                ESP_LOGW(TAG, "⚠️ 索引建立失败: %s", preload_order[i]);
            }
        } else {
            ESP_LOGW(TAG, "⚠️ 加载失败: %s", preload_order[i]);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    player->base_emotions_loaded = true;
    uint64_t preload_cost_ms = (esp_timer_get_time() - preload_begin_us) / 1000ULL;
    StackDiagLog("preload_task_done", "all_emotions");
    StackDiagSetPreloadActive(false);
    ESP_LOGW(TAG, "CTRL P2 preload_base done total=%ums", (unsigned)preload_cost_ms);
    ESP_LOGI(TAG, "🎉 基础表情预加载完成 total=%ums", (unsigned)preload_cost_ms);
}

static void preload_emotions_task(void *arg)
{
    emotion_video_player_t *player = (emotion_video_player_t *)arg;
    preload_base_emotions(player);
    vTaskDelete(NULL);
}

static esp_err_t seed_one_emotion_still(emotion_video_player_t *player, const char *name)
{
    const emotion_def_t *def = find_emotion_def(name);
    if (!def || def->cache_index < 0 || def->cache_index >= TOTAL_EMOTIONS_COUNT) {
        return ESP_ERR_NOT_FOUND;
    }
    const int idx = def->cache_index;
    if (player->seed_stills[idx].ready && player->seed_stills[idx].rgb) {
        return ESP_OK;
    }

    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    bool cache_ok = player->video_caches[idx].ready && player->video_caches[idx].index_built;
    xSemaphoreGive(player->cache_mutex);
    if (!cache_ok) {
        esp_err_t lr = load_video_to_cache(player, idx);
        if (lr == ESP_OK) {
            lr = build_frame_index(player, idx);
        }
        if (lr != ESP_OK) {
            ESP_LOGW(TAG, "CTRL S1 seed load fail name=%s err=%s", name, esp_err_to_name(lr));
            return lr;
        }
    }

    // Mid-clip still — frame0 is often near-white/idle pose (all emos looked identical).
    uint32_t skip = 0;
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    const int fc = player->video_caches[idx].frame_count;
    xSemaphoreGive(player->cache_mutex);
    if (fc > 4) {
        skip = (uint32_t)(fc / 3);
    }

    const uint8_t *rgb = NULL;
    uint32_t size = 0, w = 0, h = 0;
    esp_err_t dr = emotion_video_player_decode_one_rgb565(
        (emotion_video_handle_t)player, name, skip, &rgb, &size, &w, &h);
    if (dr != ESP_OK || rgb == NULL || size == 0 || w == 0 || h == 0) {
        ESP_LOGW(TAG, "CTRL S1 seed decode fail name=%s err=%s skip=%u", name,
                 esp_err_to_name(dr), (unsigned)skip);
        return (dr == ESP_OK) ? ESP_FAIL : dr;
    }

    uint8_t *copy = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = (uint8_t *)malloc(size);
    }
    if (!copy) {
        ESP_LOGW(TAG, "CTRL S1 seed oom name=%s size=%u", name, (unsigned)size);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, rgb, size);
    if (player->seed_stills[idx].rgb) {
        heap_caps_free(player->seed_stills[idx].rgb);
    }
    player->seed_stills[idx].rgb = copy;
    player->seed_stills[idx].size = size;
    player->seed_stills[idx].w = w;
    player->seed_stills[idx].h = h;
    player->seed_stills[idx].ready = true;
    // Quality fingerprint — corner/mid often 0xFFFF (white bg); count non-white.
    {
        const uint16_t *px = (const uint16_t *)copy;
        const uint32_t np = size / 2;
        uint32_t nw = 0, acc = 0;
        uint16_t face = 0;
        for (uint32_t i = 0; i < np; i += 64) {
            if (px[i] != 0xFFFFu) {
                nw++;
            }
            acc = (acc * 131u) + px[i];
        }
        if (w >= 241 && h >= 201) {
            face = px[(uint32_t)200 * w + 240];
        }
        ESP_LOGW(TAG, "CTRL S1 seed ok name=%s %ux%u bytes=%u nw=%u hash=%08x face=%04x",
                 name, (unsigned)w, (unsigned)h, (unsigned)size, (unsigned)nw,
                 (unsigned)acc, (unsigned)face);
    }
    return ESP_OK;
}

/** Sparse RGB565 fingerprint — same stride as CTRL S1 seed ok hash. */
static uint32_t rgb565_sparse_hash(const uint8_t *rgb, uint32_t size)
{
    if (!rgb || size < 2) {
        return 0;
    }
    const uint16_t *px = (const uint16_t *)rgb;
    const uint32_t np = size / 2;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < np; i += 64) {
        acc = (acc * 131u) + px[i];
    }
    return acc;
}

/**
 * s1ce / P1: pure probe — decode frame0 + last, log hashes. Does not change seed path.
 * Contract (rules): hash(f0[emo]) == hash(last[emo]) == hash(f0[standby]).
 */
static esp_err_t probe_emotion_contract_ends(emotion_video_player_t *player, const char *name,
                                             uint32_t *out_fc, uint32_t *out_h0, uint32_t *out_hlast)
{
    if (!player || !name || !out_fc || !out_h0 || !out_hlast) {
        return ESP_ERR_INVALID_ARG;
    }
    const emotion_def_t *def = find_emotion_def(name);
    if (!def || def->cache_index < 0 || def->cache_index >= TOTAL_EMOTIONS_COUNT) {
        return ESP_ERR_NOT_FOUND;
    }
    const int idx = def->cache_index;
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    const int fc = player->video_caches[idx].frame_count;
    const bool ready = player->video_caches[idx].ready && player->video_caches[idx].index_built;
    xSemaphoreGive(player->cache_mutex);
    if (!ready || fc <= 0) {
        ESP_LOGW(TAG, "FACE_ASSET emo=%s skip=no_cache s1ce", name);
        return ESP_ERR_INVALID_STATE;
    }
    *out_fc = (uint32_t)fc;
    const uint32_t last_i = (fc > 0) ? (uint32_t)(fc - 1) : 0;

    const uint8_t *rgb = NULL;
    uint32_t size = 0, w = 0, h = 0;
    esp_err_t r0 = emotion_video_player_decode_at_rgb565((emotion_video_handle_t)player, name, 0,
                                                        &rgb, &size, &w, &h);
    if (r0 != ESP_OK || rgb == NULL || size == 0) {
        ESP_LOGW(TAG, "FACE_ASSET emo=%s f0_fail err=%s s1ce", name, esp_err_to_name(r0));
        return (r0 == ESP_OK) ? ESP_FAIL : r0;
    }
    *out_h0 = rgb565_sparse_hash(rgb, size);
    vTaskDelay(pdMS_TO_TICKS(10));

    rgb = NULL;
    size = 0;
    esp_err_t rl = emotion_video_player_decode_at_rgb565((emotion_video_handle_t)player, name, last_i,
                                                        &rgb, &size, &w, &h);
    if (rl != ESP_OK || rgb == NULL || size == 0) {
        ESP_LOGW(TAG, "FACE_ASSET emo=%s last_fail err=%s idx=%u s1ce", name, esp_err_to_name(rl),
                 (unsigned)last_i);
        return (rl == ESP_OK) ? ESP_FAIL : rl;
    }
    *out_hlast = rgb565_sparse_hash(rgb, size);

    const int same_ends = (*out_h0 == *out_hlast) ? 1 : 0;
    ESP_LOGW(TAG, "FACE_ASSET emo=%s fc=%u f0=%08x last=%08x same_ends=%d s1ce", name,
             (unsigned)*out_fc, (unsigned)*out_h0, (unsigned)*out_hlast, same_ends);
    return ESP_OK;
}

static void seed_base_emotion_stills(emotion_video_player_t *player)
{
    static const char *kSeedOrder[] = {"standby", "happy", "sad", "angry", "loving", "neutral"};
    ESP_LOGW(TAG, "CTRL S1 seed_stills begin");
    int ok_n = 0;
    uint32_t standby_f0 = 0;
    int standby_f0_ok = 0;
    int contract_n = 0;
    int probed_n = 0;
    for (size_t i = 0; i < sizeof(kSeedOrder) / sizeof(kSeedOrder[0]); i++) {
        if (seed_one_emotion_still(player, kSeedOrder[i]) == ESP_OK) {
            ok_n++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        // P1 probe after each seed (cache warm); pure logs.
        uint32_t fc = 0, h0 = 0, hlast = 0;
        if (probe_emotion_contract_ends(player, kSeedOrder[i], &fc, &h0, &hlast) == ESP_OK) {
            probed_n++;
            if (strcmp(kSeedOrder[i], "standby") == 0) {
                standby_f0 = h0;
                standby_f0_ok = 1;
            }
            const int same_ends = (h0 == hlast) ? 1 : 0;
            const int vs_standby = (standby_f0_ok && h0 == standby_f0) ? 1 : 0;
            ESP_LOGW(TAG, "FACE_ASSET emo=%s vs_standby_f0=%d s1ce", kSeedOrder[i], vs_standby);
            if (same_ends && vs_standby) {
                contract_n++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    player->seed_stills_ready = (ok_n > 0);
    ESP_LOGW(TAG, "CTRL S1 seed_stills done ok=%d/%d ready=%d", ok_n,
             (int)(sizeof(kSeedOrder) / sizeof(kSeedOrder[0])),
             player->seed_stills_ready ? 1 : 0);

    // contract=1 only if every probed emo has f0==last==standby.f0 (6/6).
    const int n_emos = (int)(sizeof(kSeedOrder) / sizeof(kSeedOrder[0]));
    const int contract = (standby_f0_ok && probed_n == n_emos && contract_n == n_emos) ? 1 : 0;
    ESP_LOGW(TAG,
             "FACE_ASSET contract=%d matched=%d/%d probed=%d standby_f0=%08x s1ce", contract,
             contract_n, n_emos, probed_n, (unsigned)standby_f0);
    esp_rom_printf("!!FACE_ASSET contract=%d matched=%d/%d s1ce\n", contract, contract_n, n_emos);
}

esp_err_t emotion_video_player_seed_emotion_still(emotion_video_handle_t handle,
                                                  const char *emotion_name)
{
    if (!handle || !emotion_name) {
        return ESP_ERR_INVALID_ARG;
    }
    return seed_one_emotion_still((emotion_video_player_t *)handle, emotion_name);
}

esp_err_t emotion_video_player_preload_base_sync(emotion_video_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    // s1ct: seed standby BEFORE loading the other five MJPEGs so the host can paint
    // a face while the rest of P2 is still reading SD (avoids long black canvas).
    if (seed_one_emotion_still(player, "standby") == ESP_OK) {
        ESP_LOGW(TAG, "CTRL S1 seed_early emo=standby s1ct");
        esp_rom_printf("!!FACE_BOOT seed_early standby\n");
    }
    if (!player->base_emotions_loaded) {
        if (player->preload_started) {
            ESP_LOGW(TAG, "CTRL P2 preload_base wait async");
            for (int i = 0; i < 300 && !player->base_emotions_loaded; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!player->base_emotions_loaded) {
                return ESP_ERR_TIMEOUT;
            }
        } else {
            player->preload_started = true;
            preload_base_emotions(player);
        }
    } else {
        ESP_LOGW(TAG, "CTRL P2 preload_base skip already_loaded");
    }
    if (!player->seed_stills_ready) {
        seed_base_emotion_stills(player);
    } else {
        ESP_LOGW(TAG, "CTRL S1 seed_stills skip already_ready");
    }
    return player->seed_stills_ready ? ESP_OK : ESP_FAIL;
}

esp_err_t emotion_video_player_get_seed_rgb565(emotion_video_handle_t handle,
                                               const char *emotion_name,
                                               const uint8_t **out_rgb565,
                                               uint32_t *out_size,
                                               uint32_t *out_w,
                                               uint32_t *out_h)
{
    if (!handle || !emotion_name || !out_rgb565) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    const char *resolved = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved);
    if (!def || def->cache_index < 0 || def->cache_index >= TOTAL_EMOTIONS_COUNT) {
        return ESP_ERR_NOT_FOUND;
    }
    const int idx = def->cache_index;
    if (!player->seed_stills[idx].ready || !player->seed_stills[idx].rgb) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_rgb565 = player->seed_stills[idx].rgb;
    if (out_size) {
        *out_size = player->seed_stills[idx].size;
    }
    if (out_w) {
        *out_w = player->seed_stills[idx].w;
    }
    if (out_h) {
        *out_h = player->seed_stills[idx].h;
    }
    return ESP_OK;
}

bool emotion_video_player_seed_stills_ready(emotion_video_handle_t handle)
{
    if (!handle) {
        return false;
    }
    return ((emotion_video_player_t *)handle)->seed_stills_ready;
}

uint32_t emotion_video_player_mid_stride_skip(emotion_video_handle_t handle,
                                             const char *emotion_name)
{
    if (!handle || !emotion_name) {
        return 0;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    const char *resolved = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved);
    if (!def || def->cache_index < 0 || def->cache_index >= TOTAL_EMOTIONS_COUNT) {
        return 0;
    }
    const int idx = def->cache_index;
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    const int fc = player->video_caches[idx].frame_count;
    xSemaphoreGive(player->cache_mutex);
    if (fc > 4) {
        return (uint32_t)(fc / 3);
    }
    return 0;
}

uint32_t emotion_video_player_mid_arc_start(emotion_video_handle_t handle,
                                            const char *emotion_name)
{
    // s1cd: do NOT open MID at frame0 — SD clip head is often blink/closed-eye (wake looked
    // eyes-shut). Start at seed index (fc/3) = open-eye rest, same as bookend.
    return emotion_video_player_mid_stride_skip(handle, emotion_name);
}

uint32_t emotion_video_player_mid_arc_step(emotion_video_handle_t handle,
                                          const char *emotion_name,
                                          uint32_t mid_frames)
{
    if (mid_frames == 0) {
        mid_frames = 1;
    }
    if (!handle || !emotion_name) {
        return 1;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    const char *resolved = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved);
    if (!def || def->cache_index < 0 || def->cache_index >= TOTAL_EMOTIONS_COUNT) {
        return 1;
    }
    const int idx = def->cache_index;
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    const int fc = player->video_caches[idx].frame_count;
    xSemaphoreGive(player->cache_mutex);
    // s1cd: span from seed start (fc/3) to end so arc still covers the expressive tail.
    const uint32_t start = (fc > 4) ? (uint32_t)(fc / 3) : 0;
    const uint32_t remain = (fc > (int)start) ? (uint32_t)fc - start : (uint32_t)fc;
    const uint32_t step = remain / mid_frames;
    return step > 0 ? step : 1;
}

esp_err_t emotion_video_player_pause_loop(emotion_video_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    player->decode_paused = true;
    return ESP_OK;
}

bool emotion_video_player_is_decode_paused(emotion_video_handle_t handle)
{
    if (!handle) {
        return false;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    return player->decode_paused;
}

esp_err_t emotion_video_player_decode_one_rgb565(emotion_video_handle_t handle,
                                                 const char *emotion_name,
                                                 uint32_t skip_frames,
                                                 const uint8_t **out_rgb565,
                                                 uint32_t *out_size,
                                                 uint32_t *out_w,
                                                 uint32_t *out_h)
{
    if (!handle || !emotion_name || !out_rgb565) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;

    // Do NOT hold state_mutex here: change_state()/switch_to_emotion() take it
    // (non-recursive) → FreeRTOS priority-disinherit assert / panic.
    esp_err_t ret = ESP_OK;
    if (strcmp(player->current_emotion, emotion_name) != 0) {
        ESP_LOGW(TAG, "CTRL BYPASS decode_one switch name=%s", emotion_name);
        ret = switch_to_emotion(player, emotion_name);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "CTRL BYPASS decode_one switch fail name=%s err=%s",
                     emotion_name, esp_err_to_name(ret));
            return ret;
        }
    }
    reset_current_emotion_position(player);

    emotion_video_frame_cb_t saved_cb = player->frame_cb;
    player->frame_cb = NULL;
    // Decode skip_frames+1 times; keep the last (often more expressive mid-clip).
    for (uint32_t i = 0; i <= skip_frames; i++) {
        ret = hw_decode_frame_optimized(player);
        if (ret != ESP_OK) {
            break;
        }
    }
    player->frame_cb = saved_cb;

    if (ret != ESP_OK || player->output_buffer == NULL) {
        ESP_LOGW(TAG, "CTRL BYPASS decode_one fail name=%s err=%s skip=%u",
                 emotion_name, esp_err_to_name(ret), (unsigned)skip_frames);
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    uint32_t w = player->frame_info.res.width;
    uint32_t h = player->frame_info.res.height;
    if (w == 0 || h == 0) {
        w = player->config.canvas_width;
        h = player->config.canvas_height;
    }
    *out_rgb565 = player->output_buffer;
    if (out_size) {
        *out_size = w * h * 2;
    }
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    ESP_LOGW(TAG, "CTRL BYPASS decode_one ok name=%s w=%u h=%u skip=%u", emotion_name,
             (unsigned)w, (unsigned)h, (unsigned)skip_frames);
    return ESP_OK;
}

esp_err_t emotion_video_player_decode_next_rgb565(emotion_video_handle_t handle,
                                                  const uint8_t **out_rgb565,
                                                  uint32_t *out_size,
                                                  uint32_t *out_w,
                                                  uint32_t *out_h)
{
    return emotion_video_player_decode_next_n_rgb565(handle, 1, out_rgb565, out_size, out_w,
                                                     out_h);
}

esp_err_t emotion_video_player_decode_next_n_rgb565(emotion_video_handle_t handle,
                                                    uint32_t n,
                                                    const uint8_t **out_rgb565,
                                                    uint32_t *out_size,
                                                    uint32_t *out_w,
                                                    uint32_t *out_h)
{
    if (!handle || !out_rgb565) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n == 0) {
        n = 1;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;

    emotion_video_frame_cb_t saved_cb = player->frame_cb;
    player->frame_cb = NULL;
    esp_err_t ret = ESP_FAIL;
    for (uint32_t i = 0; i < n; i++) {
        ret = hw_decode_frame_optimized(player);
        if (ret == ESP_ERR_NOT_FOUND) {
            // Loop clip for short bypass anim.
            reset_current_emotion_position(player);
            ret = hw_decode_frame_optimized(player);
        }
        if (ret != ESP_OK) {
            break;
        }
    }
    player->frame_cb = saved_cb;

    if (ret != ESP_OK || player->output_buffer == NULL) {
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    uint32_t w = player->frame_info.res.width;
    uint32_t h = player->frame_info.res.height;
    if (w == 0 || h == 0) {
        w = player->config.canvas_width;
        h = player->config.canvas_height;
    }
    *out_rgb565 = player->output_buffer;
    if (out_size) {
        *out_size = w * h * 2;
    }
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    return ESP_OK;
}

esp_err_t emotion_video_player_decode_at_rgb565(emotion_video_handle_t handle,
                                                const char *emotion_name,
                                                uint32_t frame_index,
                                                const uint8_t **out_rgb565,
                                                uint32_t *out_size,
                                                uint32_t *out_w,
                                                uint32_t *out_h)
{
    // s1by: index-table seek + one JPEG. Avoids decode_next_n(step) burning step
    // consecutive hw_decode calls (idle breathe step≈fc/6 ≈13 → WDT / InstrFault).
    if (!handle || !emotion_name || !out_rgb565) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;

    esp_err_t ret = ESP_OK;
    if (strcmp(player->current_emotion, emotion_name) != 0) {
        ESP_LOGW(TAG, "CTRL BYPASS decode_at switch name=%s", emotion_name);
        ret = switch_to_emotion(player, emotion_name);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (player->current_cache_index < 0 ||
        player->current_cache_index >= TOTAL_EMOTIONS_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    video_cache_t *cache = &player->video_caches[player->current_cache_index];
    if (!cache->ready || !cache->index_built || cache->frame_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t fc = (uint32_t)cache->frame_count;
    const uint32_t idx = frame_index % fc;
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    player->current_frame_index = (int)idx;
    xSemaphoreGive(player->cache_mutex);

    emotion_video_frame_cb_t saved_cb = player->frame_cb;
    player->frame_cb = NULL;
    ret = hw_decode_frame_optimized(player);
    player->frame_cb = saved_cb;

    if (ret != ESP_OK || player->output_buffer == NULL) {
        ESP_LOGW(TAG, "CTRL BYPASS decode_at fail name=%s err=%s idx=%u", emotion_name,
                 esp_err_to_name(ret), (unsigned)idx);
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    uint32_t w = player->frame_info.res.width;
    uint32_t h = player->frame_info.res.height;
    if (w == 0 || h == 0) {
        w = player->config.canvas_width;
        h = player->config.canvas_height;
    }
    *out_rgb565 = player->output_buffer;
    if (out_size) {
        *out_size = w * h * 2;
    }
    if (out_w) {
        *out_w = w;
    }
    if (out_h) {
        *out_h = h;
    }
    return ESP_OK;
}

esp_err_t emotion_video_player_resume_loop(emotion_video_handle_t handle)
{
    return emotion_video_player_resume_loop_at_fps(handle, 0);
}

esp_err_t emotion_video_player_resume_loop_at_fps(emotion_video_handle_t handle, uint32_t fps)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    const uint64_t now_us = esp_timer_get_time();
    const uint64_t kResumeWarmupUs = 300000ULL;      // 0.3s
    const uint64_t kResumeSoftStartUs = 2000000ULL;  // 2s
    const uint32_t kSoftStartMinIntervalUs = 200000; // 5fps ceiling during ramp

    if (fps > 0) {
        if (fps > 30) {
            fps = 30;
        }
        player->config.frame_rate = fps;
        player->normal_frame_interval_us = 1000000U / fps;
    } else if (player->normal_frame_interval_us == 0) {
        uint32_t base_fps = player->config.frame_rate ? player->config.frame_rate : 30;
        player->normal_frame_interval_us = 1000000U / base_fps;
    }

    player->last_resume_us = now_us;
    player->resume_warmup_until_us = now_us + kResumeWarmupUs;
    player->soft_start_until_us = now_us + kResumeSoftStartUs;
    player->frame_deadline_us = player->resume_warmup_until_us;
    player->last_decode_success_us = now_us;
    player->last_stall_diag_us = 0;

    uint32_t start_interval = player->normal_frame_interval_us;
    if (start_interval < kSoftStartMinIntervalUs) {
        start_interval = kSoftStartMinIntervalUs;
    }
    player->frame_interval_us = start_interval;
    player->decode_paused = false;
    ESP_LOGI(TAG,
             "MJPEG resume soft-start: warmup=%ums soft_start=%ums target_fps=%u interval=%luus",
             (unsigned)(kResumeWarmupUs / 1000ULL),
             (unsigned)(kResumeSoftStartUs / 1000ULL),
             (unsigned)(player->normal_frame_interval_us
                            ? (1000000U / player->normal_frame_interval_us)
                            : 0),
             (unsigned long)player->frame_interval_us);
    return ESP_OK;
}

esp_err_t emotion_video_player_start_deferred_preload(emotion_video_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    if (player->preload_started) {
        return ESP_OK;
    }
    player->preload_started = true;
    BaseType_t ret_preload = xTaskCreate(
        preload_emotions_task,
        "emotion_preload",
        4096,
        player,
        1,
        NULL
    );
    if (ret_preload != pdPASS) {
        player->preload_started = false;
        ESP_LOGW(TAG, "⚠️ 预加载任务启动失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "✅ 预加载任务已启动（唤醒稳定后）");
    return ESP_OK;
}

static void async_load_task(void *arg)
{
    async_load_params_t *params = (async_load_params_t *)arg;
    emotion_video_player_t *player = params->player;
    emotion_video_event_cb_t event_cb = params->event_cb;
    void *user_data = params->user_data;
    
    ESP_LOGI(TAG, "🎬 开始异步加载剩余基础表情...");
    
    if (event_cb) {
        event_cb(EMOTION_VIDEO_EVENT_LOADING_START, user_data);
    }
    
    int loaded_count = 0;
    int failed_count = 0;
    int skipped_count = 0;
    
    for (int i = 0; i < BASE_EMOTIONS_COUNT; i++) {
        const emotion_def_t *def = &g_emotion_defs[i];
        
        if (def->type == EMOTION_TYPE_BASE) {
            xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
            bool already_loaded = player->video_caches[i].ready && player->video_caches[i].buffer;
            xSemaphoreGive(player->cache_mutex);
            
            if (already_loaded) {
                ESP_LOGI(TAG, "⏭️ 跳过已加载表情: %s", def->name);
                skipped_count++;
                continue;
            }
            
            ESP_LOGI(TAG, "📂 异步加载表情: %s (%s)", def->name, def->file);
            
            esp_err_t ret = load_video_to_cache(player, i);
            if (ret == ESP_OK) {
                ret = build_frame_index(player, i);
                if (ret == ESP_OK) {
                    loaded_count++;
                    ESP_LOGI(TAG, "✅ 表情异步加载成功: %s", def->name);
                } else {
                    failed_count++;
                    ESP_LOGW(TAG, "⚠️ 帧索引表建立失败: %s", def->name);
                }
            } else {
                failed_count++;
                ESP_LOGW(TAG, "⚠️ 表情异步加载失败: %s (%s)", def->name, esp_err_to_name(ret));
            }
            
            taskYIELD();
        }
    }
    
    if (loaded_count > 0 || skipped_count == BASE_EMOTIONS_COUNT) {
        player->base_emotions_loaded = true;
    }
    
    ESP_LOGI(TAG, "🎉 基础表情异步加载完成: 成功 %d 个，失败 %d 个，跳过 %d 个", 
             loaded_count, failed_count, skipped_count);
    
    if (event_cb) {
        event_cb(EMOTION_VIDEO_EVENT_LOADING_END, user_data);
    }
    
    free(params);
    vTaskDelete(NULL);
}

static esp_err_t load_video_to_cache(emotion_video_player_t *player, int cache_index)
{
    if (cache_index < 0 || cache_index >= TOTAL_EMOTIONS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    video_cache_t *cache = &player->video_caches[cache_index];
    const emotion_def_t *def = &g_emotion_defs[cache_index];
    
    if (cache->ready && cache->buffer && def->type == EMOTION_TYPE_SPECIAL) {
        ESP_LOGI(TAG, "🔄 释放旧缓存并重新加载: %s", cache->file_name);
        heap_caps_free(cache->buffer);
        cache->buffer = NULL;
        cache->ready = false;
        cache->size = 0;
        cache->index_built = false;
    } else if (cache->ready && cache->buffer && def->type == EMOTION_TYPE_BASE) {
        ESP_LOGD(TAG, "✅ 基础表情缓存已就绪，跳过加载: %s", cache->file_name);
        return ESP_OK;
    }
    
    if (cache->ready && !cache->buffer) {
        ESP_LOGW(TAG, "⚠️ 检测到缓存状态异常，强制重置");
        cache->ready = false;
        cache->size = 0;
        cache->index_built = false;
    }
    
    char file_path[128];
    snprintf(file_path, sizeof(file_path), "%s/mjpeg/%s", MOUNT_POINT, cache->file_name);
    
    struct stat mount_stat;
    if (stat(MOUNT_POINT, &mount_stat) != 0) {
        ESP_LOGE(TAG, "❌ SD卡挂载点不存在");
        return ESP_ERR_NOT_FOUND;
    }
    
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        ESP_LOGE(TAG, "❌ 文件不存在: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "❌ 无法打开文件: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    if (free_psram < file_size + 512*1024) {
        ESP_LOGE(TAG, "❌ PSRAM内存不足");
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    
    cache->buffer = heap_caps_aligned_alloc(64, file_size, MALLOC_CAP_SPIRAM);
    if (!cache->buffer) {
        ESP_LOGE(TAG, "❌ PSRAM分配缓存失败");
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = 0;
    const size_t read_chunk_size = 16 * 1024;
    uint8_t *sd_read_buffer = heap_caps_aligned_alloc(
        64, read_chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (sd_read_buffer != NULL) {
        while (bytes_read < file_size) {
            const size_t remaining = file_size - bytes_read;
            const size_t chunk_size = remaining < read_chunk_size ? remaining : read_chunk_size;
            const size_t chunk_read = fread(sd_read_buffer, 1, chunk_size, file);
            if (chunk_read > 0) {
                memcpy(cache->buffer + bytes_read, sd_read_buffer, chunk_read);
                bytes_read += chunk_read;
            }
            if (chunk_read != chunk_size) {
                break;
            }
            // Bound each PSRAM write burst and let audio/network control work run.
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        heap_caps_free(sd_read_buffer);
        ESP_LOGI(TAG, "SD_STAGE_READ staged=1 chunk=%u file=%s bytes=%u/%u",
                 (unsigned)read_chunk_size, cache->file_name, (unsigned)bytes_read,
                 (unsigned)file_size);
    } else {
        // Allocation pressure must not turn a valid legacy SD pack into a boot
        // failure.  The previous direct path remains the bounded fallback.
        ESP_LOGW(TAG, "SD_STAGE_READ staged=0 fallback=direct file=%s", cache->file_name);
        bytes_read = fread(cache->buffer, 1, file_size, file);
    }
    fclose(file);

    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "❌ 文件读取不完整");
        heap_caps_free(cache->buffer);
        cache->buffer = NULL;
        return ESP_FAIL;
    }

    cache->size = bytes_read;
    cache->ready = true;
    
    ESP_LOGI(TAG, "✅ 表情加载成功: %s (%lu bytes)", cache->file_name, (unsigned long)bytes_read);
    
    return ESP_OK;
}

static void index_reader_yield(void *context)
{
    (void)context;
    taskYIELD();
}

// 🚀 关键新增函数：建立帧索引表
static esp_err_t build_frame_index(emotion_video_player_t *player, int cache_index)
{
    if (cache_index < 0 || cache_index >= TOTAL_EMOTIONS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    video_cache_t *cache = &player->video_caches[cache_index];
    
    if (!cache->ready || !cache->buffer) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (cache->index_built) {
        ESP_LOGD(TAG, "⏭️ 帧索引表已存在: %s", cache->file_name);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "🔨 开始建立帧索引表: %s", cache->file_name);
    
    size_t frame_count = 0;
#if EMOTION_VIDEO_USE_INDEX_READER
    const mjpeg_index_reader_result_t index_result = mjpeg_index_reader_build(
#else
    const mjpeg_index_reader_result_t index_result = mjpeg_index_reader_build_legacy(
#endif
        cache->buffer,
        cache->size,
        cache->frame_index,
        MAX_FRAMES_PER_VIDEO,
        &frame_count,
        index_reader_yield,
        NULL);
    if (index_result != MJPEG_INDEX_READER_OK) {
        return ESP_FAIL;
    }
    cache->frame_count = (int)frame_count;
    cache->index_built = true;
    
    ESP_LOGI(TAG, "✅ 帧索引表建立完成: %s (共 %d 帧) g4=%d",
             cache->file_name, cache->frame_count, EMOTION_VIDEO_USE_INDEX_READER);
    
    return ESP_OK;
}

static esp_err_t validate_mjpeg_frame(const uint8_t *frame_data, size_t frame_size)
{
    if (!frame_data || frame_size < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (frame_data[0] != 0xFF || frame_data[1] != 0xD8) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (frame_data[frame_size-2] != 0xFF || frame_data[frame_size-1] != 0xD9) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

static void change_state(emotion_video_player_t *player, emotion_video_state_t new_state)
{
    xSemaphoreTake(player->state_mutex, portMAX_DELAY);
    emotion_video_state_t old_state = player->state;
    player->state = new_state;
    xSemaphoreGive(player->state_mutex);
    
    if (old_state != new_state) {
        ESP_LOGI(TAG, "📊 状态变化: %d -> %d", old_state, new_state);
    }
}

static void notify_event(emotion_video_player_t *player, uint32_t event_bits)
{
    if (player->event_group) {
        xEventGroupSetBits(player->event_group, event_bits);
    }
}

static const emotion_def_t* find_emotion_def(const char *emotion_name)
{
    if (!emotion_name) return NULL;
    
    for (int i = 0; i < TOTAL_EMOTIONS_COUNT; i++) {
        if (strcasecmp(emotion_name, g_emotion_defs[i].name) == 0) {
            return &g_emotion_defs[i];
        }
    }
    
    return NULL;
}

static const char* resolve_emotion_alias(const char *emotion_name)
{
    if (!emotion_name) return NULL;
    
    for (size_t i = 0; i < ALIAS_COUNT; i++) {
        if (strcasecmp(emotion_name, g_emotion_aliases[i].alias) == 0) {
            return g_emotion_aliases[i].target;
        }
    }
    
    return emotion_name;
}

const char* emotion_video_player_canonicalize_emotion(const char *emotion_name)
{
    if (!emotion_name || emotion_name[0] == '\0') return "neutral";
    if (!FaceRouteV2_CanonicalEmotionEnabled()) return emotion_name;
    return resolve_emotion_alias(emotion_name);
}

// 🔥 改进版：支持切换时设置循环播放标志，添加性能监控
static esp_err_t switch_to_emotion(emotion_video_player_t *player, const char *emotion_name)
{
    ESP_LOGI(TAG, "🔄 开始切换表情: %s", emotion_name);
    uint64_t switch_start_time = esp_timer_get_time();
    
    const emotion_def_t *def = find_emotion_def(emotion_name);
    if (!def) {
        ESP_LOGW(TAG, "⚠️ 未找到表情定义: %s，切换到neutral", emotion_name);
        def = find_emotion_def("neutral");
        if (!def) {
            change_state(player, EMOTION_VIDEO_STATE_PLAYING);
            return ESP_FAIL;
        }
    }
    
    int cache_index = def->cache_index;
    
    // 🔥 通知切换开始事件
    if (player->event_cb) {
        player->event_cb(EMOTION_VIDEO_EVENT_SWITCHING, player->event_user_data);
    }
    
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    video_cache_t *cache = &player->video_caches[cache_index];
    bool cache_ready = cache->ready;
    bool index_built = cache->index_built;
    xSemaphoreGive(player->cache_mutex);
    
    if (!cache_ready) {
        if (player->decode_paused) {
            // Diag-only safety: do not SD-load during pause.
            ESP_LOGW(TAG,
                     "CTRL EMO_SWITCH cache_miss name=%s idx=%d paused=1 skip_sd=1",
                     emotion_name, cache_index);
            change_state(player, EMOTION_VIDEO_STATE_PLAYING);
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGI(TAG, "🔄 按需加载表情: %s", emotion_name);
        esp_err_t ret = load_video_to_cache(player, cache_index);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 表情加载失败: %s", emotion_name);
            change_state(player, EMOTION_VIDEO_STATE_PLAYING);
            return ESP_FAIL;
        }
    }
    
    // 🚀 关键：确保帧索引表已建立
    if (!index_built) {
        if (player->decode_paused) {
            ESP_LOGW(TAG,
                     "CTRL EMO_SWITCH index_miss name=%s idx=%d paused=1 skip_build=1",
                     emotion_name, cache_index);
            change_state(player, EMOTION_VIDEO_STATE_PLAYING);
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGI(TAG, "🔨 按需建立帧索引表: %s", emotion_name);
        esp_err_t ret = build_frame_index(player, cache_index);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ 帧索引表建立失败");
            change_state(player, EMOTION_VIDEO_STATE_PLAYING);
            return ESP_FAIL;
        }
    }
    
    // 🔥 快速切换（最小化锁持有时间）
    xSemaphoreTake(player->cache_mutex, portMAX_DELAY);
    player->current_cache_index = cache_index;
    player->current_frame_index = 0;
    xSemaphoreGive(player->cache_mutex);
    
    strncpy(player->current_emotion, def->name, sizeof(player->current_emotion) - 1);
    player->current_emotion[sizeof(player->current_emotion) - 1] = '\0';
    player->current_frame = 0;
    player->last_frame_time_us = esp_timer_get_time();
    player->frame_deadline_us = esp_timer_get_time();
    
    // 🔥 重置打断标志，新表情默认可被打断
    xSemaphoreTake(player->state_mutex, portMAX_DELAY);
    player->allow_interrupt = true;
    xSemaphoreGive(player->state_mutex);
    
    // 🔥 立即恢复 PLAYING 状态
    change_state(player, EMOTION_VIDEO_STATE_PLAYING);
    
    uint64_t switch_time = (esp_timer_get_time() - switch_start_time) / 1000;
    ESP_LOGI(TAG, "✅ 切换完成: %s (耗时: %u ms)", def->name, (unsigned)switch_time);

    return ESP_OK;
}

// ==================== 队列管理函数实现 ====================

static esp_err_t queue_add_emotion_internal(emotion_video_player_t *player, const char *emotion_name)
{
    if (!player || !emotion_name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(player->queue_mutex, portMAX_DELAY);
    
    if (queue_is_full(player)) {
        xSemaphoreGive(player->queue_mutex);
        ESP_LOGW(TAG, "⚠️ 表情队列已满，无法添加: %s", emotion_name);
        return ESP_ERR_NO_MEM;
    }
    
    emotion_queue_item_t *item = &player->emotion_queue[player->queue_tail];
    strncpy(item->emotion_name, emotion_name, sizeof(item->emotion_name) - 1);
    item->emotion_name[sizeof(item->emotion_name) - 1] = '\0';
    item->queue_id = player->next_queue_id++;
    item->queue_time_ms = esp_timer_get_time() / 1000ULL;
    
    player->queue_tail = (player->queue_tail + 1) % EMOTION_QUEUE_MAX_SIZE;
    player->queue_size++;
    
    xSemaphoreGive(player->queue_mutex);
    return ESP_OK;
}

static esp_err_t queue_get_next_emotion(emotion_video_player_t *player, char *emotion_name, size_t name_size)
{
    if (!player || !emotion_name || name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(player->queue_mutex, portMAX_DELAY);
    
    if (queue_is_empty(player)) {
        xSemaphoreGive(player->queue_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    emotion_queue_item_t *item = &player->emotion_queue[player->queue_head];
    strncpy(emotion_name, item->emotion_name, name_size - 1);
    emotion_name[name_size - 1] = '\0';
    
    player->queue_head = (player->queue_head + 1) % EMOTION_QUEUE_MAX_SIZE;
    player->queue_size--;
    
    xSemaphoreGive(player->queue_mutex);
    return ESP_OK;
}

static void queue_clear_internal(emotion_video_player_t *player)
{
    if (!player) return;
    
    xSemaphoreTake(player->queue_mutex, portMAX_DELAY);
    
    player->queue_head = 0;
    player->queue_tail = 0;
    player->queue_size = 0;
    memset(player->emotion_queue, 0, sizeof(player->emotion_queue));
    
    xSemaphoreGive(player->queue_mutex);
    
    ESP_LOGD(TAG, "📭 表情队列已清空");
}

static bool queue_is_empty(emotion_video_player_t *player)
{
    if (!player) return true;
    return player->queue_size == 0;
}

static bool queue_is_full(emotion_video_player_t *player)
{
    if (!player) return true;
    return player->queue_size >= EMOTION_QUEUE_MAX_SIZE;
}

static bool queue_contains_emotion(emotion_video_player_t *player, const char *emotion_name)
{
    if (!player || !emotion_name) return false;
    
    xSemaphoreTake(player->queue_mutex, portMAX_DELAY);
    
    bool found = false;
    int head = player->queue_head;
    for (int i = 0; i < player->queue_size; i++) {
        int index = (head + i) % EMOTION_QUEUE_MAX_SIZE;
        if (strcmp(player->emotion_queue[index].emotion_name, emotion_name) == 0) {
            found = true;
            break;
        }
    }
    
    xSemaphoreGive(player->queue_mutex);
    return found;
}

// ==================== 队列管理公共函数 ====================

esp_err_t emotion_video_player_queue_emotion(emotion_video_handle_t handle, const char *emotion_name)
{
    if (!handle || !emotion_name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved_name);
    
    if (!def) {
        ESP_LOGW(TAG, "⚠️ 未找到表情定义: %s", resolved_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (def->type != EMOTION_TYPE_BASE) {
        ESP_LOGW(TAG, "⚠️ 不支持特殊表情的队列管理: %s", resolved_name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (queue_contains_emotion(player, resolved_name)) {
        ESP_LOGI(TAG, "📺 表情已在队列中，跳过重复添加: %s", resolved_name);
        return ESP_OK;
    }
    
    return queue_add_emotion_internal(player, resolved_name);
}

int emotion_video_player_get_queue_size(emotion_video_handle_t handle)
{
    if (!handle) {
        return -1;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    xSemaphoreTake(player->queue_mutex, portMAX_DELAY);
    int size = player->queue_size;
    xSemaphoreGive(player->queue_mutex);
    
    return size;
}

esp_err_t emotion_video_player_clear_queue(emotion_video_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    queue_clear_internal(player);
    
    return ESP_OK;
}

int emotion_video_player_get_queue_items(emotion_video_handle_t handle, 
                                        emotion_queue_item_t *queue_items, int max_items)
{
    if (!handle || !queue_items || max_items <= 0) {
        return -1;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    xSemaphoreTake(player->queue_mutex, portMAX_DELAY);
    
    int items_to_copy = (player->queue_size < max_items) ? player->queue_size : max_items;
    int head = player->queue_head;
    
    for (int i = 0; i < items_to_copy; i++) {
        queue_items[i] = player->emotion_queue[head];
        head = (head + 1) % EMOTION_QUEUE_MAX_SIZE;
    }
    
    xSemaphoreGive(player->queue_mutex);
    
    return items_to_copy;
}

bool emotion_video_player_is_emotion_in_queue(emotion_video_handle_t handle, const char *emotion_name)
{
    if (!handle || !emotion_name) {
        return false;
    }
    
    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    
    return queue_contains_emotion(player, resolved_name);
}

esp_err_t emotion_video_player_force_switch_emotion(emotion_video_handle_t handle, const char* emotion_name)
{
    if (!handle || !emotion_name) {
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_player_t *player = (emotion_video_player_t *)handle;
    
    const char *resolved_name = resolve_emotion_alias(emotion_name);
    const emotion_def_t *def = find_emotion_def(resolved_name);
    
    if (!def) {
        ESP_LOGW(TAG, "⚠️ 未找到表情定义: %s", resolved_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    const emotion_def_t *current_def = find_emotion_def(player->current_emotion);
    
    if (current_def && current_def->type == EMOTION_TYPE_SPECIAL) {
        queue_clear_internal(player);
        change_state(player, EMOTION_VIDEO_STATE_IDLE);
        
        xSemaphoreTake(player->state_mutex, portMAX_DELAY);
        strncpy(player->requested_emotion, resolved_name, sizeof(player->requested_emotion) - 1);
        player->requested_emotion[sizeof(player->requested_emotion) - 1] = '\0';
        player->switch_requested = true;
        xSemaphoreGive(player->state_mutex);
        
        taskYIELD();
        notify_event(player, PLAYER_EVENT_SWITCH);
        
        ESP_LOGI(TAG, "🔄 强制切换表情: %s -> %s", player->current_emotion, resolved_name);
        return ESP_OK;
    } else {
        return emotion_video_player_play_emotion(handle, emotion_name);
    }
}
