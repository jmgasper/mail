#pragma once
#include "mail/Http.h"
#include <nlohmann/json.hpp>

namespace mail::google {
using Json = nlohmann::json;
inline Json parse(const std::string& text)
{
    try {
        auto value = Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32) throw NetworkError(NetworkFailure::Protocol, "Google returned excessively nested JSON.");
            return true;
        });
        if (!value.is_object()) throw NetworkError(NetworkFailure::Protocol, "Google returned an invalid JSON object.");
        return value;
    } catch (const Json::exception&) { throw NetworkError(NetworkFailure::Protocol, "Google returned malformed JSON."); }
}
inline std::string string(const Json& value, const char* key, std::size_t max = 16384, bool required = true)
{
    auto item = value.find(key);
    if (item == value.end() && !required) return {};
    if (item == value.end() || !item->is_string()) throw NetworkError(NetworkFailure::Protocol, "Google returned an invalid field.");
    auto text = item->get<std::string>();
    if (text.size() > max || text.find('\0') != std::string::npos || (required && text.empty()))
        throw NetworkError(NetworkFailure::Protocol, "Google returned an invalid field size.");
    return text;
}
inline void opaque(const std::string& value, const char* description, std::size_t max = 16384)
{
    if (value.empty() || value.size() > max) throw NetworkError(NetworkFailure::Protocol, std::string("Invalid ") + description + ".");
    for (unsigned char c : value) if (c < 33 || c > 126)
        throw NetworkError(NetworkFailure::Protocol, std::string("Invalid ") + description + ".");
}
}
