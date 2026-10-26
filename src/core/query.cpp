#include "core/query.hpp"

#include <array>
#include <vector>

#include "core/text.hpp"

namespace beam {
namespace {

constexpr std::array<const wchar_t*, 72> kKnownTlds = {
    L"com", L"net", L"org", L"io", L"dev", L"app", L"ai", L"co", L"tw", L"jp", L"cn", L"hk",
    L"kr", L"sg", L"uk", L"us", L"de", L"fr", L"nl", L"eu", L"ca", L"au", L"in", L"gov",
    L"edu", L"me", L"info", L"biz", L"xyz", L"tv", L"gg", L"so", L"ly", L"to", L"fm", L"cc",
    L"im", L"tech", L"site", L"online", L"cloud", L"blog", L"news", L"page", L"link", L"run",
    L"wiki", L"store", L"shop", L"design", L"art", L"club", L"live", L"studio", L"space", L"top",
    L"pro", L"asia", L"moe", L"one", L"ch", L"se", L"no", L"fi", L"it", L"es", L"ru", L"br",
    L"mx", L"nz", L"vn", L"th",
};

bool IsIpv4(std::wstring_view host) {
    int parts = 0;
    size_t i = 0;
    while (i <= host.size()) {
        size_t j = i;
        int value = 0;
        while (j < host.size() && IsAsciiDigit(host[j]) && j - i < 3) value = value * 10 + (host[j++] - L'0');
        if (j == i || value > 255) return false;
        ++parts;
        if (j == host.size()) break;
        if (host[j] != L'.') return false;
        i = j + 1;
    }
    return parts == 4;
}

bool IsDomain(std::wstring_view host) {
    std::vector<std::wstring_view> labels;
    size_t start = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == L'.') {
            labels.push_back(host.substr(start, i - start));
            start = i + 1;
        }
    }
    if (labels.size() < 2) return false;
    for (const auto& label : labels) {
        if (label.empty() || label.size() > 63 || label.front() == L'-' || label.back() == L'-') return false;
        for (const wchar_t c : label)
            if (!(IsAsciiAlpha(c) || IsAsciiDigit(c) || c == L'-')) return false;
    }
    const std::wstring tld = Fold(labels.back());
    for (const wchar_t* known : kKnownTlds)
        if (tld == known) return true;
    return false;
}

}  // namespace

std::optional<std::wstring> TryGetUrl(std::wstring_view input) {
    const std::wstring s = Trim(input);
    if (s.empty()) return std::nullopt;
    for (const wchar_t c : s)
        if (IsSpace(c)) return std::nullopt;

    for (const wchar_t* scheme : {L"http://", L"https://", L"ftp://"}) {
        const std::wstring_view sv(scheme);
        if (StartsWithIgnoreCase(s, sv) && s.size() > sv.size()) return s;
    }

    const size_t hostEnd = s.find_first_of(L":/?#");
    const std::wstring host = s.substr(0, hostEnd == std::wstring::npos ? s.size() : hostEnd);
    const std::wstring tail = hostEnd == std::wstring::npos ? L"" : s.substr(hostEnd);
    if (host.empty()) return std::nullopt;
    if (!tail.empty() && tail[0] == L':') {
        size_t i = 1;
        while (i < tail.size() && IsAsciiDigit(tail[i])) ++i;
        if (i == 1 || i > 6) return std::nullopt;
        if (i < tail.size() && tail[i] != L'/' && tail[i] != L'?' && tail[i] != L'#') return std::nullopt;
    }
    const std::wstring lower = Fold(host);
    if (lower == L"localhost" || IsIpv4(lower)) return L"http://" + s;
    if (IsDomain(host)) return L"https://" + s;
    return std::nullopt;
}

bool LooksLikePath(std::wstring_view input) {
    const std::wstring s = Trim(input);
    if (s.size() >= 2 && IsAsciiAlpha(s[0]) && s[1] == L':' && (s.size() == 2 || s[2] == L'\\' || s[2] == L'/'))
        return true;
    if (s.size() > 2 && s[0] == L'\\' && s[1] == L'\\') return true;
    if (s == L"~" || (s.size() >= 2 && s[0] == L'~' && (s[1] == L'\\' || s[1] == L'/'))) return true;
    if (s.size() > 2 && s[0] == L'%') {
        const size_t close = s.find(L'%', 1);
        if (close != std::wstring::npos && close > 1) return true;
    }
    return false;
}

KeywordSplit SplitKeyword(std::wstring_view input) {
    KeywordSplit r;
    const std::wstring s = TrimStart(input);
    size_t space = std::wstring::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (IsSpace(s[i])) {
            space = i;
            break;
        }
    }
    if (space == std::wstring::npos) {
        r.keyword = s;
        return r;
    }
    r.keyword = s.substr(0, space);
    r.rest = TrimStart(std::wstring_view(s).substr(space));
    r.hasSpace = true;
    return r;
}

std::string PercentEncode(std::string_view utf8) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : utf8) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

bool HasQueryPlaceholder(std::wstring_view tmpl) { return FindIgnoreCase(tmpl, L"{query}") != std::wstring::npos; }

std::wstring FillTemplate(std::wstring_view tmpl, std::wstring_view query, bool urlEncode) {
    const std::wstring value = urlEncode ? Utf8ToWide(PercentEncode(WideToUtf8(query))) : std::wstring(query);
    std::wstring out;
    size_t from = 0;
    for (;;) {
        const size_t at = FindIgnoreCase(tmpl, L"{query}", from);
        if (at == std::wstring::npos) {
            out.append(tmpl.substr(from));
            return out;
        }
        out.append(tmpl.substr(from, at - from));
        out += value;
        from = at + 7;
    }
}

}  // namespace beam
