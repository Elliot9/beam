#include "win/renderer.hpp"

#include <algorithm>

#include "win/log.hpp"
#include "win/shell.hpp"

namespace beam {
namespace {

D2D1_COLOR_F ToColor(uint32_t argb) {
    return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                        ((argb >> 24) & 0xFF) / 255.0f);
}

}  // namespace

Palette MakePalette(bool dark) {
    Palette p;
    p.dark = dark;
    if (dark) {
        p.background = 0xFF1E1E21;
        p.footer = 0xFF232327;
        p.border = 0xFF3A3A41;
        p.separator = 0xFF2E2E34;
        p.text = 0xFFEDEDEF;
        p.textSecondary = 0xFF9B9BA4;
        p.textTertiary = 0xFF6F6F79;
        p.selection = 0xFF34343B;
        p.panel = 0xFF27272C;
        p.panelBorder = 0xFF3F3F47;
        p.keycap = 0xFF34343B;
        p.keycapText = 0xFFB4B4BD;
        p.chip = 0xFF3A3A42;
        p.chipText = 0xFFEDEDEF;
        p.warning = 0xFFFF9F43;
        p.accent = 0xFFFF5A5F;
        p.textSelection = 0x804C8DFF;
        p.caret = 0xFFEDEDEF;
        p.shadow = 0xFF000000;
    } else {
        p.background = 0xFFFBFBFC;
        p.footer = 0xFFF2F2F5;
        p.border = 0xFFD5D5DC;
        p.separator = 0xFFE6E6EB;
        p.text = 0xFF1D1D1F;
        p.textSecondary = 0xFF696972;
        p.textTertiary = 0xFF9B9BA4;
        p.selection = 0xFFE7E7EC;
        p.panel = 0xFFFFFFFF;
        p.panelBorder = 0xFFD5D5DC;
        p.keycap = 0xFFE6E6EB;
        p.keycapText = 0xFF55555E;
        p.chip = 0xFFE7E7EC;
        p.chipText = 0xFF1D1D1F;
        p.warning = 0xFFD9480F;
        p.accent = 0xFFE5484D;
        p.textSelection = 0x662F6FEB;
        p.caret = 0xFF1D1D1F;
        p.shadow = 0xFF000000;
    }
    return p;
}

Palette ResolvePalette(const std::wstring& theme) {
    if (theme == L"dark") return MakePalette(true);
    if (theme == L"light") return MakePalette(false);
    return MakePalette(!SystemUsesLightTheme());
}

Renderer::~Renderer() {
    ReleaseDeviceResources();
    if (memDC_) {
        if (originalBitmap_) SelectObject(memDC_, originalBitmap_);
        DeleteDC(memDC_);
    }
    if (dib_) DeleteObject(dib_);
}

bool Renderer::Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                   factory_.PutVoid());
    if (FAILED(hr)) {
        log::Error(L"D2D1CreateFactory: " + log::HResultText(hr));
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_.Put()));
    if (FAILED(hr)) {
        log::Error(L"DWriteCreateFactory: " + log::HResultText(hr));
        return false;
    }

    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-us";
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    struct Spec {
        Font font;
        const wchar_t* family;
        DWRITE_FONT_WEIGHT weight;
        float size;
    };
    const Spec specs[] = {
        {Font::Input, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 18.0f},
        {Font::Title, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 14.0f},
        {Font::Subtitle, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 13.0f},
        {Font::Small, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 12.0f},
        {Font::SmallBold, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f},
        {Font::Keycap, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f},
        {Font::Glyph, L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 13.0f},
        {Font::GlyphLarge, L"Segoe MDL2 Assets", DWRITE_FONT_WEIGHT_NORMAL, 16.0f},
        {Font::Brand, L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 10.0f},
        {Font::Mono, L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 12.5f},
    };
    for (const auto& s : specs) {
        auto& fmt = formats_[static_cast<size_t>(s.font)];
        hr = dwrite_->CreateTextFormat(s.family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, s.size, locale, fmt.Put());
        if (FAILED(hr)) {
            log::Error(L"CreateTextFormat: " + log::HResultText(hr));
            return false;
        }
        if (s.font == Font::Mono) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            fmt->SetIncrementalTabStop(4 * 7.0f);  // ~4 Consolas columns at 12.5 DIP
            continue;
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(fmt.Get(), ellipsis.Put()))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt->SetTrimming(&trimming, ellipsis.Get());
        }
    }
    return true;
}

bool Renderer::EnsureSurface(int width, int height) {
    if (dib_ && width == width_ && height == height_) return true;
    if (!memDC_) memDC_ = CreateCompatibleDC(nullptr);
    if (!memDC_) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return false;
    HGDIOBJ previous = SelectObject(memDC_, dib);
    if (!originalBitmap_) originalBitmap_ = previous;
    if (dib_) DeleteObject(dib_);
    dib_ = dib;
    width_ = width;
    height_ = height;
    return true;
}

void Renderer::ReleaseDeviceResources() {
    bitmaps_.clear();
    clipGeometry_.Reset();
    layer_.Reset();
    brush_.Reset();
    rt_.Reset();
}

bool Renderer::Begin(int pixelWidth, int pixelHeight, float dpi) {
    if (!factory_ || !EnsureSurface(pixelWidth, pixelHeight)) return false;
    if (!rt_) {
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0,
            D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        HRESULT hr = factory_->CreateDCRenderTarget(&props, rt_.Put());
        if (FAILED(hr)) {
            log::Error(L"CreateDCRenderTarget: " + log::HResultText(hr));
            return false;
        }
        hr = rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.Put());
        if (FAILED(hr)) {
            ReleaseDeviceResources();
            return false;
        }
    }
    const RECT rc{0, 0, pixelWidth, pixelHeight};
    if (FAILED(rt_->BindDC(memDC_, &rc))) {
        ReleaseDeviceResources();
        return false;
    }
    rt_->SetDpi(dpi, dpi);
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs an opaque target
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return true;
}

bool Renderer::End(HWND hwnd, POINT screenPos) {
    const HRESULT hr = rt_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        ReleaseDeviceResources();
        return false;
    }
    if (FAILED(hr)) {
        log::Error(L"EndDraw: " + log::HResultText(hr));
        return false;
    }
    SIZE size{width_, height_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    const BOOL ok = UpdateLayeredWindow(hwnd, screen, &screenPos, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

ID2D1SolidColorBrush* Renderer::Brush(uint32_t argb) {
    brush_->SetColor(ToColor(argb));
    return brush_.Get();
}

void Renderer::FillRect(const D2D1_RECT_F& r, uint32_t argb) { rt_->FillRectangle(r, Brush(argb)); }

void Renderer::FillRoundRect(const D2D1_RECT_F& r, float radius, uint32_t argb) {
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(argb));
}

void Renderer::StrokeRoundRect(const D2D1_RECT_F& r, float radius, uint32_t argb, float width) {
    const float h = width / 2;
    const D2D1_RECT_F inset{r.left + h, r.top + h, r.right - h, r.bottom - h};
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(inset, radius, radius), Brush(argb), width);
}

void Renderer::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t argb, float width) {
    rt_->DrawLine(a, b, Brush(argb), width);
}

void Renderer::Text(std::wstring_view text, const D2D1_RECT_F& r, Font font, uint32_t argb,
                    DWRITE_TEXT_ALIGNMENT align) {
    if (text.empty() || r.right <= r.left) return;
    IDWriteTextFormat* fmt = Format(font);
    fmt->SetTextAlignment(align);
    rt_->DrawText(text.data(), static_cast<UINT32>(text.size()), fmt, r, Brush(argb),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

ComPtr<IDWriteTextLayout> Renderer::Layout(std::wstring_view text, Font font, float maxWidth, float maxHeight) const {
    ComPtr<IDWriteTextLayout> layout;
    IDWriteTextFormat* fmt = Format(font);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    dwrite_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, std::max(0.0f, maxWidth),
                              std::max(0.0f, maxHeight), layout.Put());
    return layout;
}

float Renderer::TextWidth(std::wstring_view text, Font font) const {
    if (text.empty()) return 0;
    auto layout = Layout(text, font, 10000.0f, 100.0f);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

void Renderer::DrawIcon(const std::wstring& key, const std::shared_ptr<const IconPixels>& pixels,
                        const D2D1_RECT_F& dest) {
    if (!pixels || pixels->width <= 0) return;
    auto& cached = bitmaps_[key];
    if (cached.source != pixels || !cached.bitmap) {
        cached.bitmap.Reset();
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(rt_->CreateBitmap(D2D1::SizeU(pixels->width, pixels->height), pixels->bgra.data(),
                                     static_cast<UINT32>(pixels->width * 4), props, cached.bitmap.Put()))) {
            return;
        }
        cached.source = pixels;
    }
    rt_->DrawBitmap(cached.bitmap.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Renderer::Shadow(const D2D1_RECT_F& r, float radius, float spread, uint32_t argb) {
    // Stacked translucent rounded rects approximate a blur without D2D 1.1 effects.
    const int steps = 12;
    const float baseAlpha = ((argb >> 24) & 0xFF) / 255.0f;
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / steps;
        const float grow = spread * t;
        const float alpha = baseAlpha * (1.0f - t) * (1.0f - t) * 0.22f;
        const D2D1_RECT_F rr{r.left - grow, r.top - grow * 0.6f, r.right + grow, r.bottom + grow * 1.2f};
        const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        FillRoundRect(rr, radius + grow, (a << 24) | (argb & 0x00FFFFFF));
    }
}

void Renderer::PushRoundClip(const D2D1_RECT_F& r, float radius) {
    clipGeometry_.Reset();
    factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), clipGeometry_.Put());
    if (!layer_) rt_->CreateLayer(nullptr, layer_.Put());
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeometry_.Get()), layer_.Get());
}

void Renderer::PopRoundClip() { rt_->PopLayer(); }

void Renderer::PushClip(const D2D1_RECT_F& r) { rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }

void Renderer::PopClip() { rt_->PopAxisAlignedClip(); }

}  // namespace beam
