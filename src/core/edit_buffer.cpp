#include "core/edit_buffer.hpp"

#include <algorithm>

#include "core/text.hpp"

namespace beam {
namespace {

bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

int CharClass(wchar_t c) {
    if (IsSpace(c)) return 0;
    if (IsAsciiDigit(c) || IsAsciiAlpha(c) || c == L'_' || IsCjk(c)) return 1;
    if (c < 0x80) return 2;  // ASCII punctuation
    return 1;                // other letters
}

}  // namespace

void EditBuffer::SetText(std::wstring_view text, bool selectAll) {
    text_.assign(text);
    caret_ = text_.size();
    anchor_ = selectAll ? 0 : caret_;
}

void EditBuffer::Insert(std::wstring_view s) {
    std::wstring clean;
    clean.reserve(s.size());
    for (const wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            if (!clean.empty() && clean.back() != L' ') clean.push_back(L' ');
        } else if (c >= 0x20 && c != 0x7F) {
            clean.push_back(c);
        }
    }
    if (clean.empty() && !HasSelection()) return;
    const size_t start = SelStart();
    text_.replace(start, SelEnd() - start, clean);
    caret_ = anchor_ = start + clean.size();
}

bool EditBuffer::DeleteSelection() {
    if (!HasSelection()) return false;
    const size_t start = SelStart();
    text_.erase(start, SelEnd() - start);
    caret_ = anchor_ = start;
    return true;
}

bool EditBuffer::Backspace(bool word) {
    if (DeleteSelection()) return true;
    if (caret_ == 0) return false;
    const size_t from = word ? PrevWord(caret_) : PrevPos(caret_);
    text_.erase(from, caret_ - from);
    caret_ = anchor_ = from;
    return true;
}

bool EditBuffer::Delete(bool word) {
    if (DeleteSelection()) return true;
    if (caret_ >= text_.size()) return false;
    const size_t to = word ? NextWord(caret_) : NextPos(caret_);
    text_.erase(caret_, to - caret_);
    anchor_ = caret_;
    return true;
}

void EditBuffer::MoveLeft(bool word, bool select) {
    size_t target;
    if (!select && HasSelection() && !word) target = SelStart();
    else target = word ? PrevWord(caret_) : PrevPos(caret_);
    SetCaret(target, select);
}

void EditBuffer::MoveRight(bool word, bool select) {
    size_t target;
    if (!select && HasSelection() && !word) target = SelEnd();
    else target = word ? NextWord(caret_) : NextPos(caret_);
    SetCaret(target, select);
}

void EditBuffer::Home(bool select) { SetCaret(0, select); }
void EditBuffer::End(bool select) { SetCaret(text_.size(), select); }

void EditBuffer::SelectAll() {
    anchor_ = 0;
    caret_ = text_.size();
}

void EditBuffer::SetCaret(size_t pos, bool select) {
    caret_ = std::min(pos, text_.size());
    if (caret_ > 0 && caret_ < text_.size() && IsLowSurrogate(text_[caret_]) && IsHighSurrogate(text_[caret_ - 1]))
        --caret_;
    if (!select) anchor_ = caret_;
}

void EditBuffer::SelectWordAt(size_t pos) {
    if (text_.empty()) return;
    pos = std::min(pos, text_.size() - 1);
    const int cls = CharClass(text_[pos]);
    size_t b = pos, e = pos + 1;
    while (b > 0 && CharClass(text_[b - 1]) == cls) --b;
    while (e < text_.size() && CharClass(text_[e]) == cls) ++e;
    anchor_ = b;
    caret_ = e;
}

size_t EditBuffer::PrevPos(size_t pos) const {
    if (pos == 0) return 0;
    if (pos >= 2 && IsLowSurrogate(text_[pos - 1]) && IsHighSurrogate(text_[pos - 2])) return pos - 2;
    return pos - 1;
}

size_t EditBuffer::NextPos(size_t pos) const {
    if (pos >= text_.size()) return text_.size();
    if (pos + 1 < text_.size() && IsHighSurrogate(text_[pos]) && IsLowSurrogate(text_[pos + 1])) return pos + 2;
    return pos + 1;
}

size_t EditBuffer::PrevWord(size_t pos) const {
    while (pos > 0 && CharClass(text_[pos - 1]) == 0) --pos;
    if (pos == 0) return 0;
    const int cls = CharClass(text_[pos - 1]);
    while (pos > 0 && CharClass(text_[pos - 1]) == cls) --pos;
    return pos;
}

size_t EditBuffer::NextWord(size_t pos) const {
    if (pos >= text_.size()) return text_.size();
    const int cls = CharClass(text_[pos]);
    if (cls != 0)
        while (pos < text_.size() && CharClass(text_[pos]) == cls) ++pos;
    while (pos < text_.size() && CharClass(text_[pos]) == 0) ++pos;
    return pos;
}

}  // namespace beam
