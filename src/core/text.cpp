#include "core/text.hpp"

#include <cstdint>

namespace beam {
namespace {

void AppendCodePoint(std::wstring& out, uint32_t cp) {
    if constexpr (sizeof(wchar_t) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            return;
        }
    }
    out.push_back(static_cast<wchar_t>(cp));
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::wstring Utf8ToWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len > 1) {
            if (i + len > s.size()) {
                cp = 0xFFFD;
                len = 1;
            } else {
                uint32_t v = b0 & (0xFF >> (len + 1));
                bool ok = true;
                for (size_t k = 1; k < len; ++k) {
                    const auto b = static_cast<unsigned char>(s[i + k]);
                    if ((b & 0xC0) != 0x80) {
                        ok = false;
                        break;
                    }
                    v = (v << 6) | (b & 0x3F);
                }
                const bool overlong = (len == 2 && v < 0x80) || (len == 3 && v < 0x800) ||
                                      (len == 4 && v < 0x10000);
                if (!ok || overlong || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
                    cp = 0xFFFD;
                    len = 1;
                } else {
                    cp = v;
                }
            }
        }
        AppendCodePoint(out, cp);
        i += len;
    }
    return out;
}

std::string WideToUtf8(std::wstring_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
                const uint32_t lo = static_cast<uint32_t>(s[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++i;
                }
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // unpaired surrogate
        if (cp > 0x10FFFF) cp = 0xFFFD;
        AppendUtf8(out, cp);
    }
    return out;
}

wchar_t FoldChar(wchar_t c) {
    const auto u = static_cast<uint32_t>(c);
    if (u < 0x80) return (u >= 'A' && u <= 'Z') ? static_cast<wchar_t>(u + 32) : c;
    if (u >= 0xFF01 && u <= 0xFF5E) return FoldChar(static_cast<wchar_t>(u - 0xFEE0));  // full-width ASCII
    if (u == 0x3000) return L' ';                                                       // ideographic space
    if (u >= 0xC0 && u <= 0xDE && u != 0xD7) return static_cast<wchar_t>(u + 32);       // Latin-1
    if ((u >= 0x100 && u <= 0x12F) || (u >= 0x132 && u <= 0x137) || (u >= 0x14A && u <= 0x177))
        return (u % 2 == 0) ? static_cast<wchar_t>(u + 1) : c;
    if ((u >= 0x139 && u <= 0x148) || (u >= 0x179 && u <= 0x17E))
        return (u % 2 == 1) ? static_cast<wchar_t>(u + 1) : c;
    if (u >= 0x391 && u <= 0x3A9 && u != 0x3A2) return static_cast<wchar_t>(u + 32);  // Greek
    if (u >= 0x410 && u <= 0x42F) return static_cast<wchar_t>(u + 32);                // Cyrillic
    if (u >= 0x400 && u <= 0x40F) return static_cast<wchar_t>(u + 80);
    return c;
}

std::wstring Fold(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = FoldChar(c);
    return out;
}

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000 || c == 0xA0;
}

bool IsCjk(wchar_t c) {
    const auto u = static_cast<uint32_t>(c);
    return (u >= 0x3040 && u <= 0x30FF) ||  // kana
           (u >= 0x3100 && u <= 0x312F) ||  // bopomofo
           (u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF) ||
           (u >= 0xAC00 && u <= 0xD7AF) ||  // hangul
           (u >= 0xF900 && u <= 0xFAFF);
}

bool IsAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsAsciiAlpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

std::wstring TrimStart(std::wstring_view s) {
    size_t b = 0;
    while (b < s.size() && IsSpace(s[b])) ++b;
    return std::wstring(s.substr(b));
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (FoldChar(a[i]) != FoldChar(b[i])) return false;
    return true;
}

bool StartsWithIgnoreCase(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && EqualsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

bool EndsWithIgnoreCase(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && EqualsIgnoreCase(s.substr(s.size() - suffix.size()), suffix);
}

size_t FindIgnoreCase(std::wstring_view s, std::wstring_view needle, size_t from) {
    if (needle.empty()) return from <= s.size() ? from : std::wstring_view::npos;
    if (needle.size() > s.size()) return std::wstring_view::npos;
    for (size_t i = from; i + needle.size() <= s.size(); ++i) {
        if (EqualsIgnoreCase(s.substr(i, needle.size()), needle)) return i;
    }
    return std::wstring_view::npos;
}

std::vector<std::wstring> SplitWhitespace(std::wstring_view s) {
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpace(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !IsSpace(s[j])) ++j;
        if (j > i) parts.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return parts;
}

}  // namespace beam
