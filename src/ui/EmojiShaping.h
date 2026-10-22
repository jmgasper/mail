#pragma once
#include <Font.h>
#include <optional>
#include <string_view>

namespace mail::ui {
// Fonts are private to the application. A shaped glyph is drawn at the first
// scalar; continuation scalars keep their Unicode but have zero advance.
std::optional<BFont> EmojiSequenceFont(std::string_view cluster);
BFont EmojiContinuationFont();
// Select the font's designed weight while preserving size and decorations.
std::optional<BFont> StyledEmojiFont(const BFont&,bool bold);
bool IsShapedEmojiFont(const BFont&);
struct EmojiFontCacheInfo { std::size_t fonts, bytes, glyphLimit; };
EmojiFontCacheInfo EmojiShapingCacheInfo();
}
