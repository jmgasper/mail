#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace mail {
// UTF-8 byte offsets for Unicode extended grapheme clusters. Input bytes are
// never normalized or rewritten; offsets can be used directly with BTextView.
class TextClusters {
public:
    explicit TextClusters(std::string_view);
    const std::vector<int32_t>& Boundaries() const { return fBoundaries; }
    int32_t Floor(int32_t) const;
    int32_t Ceil(int32_t) const;
    int32_t Before(int32_t) const;
    int32_t After(int32_t) const;
    bool IsBoundary(int32_t) const;
private:
    std::vector<int32_t> fBoundaries;
};
}
