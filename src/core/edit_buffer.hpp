// Single-line text editing model behind the search field (caret, selection,
// word-wise movement). Independent of rendering so it can be unit tested.
#pragma once

#include <string>
#include <string_view>

namespace beam {

class EditBuffer {
public:
    const std::wstring& Text() const { return text_; }
    size_t Caret() const { return caret_; }
    size_t Anchor() const { return anchor_; }
    bool HasSelection() const { return caret_ != anchor_; }
    size_t SelStart() const { return caret_ < anchor_ ? caret_ : anchor_; }
    size_t SelEnd() const { return caret_ < anchor_ ? anchor_ : caret_; }
    std::wstring SelectedText() const { return text_.substr(SelStart(), SelEnd() - SelStart()); }

    void SetText(std::wstring_view text, bool selectAll = false);
    void Insert(std::wstring_view s);  // replaces the selection; drops control characters
    bool Backspace(bool word);
    bool Delete(bool word);
    bool DeleteSelection();
    void MoveLeft(bool word, bool select);
    void MoveRight(bool word, bool select);
    void Home(bool select);
    void End(bool select);
    void SelectAll();
    void SetCaret(size_t pos, bool select);
    void SelectWordAt(size_t pos);

private:
    size_t PrevPos(size_t pos) const;
    size_t NextPos(size_t pos) const;
    size_t PrevWord(size_t pos) const;
    size_t NextWord(size_t pos) const;

    std::wstring text_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
};

}  // namespace beam
