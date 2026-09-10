#include "../../main/domain/emotion_intent_policy.h"

#include <cassert>
#include <cstring>

namespace {

void Expect(const char* text, const char* expected) {
    const char* actual = domain::InferCanonicalEmotionFromText(text);
    if (expected == nullptr) {
        assert(actual == nullptr);
        return;
    }
    assert(actual != nullptr);
    assert(std::strcmp(actual, expected) == 0);
}

}  // namespace

int main() {
    Expect(nullptr, nullptr);
    Expect("", nullptr);
    Expect("今天天气怎么样", nullptr);

    Expect("请做一个愤怒表情", "angry");
    Expect("我有点伤心", "sad");
    Expect("今天很开心", "happy");
    Expect("我喜欢你", "loving");
    Expect("保持平静", "neutral");

    // Preserve the legacy precedence when a sentence contains multiple cues.
    Expect("虽然开心但是很生气", "angry");
    Expect("happy but sad", "sad");
    return 0;
}
