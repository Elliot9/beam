#include "core/fuzzy.hpp"

#include <algorithm>

#include "core/text.hpp"

namespace beam {
namespace {

bool IsSeparator(wchar_t c) {
    switch (c) {
        case L' ': case L'-': case L'_': case L'.': case L'/': case L'\\': case L'(': case L')':
        case L'[': case L']': case L':': case L',': case L'+': case L'&': case L'|': case L'@':
        case L'#': case L'\'': case L'"': case 0x3000: case 0x00B7: case 0x30FB:
        case 0xFF08: case 0xFF09: case 0x3001: case 0xFF0C:
            return true;
        default:
            return false;
    }
}

bool IsUpper(wchar_t c) { return c >= L'A' && c <= L'Z'; }
bool IsLower(wchar_t c) { return c >= L'a' && c <= L'z'; }

// `orig` is the un-folded candidate so camelCase boundaries are still visible.
bool IsWordStart(std::wstring_view orig, size_t i) {
    const wchar_t c = orig[i];
    if (IsSeparator(c)) return false;
    if (i == 0) return true;
    const wchar_t p = orig[i - 1];
    if (IsSeparator(p)) return true;
    if (IsCjk(c)) return true;
    if (IsUpper(c) && IsLower(p)) return true;
    if (IsAsciiDigit(c) && IsAsciiAlpha(p)) return true;
    if (IsAsciiAlpha(c) && IsAsciiDigit(p)) return true;
    return false;
}

int InitialsScore(std::wstring_view q, std::wstring_view c, std::wstring_view orig) {
    std::vector<size_t> starts;
    for (size_t i = 0; i < orig.size(); ++i)
        if (IsWordStart(orig, i)) starts.push_back(i);
    if (starts.size() < q.size()) return kNoMatch;

    size_t next = 0;
    size_t first = 0;
    bool contiguous = true;
    for (size_t qi = 0; qi < q.size(); ++qi) {
        size_t found = starts.size();
        for (size_t k = next; k < starts.size(); ++k) {
            if (c[starts[k]] == q[qi]) {
                found = k;
                break;
            }
        }
        if (found == starts.size()) return kNoMatch;
        if (qi == 0) first = found;
        else if (found != next) contiguous = false;
        next = found + 1;
    }
    const int base = contiguous ? 700 : 650;
    return base - static_cast<int>(std::min<size_t>(40, first * 10));
}

int SubsequenceScore(std::wstring_view q, std::wstring_view c, std::wstring_view orig) {
    int points = 0;
    size_t from = 0;
    size_t first = std::wstring_view::npos;
    size_t prev = std::wstring_view::npos;
    for (const wchar_t qc : q) {
        if (IsSpace(qc)) continue;
        size_t found = c.find(qc, from);
        if (first == std::wstring_view::npos) {
            // The first character must start a word; otherwise matches are mostly noise.
            while (found != std::wstring_view::npos && !IsWordStart(orig, found)) found = c.find(qc, found + 1);
            if (found == std::wstring_view::npos) return kNoMatch;
            first = found;
        }
        if (found == std::wstring_view::npos) return kNoMatch;
        int bonus = 1;
        if (prev != std::wstring_view::npos && found == prev + 1) bonus += 5;
        if (IsWordStart(orig, found)) bonus += 8;
        points += bonus;
        prev = found;
        from = found + 1;
    }
    if (first == std::wstring_view::npos) return kNoMatch;
    const double quality = points / (14.0 * static_cast<double>(q.size()));
    const double span = static_cast<double>(prev - first + 1);
    const double compact = static_cast<double>(q.size()) / span;
    int score = static_cast<int>(150 + 250 * (0.6 * quality + 0.4 * compact));
    score -= static_cast<int>(std::min<size_t>(50, first));
    if (score < 180) return kNoMatch;  // too scattered to be useful
    return std::min(score, 450);
}

int ScoreToken(std::wstring_view q, std::wstring_view c, std::wstring_view orig) {
    size_t idx = c.find(q);
    if (idx != std::wstring_view::npos) {
        for (size_t pos = idx; pos != std::wstring_view::npos; pos = c.find(q, pos + 1)) {
            if (IsWordStart(orig, pos)) return 800 - static_cast<int>(std::min<size_t>(100, pos));
        }
        return 600 - static_cast<int>(std::min<size_t>(100, idx));
    }
    if (q.size() >= 2) {
        const int initials = InitialsScore(q, c, orig);
        if (initials != kNoMatch) return initials;
        return SubsequenceScore(q, c, orig);
    }
    return kNoMatch;
}

}  // namespace

int FuzzyScore(std::wstring_view query, std::wstring_view candidate) {
    const std::wstring q = Fold(Trim(query));
    if (q.empty()) return 0;
    if (candidate.empty()) return kNoMatch;
    const std::wstring c = Fold(candidate);

    if (c == q) return 1000;
    if (c.size() > q.size() && c.compare(0, q.size(), q) == 0)
        return 900 - static_cast<int>(std::min<size_t>(50, c.size() - q.size()));

    const auto tokens = SplitWhitespace(q);
    if (tokens.size() > 1) {
        int total = 0;
        for (const auto& t : tokens) {
            const int s = ScoreToken(t, c, candidate);
            if (s == kNoMatch) return kNoMatch;
            total += s;
        }
        return std::min(850, total / static_cast<int>(tokens.size()));
    }
    return ScoreToken(q, c, candidate);
}

int BestScore(std::wstring_view query, std::wstring_view title,
              const std::vector<std::wstring>& aliases, int aliasPenalty) {
    int best = FuzzyScore(query, title);
    for (const auto& alias : aliases) {
        const int s = FuzzyScore(query, alias);
        if (s == kNoMatch) continue;
        best = std::max(best, std::max(1, s - aliasPenalty));
    }
    return best;
}

}  // namespace beam
