// Direct2D/DirectWrite drawing into a per-pixel-alpha layered window
// (rounded corners and a soft shadow on Windows 10, no DWM tricks needed).
#pragma once

#include "win/common.hpp"

#include <d2d1.h>
#include <dwrite.h>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

#include "win/icons.hpp"

namespace beam {

struct Palette {
    bool dark = true;
    uint32_t background, footer, border, separator;
    uint32_t text, textSecondary, textTertiary;
    uint32_t selection, panel, panelBorder, keycap, keycapText, chip, chipText;
    uint32_t warning, accent, textSelection, caret, shadow;
};
Palette MakePalette(bool dark);
Palette ResolvePalette(const std::wstring& theme);  // "auto" follows the Windows app theme

// Mono wraps (detail view); every other font is single-line, vertically centred and trimmed.
enum class Font { Input, Title, Subtitle, Small, SmallBold, Keycap, Glyph, GlyphLarge, Brand, Mono, Count };

class Renderer {
public:
    ~Renderer();
    bool Initialize();

    // Begin/End wrap a frame; End pushes the pixels with UpdateLayeredWindow.
    bool Begin(int pixelWidth, int pixelHeight, float dpi);
    bool End(HWND hwnd, POINT screenPos);

    ID2D1RenderTarget* rt() const { return rt_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    IDWriteTextFormat* Format(Font font) const { return formats_[static_cast<size_t>(font)].Get(); }

    ID2D1SolidColorBrush* Brush(uint32_t argb);
    void FillRect(const D2D1_RECT_F& r, uint32_t argb);
    void FillRoundRect(const D2D1_RECT_F& r, float radius, uint32_t argb);
    void StrokeRoundRect(const D2D1_RECT_F& r, float radius, uint32_t argb, float width = 1.0f);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t argb, float width = 1.0f);
    void Text(std::wstring_view text, const D2D1_RECT_F& r, Font font, uint32_t argb,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextWidth(std::wstring_view text, Font font) const;
    ComPtr<IDWriteTextLayout> Layout(std::wstring_view text, Font font, float maxWidth, float maxHeight) const;
    void DrawIcon(const std::wstring& key, const std::shared_ptr<const IconPixels>& pixels, const D2D1_RECT_F& dest);
    void Shadow(const D2D1_RECT_F& r, float radius, float spread, uint32_t argb);

    void PushRoundClip(const D2D1_RECT_F& r, float radius);
    void PopRoundClip();
    void PushClip(const D2D1_RECT_F& r);
    void PopClip();

private:
    bool EnsureSurface(int width, int height);
    void ReleaseDeviceResources();

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    std::array<ComPtr<IDWriteTextFormat>, static_cast<size_t>(Font::Count)> formats_;
    ComPtr<ID2D1DCRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1Layer> layer_;
    ComPtr<ID2D1RoundedRectangleGeometry> clipGeometry_;
    struct CachedBitmap {
        std::shared_ptr<const IconPixels> source;
        ComPtr<ID2D1Bitmap> bitmap;
    };
    std::unordered_map<std::wstring, CachedBitmap> bitmaps_;

    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ originalBitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace beam
