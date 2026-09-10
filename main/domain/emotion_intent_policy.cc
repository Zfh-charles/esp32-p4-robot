#include "emotion_intent_policy.h"

#include <cstring>

namespace domain {

const char* InferCanonicalEmotionFromText(const char* text) noexcept {
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    // Keep the precedence explicit and avoid persistent pointer tables. Besides
    // being simpler, this keeps the P4 image below the next 64 KiB mapped-image
    // boundary; crossing it creates a nearly 64 KiB RAM padding segment during
    // image generation on this target.
    if (std::strstr(text, "愤怒") || std::strstr(text, "生气") ||
        std::strstr(text, "发火") || std::strstr(text, "怒") ||
        std::strstr(text, "angry")) return "angry";
    if (std::strstr(text, "悲伤") || std::strstr(text, "难过") ||
        std::strstr(text, "伤心") || std::strstr(text, "哭") ||
        std::strstr(text, "sad")) return "sad";
    if (std::strstr(text, "开心") || std::strstr(text, "高兴") ||
        std::strstr(text, "快乐") || std::strstr(text, "笑") ||
        std::strstr(text, "happy")) return "happy";
    if (std::strstr(text, "喜欢") || std::strstr(text, "亲亲") ||
        std::strstr(text, "loving") || std::strstr(text, "love")) return "loving";
    if (std::strstr(text, "中性") || std::strstr(text, "平静") ||
        std::strstr(text, "neutral")) return "neutral";
    return nullptr;
}

}  // namespace domain
