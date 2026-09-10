#include "face_speech_core_bank.h"

#if S1GL_Y_SPEECH_CORE_BANK
#include "emotion_video_player.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>

namespace {
#define S1GL_IROM_LITERAL __attribute__((section(".literal.s1gl"), used, aligned(4)))

constexpr size_t kEmotionCount = 6;
constexpr size_t kLevelCount = 4;
constexpr size_t kManifestMax = 64 * 1024;
constexpr size_t kPsramReserve = 4 * 1024 * 1024;
const char kRoot[] S1GL_IROM_LITERAL = "/sdcard/dialogue_v2";
const char kReadBinary[] S1GL_IROM_LITERAL = "rb";
const char kDotDot[] S1GL_IROM_LITERAL = "..";
const char kManifestFormat[] S1GL_IROM_LITERAL = "%s/%s/manifest.json";
const char kAssetFormat[] S1GL_IROM_LITERAL = "%s/%s/%s";
const char kRender[] S1GL_IROM_LITERAL = "render";
const char kMode[] S1GL_IROM_LITERAL = "mode";
const char kLayered[] S1GL_IROM_LITERAL = "layered";
const char kMouthRoi[] S1GL_IROM_LITERAL = "mouth_roi";
const char kX[] S1GL_IROM_LITERAL = "x";
const char kY[] S1GL_IROM_LITERAL = "y";
const char kWidth[] S1GL_IROM_LITERAL = "width";
const char kHeight[] S1GL_IROM_LITERAL = "height";
const char kHoldBase[] S1GL_IROM_LITERAL = "hold_base";
const char kRgb565[] S1GL_IROM_LITERAL = "rgb565";
const char kMouthLevels[] S1GL_IROM_LITERAL = "mouth_levels";
const char kMouthComposite[] S1GL_IROM_LITERAL = "mouth_composite";
const char kMethod[] S1GL_IROM_LITERAL = "method";
const char kCompositeV1[] S1GL_IROM_LITERAL = "canonical_base_feather_v1";
const char kCompositeV2[] S1GL_IROM_LITERAL = "canonical_base_activity_feather_v2";
const char kPreloadEmotion[] S1GL_IROM_LITERAL = "!!FACE_S1GL preload=0 emo=%s\n";
const char kPreloadReserve[] S1GL_IROM_LITERAL = "!!FACE_S1GL preload=0 reserve=%u\n";
const char kPreloadOk[] S1GL_IROM_LITERAL =
    "!!FACE_S1GL preload=1 n=%u bytes=%u free=%u\n";
const char kEmotions[kEmotionCount][8] S1GL_IROM_LITERAL = {
    "standby", "happy", "sad", "angry", "loving", "neutral"};
const char kLevels[kLevelCount][7] S1GL_IROM_LITERAL = {
    "closed", "small", "medium", "large"};

struct Asset {
    uint8_t* rgb = nullptr;
    uint16_t w = 0;
    uint16_t h = 0;
};
struct Entry {
    const char* emotion = nullptr;
    Asset base;
    Asset mouth[kLevelCount];
    int x = 0, y = 0, w = 0, h = 0;
    bool precomposited = false;
    size_t bytes = 0;
};

std::atomic<bool> g_enabled{S1GL_Y_SPEECH_CORE_BANK != 0};
std::atomic<bool> g_ready{false};
std::atomic<int> g_active{-1};
Entry g_bank[kEmotionCount];
size_t g_total = 0;

bool SafePath(const char* path) {
    return path && path[0] && path[0] != '/' && path[0] != '\\' &&
           !strchr(path, ':') && !strstr(path, kDotDot);
}
void FreeAsset(Asset* asset) {
    if (asset->rgb) heap_caps_free(asset->rgb);
    *asset = {};
}
void FreeEntry(Entry* entry) {
    FreeAsset(&entry->base);
    for (auto& mouth : entry->mouth) FreeAsset(&mouth);
    *entry = {};
}
void FreeEntries(Entry entries[kEmotionCount]) {
    for (size_t i = 0; i < kEmotionCount; ++i) FreeEntry(&entries[i]);
}

bool ReadAsset(const char* path, size_t bytes, uint16_t w, uint16_t h, Asset* out) {
    FILE* file = fopen(path, kReadBinary);
    if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? ftell(file) : -1;
    ok = ok && size > 0 && static_cast<size_t>(size) == bytes &&
         fseek(file, 0, SEEK_SET) == 0;
    uint8_t* data = nullptr;
    if (ok) {
        data = static_cast<uint8_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ok = data && (reinterpret_cast<uintptr_t>(data) & 3U) == 0;
    }
    if (ok) ok = fread(data, 1, bytes, file) == bytes && fgetc(file) == EOF;
    fclose(file);
    if (!ok) {
        if (data) heap_caps_free(data);
        return false;
    }
    out->rgb = data;
    out->w = w;
    out->h = h;
    return true;
}

char* ReadManifest(const char* path, size_t* out_size) {
    FILE* file = fopen(path, kReadBinary);
    if (!file) return nullptr;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? ftell(file) : -1;
    ok = ok && size > 0 && static_cast<size_t>(size) <= kManifestMax &&
         fseek(file, 0, SEEK_SET) == 0;
    char* json = nullptr;
    if (ok) {
        json = static_cast<char*>(heap_caps_malloc(
            static_cast<size_t>(size) + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        ok = json != nullptr;
    }
    if (ok) ok = fread(json, 1, static_cast<size_t>(size), file) ==
                 static_cast<size_t>(size);
    fclose(file);
    if (!ok) {
        if (json) heap_caps_free(json);
        return nullptr;
    }
    json[size] = 0;
    *out_size = static_cast<size_t>(size);
    return json;
}

bool JsonInt(cJSON* object, const char* key, int* out) {
    cJSON* item = object ? cJSON_GetObjectItem(object, key) : nullptr;
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valueint;
    return true;
}

bool LoadEntry(const char* emotion, Entry* out) {
    char manifest_path[160];
    snprintf(manifest_path, sizeof(manifest_path), kManifestFormat, kRoot, emotion);
    size_t manifest_size = 0;
    char* json = ReadManifest(manifest_path, &manifest_size);
    if (!json) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(json, manifest_size);
    heap_caps_free(json);
    if (!root) {
        return false;
    }

    cJSON* render = cJSON_GetObjectItem(root, kRender);
    cJSON* mode = render ? cJSON_GetObjectItem(render, kMode) : nullptr;
    bool ok = cJSON_IsString(mode) && strcmp(mode->valuestring, kLayered) == 0;
    cJSON* roi = render ? cJSON_GetObjectItem(render, kMouthRoi) : nullptr;
    ok = ok && cJSON_IsObject(roi) &&
         JsonInt(roi, kX, &out->x) && JsonInt(roi, kY, &out->y) &&
         JsonInt(roi, kWidth, &out->w) && JsonInt(roi, kHeight, &out->h);
    ok = ok && out->x >= 0 && out->y >= 0 && out->w >= 8 && out->h >= 8 &&
         out->x + out->w <= 480 && out->y + out->h <= 480;

    cJSON* hold = render ? cJSON_GetObjectItem(render, kHoldBase) : nullptr;
    cJSON* hold_file = hold ? cJSON_GetObjectItem(hold, kRgb565) : nullptr;
    int base_w = 0, base_h = 0;
    ok = ok && cJSON_IsString(hold_file) && SafePath(hold_file->valuestring) &&
         JsonInt(hold, kWidth, &base_w) && JsonInt(hold, kHeight, &base_h) &&
         base_w == 480 && base_h == 480;
    if (ok) {
        char path[224];
        snprintf(path, sizeof(path), kAssetFormat, kRoot, emotion, hold_file->valuestring);
        const size_t bytes = static_cast<size_t>(base_w) * base_h * 2U;
        ok = ReadAsset(path, bytes, static_cast<uint16_t>(base_w),
                       static_cast<uint16_t>(base_h), &out->base);
        if (ok) out->bytes += bytes;
    }

    cJSON* levels = render ? cJSON_GetObjectItem(render, kMouthLevels) : nullptr;
    for (size_t i = 0; ok && i < kLevelCount; ++i) {
        cJSON* level = cJSON_IsObject(levels)
                           ? cJSON_GetObjectItem(levels, kLevels[i]) : nullptr;
        cJSON* file = level ? cJSON_GetObjectItem(level, kRgb565) : nullptr;
        ok = cJSON_IsString(file) && SafePath(file->valuestring);
        if (!ok) break;
        char path[224];
        snprintf(path, sizeof(path), kAssetFormat, kRoot, emotion, file->valuestring);
        const size_t bytes = static_cast<size_t>(out->w) * out->h * 2U;
        ok = ReadAsset(path, bytes, static_cast<uint16_t>(out->w),
                       static_cast<uint16_t>(out->h), &out->mouth[i]);
        if (ok) out->bytes += bytes;
    }
    cJSON* composite = render ? cJSON_GetObjectItem(render, kMouthComposite) : nullptr;
    cJSON* method = composite ? cJSON_GetObjectItem(composite, kMethod) : nullptr;
    out->precomposited =
        cJSON_IsString(method) &&
        (strcmp(method->valuestring, kCompositeV1) == 0 ||
         strcmp(method->valuestring, kCompositeV2) == 0);
    ok = ok && out->precomposited;
    cJSON_Delete(root);
    if (!ok) {
        FreeEntry(out);
        return false;
    }
    out->emotion = emotion;
    return true;
}

int FindEmotion(const char* name) {
    const char* canonical = emotion_video_player_canonicalize_emotion(name);
    if (!canonical) return -1;
    for (size_t i = 0; i < kEmotionCount; ++i) {
        if (strcasecmp(canonical, kEmotions[i]) == 0) return static_cast<int>(i);
    }
    return -1;
}

const Entry* ActiveEntry() {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !g_ready.load(std::memory_order_acquire)) return nullptr;
    const int index = g_active.load(std::memory_order_acquire);
    if (index < 0 || index >= static_cast<int>(kEmotionCount)) return nullptr;
    return g_bank[index].base.rgb ? &g_bank[index] : nullptr;
}
}  // namespace

bool FaceSpeechCore_Enabled(void) {
    return g_enabled.load(std::memory_order_acquire);
}
void FaceSpeechCore_SetEnabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
    if (!on) g_active.store(-1, std::memory_order_release);
}

bool FaceSpeechCore_Preload(void) {
    if (!FaceSpeechCore_Enabled()) return false;
    if (g_ready.load(std::memory_order_acquire)) return true;
    Entry staged[kEmotionCount]{};
    size_t total = 0;
    for (size_t i = 0; i < kEmotionCount; ++i) {
        if (!LoadEntry(kEmotions[i], &staged[i])) {
            FreeEntries(staged);
            esp_rom_printf(kPreloadEmotion, kEmotions[i]);
            return false;
        }
        total += staged[i].bytes;
    }
    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_after < kPsramReserve) {
        esp_rom_printf(kPreloadReserve,
                       static_cast<unsigned>(free_after));
        FreeEntries(staged);
        return false;
    }
    for (size_t i = 0; i < kEmotionCount; ++i) {
        g_bank[i] = staged[i];
        staged[i] = {};
    }
    g_total = total;
    g_active.store(-1, std::memory_order_relaxed);
    g_ready.store(true, std::memory_order_release);
    esp_rom_printf(kPreloadOk,
                   static_cast<unsigned>(kEmotionCount), static_cast<unsigned>(total),
                   static_cast<unsigned>(free_after));
    return true;
}

bool FaceSpeechCore_Ready(const char* emotion_name) {
    if (!FaceSpeechCore_Enabled() ||
        !g_ready.load(std::memory_order_acquire)) return false;
    const int index = FindEmotion(emotion_name);
    return index >= 0 && g_bank[index].base.rgb;
}
bool FaceSpeechCore_Select(const char* emotion_name) {
    const int index = FindEmotion(emotion_name);
    if (index < 0 || !FaceSpeechCore_Ready(emotion_name)) return false;
    g_active.store(index, std::memory_order_release);
    return true;
}
void FaceSpeechCore_Deactivate(void) {
    g_active.store(-1, std::memory_order_release);
}
bool FaceSpeechCore_Active(void) {
    return ActiveEntry() != nullptr;
}
const char* FaceSpeechCore_ActiveEmotion(void) {
    const Entry* entry = ActiveEntry();
    return entry ? entry->emotion : nullptr;
}

const uint8_t* FaceSpeechCore_Base(uint16_t* out_w, uint16_t* out_h) {
    const Entry* entry = ActiveEntry();
    if (!entry) return nullptr;
    if (out_w) *out_w = entry->base.w;
    if (out_h) *out_h = entry->base.h;
    return entry->base.rgb;
}
const uint8_t* FaceSpeechCore_Patch(uint8_t level, uint16_t* out_w, uint16_t* out_h) {
    const Entry* entry = ActiveEntry();
    if (!entry || level >= kLevelCount) return nullptr;
    if (out_w) *out_w = entry->mouth[level].w;
    if (out_h) *out_h = entry->mouth[level].h;
    return entry->mouth[level].rgb;
}
void FaceSpeechCore_Roi(int* x, int* y, int* w, int* h) {
    const Entry* entry = ActiveEntry();
    if (x) *x = entry ? entry->x : 0;
    if (y) *y = entry ? entry->y : 0;
    if (w) *w = entry ? entry->w : 0;
    if (h) *h = entry ? entry->h : 0;
}
bool FaceSpeechCore_MouthPrecomposited(void) {
    const Entry* entry = ActiveEntry();
    return entry && entry->precomposited;
}
size_t FaceSpeechCore_TotalBytes(void) {
    return g_ready.load(std::memory_order_acquire) ? g_total : 0;
}

#endif
