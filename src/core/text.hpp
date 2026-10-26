// Portable string helpers shared by the core and the Windows front end.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace beam {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

// Lower-cases a character for searching. Also folds full-width ASCII
// (as typed by CJK IMEs in full-width mode) to plain ASCII. Always 1:1,
// so indices into the folded string match the original.
wchar_t FoldChar(wchar_t c);
std::wstring Fold(std::wstring_view s);

bool IsSpace(wchar_t c);
bool IsCjk(wchar_t c);
bool IsAsciiDigit(wchar_t c);
bool IsAsciiAlpha(wchar_t c);

std::wstring Trim(std::wstring_view s);
std::wstring TrimStart(std::wstring_view s);
bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b);
bool StartsWithIgnoreCase(std::wstring_view s, std::wstring_view prefix);
bool EndsWithIgnoreCase(std::wstring_view s, std::wstring_view suffix);
size_t FindIgnoreCase(std::wstring_view s, std::wstring_view needle, size_t from = 0);
std::vector<std::wstring> SplitWhitespace(std::wstring_view s);

}  // namespace beam
