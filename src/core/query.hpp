// Query classification helpers: URLs, paths, "keyword argument" splitting
// and {query} template expansion.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace beam {

// "github.com", "https://x.dev/a", "localhost:3000", "10.0.0.1:8080/x" -> full URL.
// File names such as "notepad.exe" or "readme.md" are not URLs.
std::optional<std::wstring> TryGetUrl(std::wstring_view input);

// "C:\...", "D:", "\\server\share", "~", "~\Documents", "%APPDATA%\x"
bool LooksLikePath(std::wstring_view input);

struct KeywordSplit {
    std::wstring keyword;  // first token
    std::wstring rest;     // everything after the first space (leading spaces trimmed)
    bool hasSpace = false;
};
KeywordSplit SplitKeyword(std::wstring_view input);

std::string PercentEncode(std::string_view utf8);
bool HasQueryPlaceholder(std::wstring_view tmpl);
// Replaces every "{query}" (case-insensitive). URLs get the query percent-encoded.
std::wstring FillTemplate(std::wstring_view tmpl, std::wstring_view query, bool urlEncode);

}  // namespace beam
