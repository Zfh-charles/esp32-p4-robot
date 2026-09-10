#pragma once

namespace domain {

// Returns a canonical face name or nullptr when the text carries no explicit
// emotion intent. The returned pointer has static storage duration.
const char* InferCanonicalEmotionFromText(const char* text) noexcept;

}  // namespace domain
