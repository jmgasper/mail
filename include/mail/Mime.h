#pragma once
#include "Model.h"
#include <string_view>

namespace mail {
// Bounded parser for RFC 5322 messages and nested MIME multiparts.
// HTML is retained for the isolated formatted reader; the main reader uses text.
Message parseMessage(std::string_view raw);
std::string base64Encode(std::string_view bytes);
std::string base64Decode(std::string_view text);
std::string escapeHtml(std::string_view text);
std::string plainFromHtml(std::string_view html);
}
