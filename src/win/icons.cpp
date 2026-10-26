#include "win/icons.hpp"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/text.hpp"
#include "win/shell.hpp"

namespace beam {
namespace {

const IID kIidImageFactory = {0xbcc18b79, 0xba16, 0x442f, {0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b}};

std::shared_ptr<const IconPixels> FromHBitmap(HBITMAP bmp) {
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return nullptr;
    const int w = bm.bmWidth;
    const int h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    auto px = std::make_shared<IconPixels>();
    px->width = w;
    px->height = h;
    px->bgra.resize(static_cast<size_t>(w) * h * 4);
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), px->bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != h) return nullptr;

    bool anyAlpha = false;
    bool anyColor = false;
    bool straight = false;
    auto& d = px->bgra;
    for (size_t i = 0; i < d.size(); i += 4) {
        const uint8_t a = d[i + 3];
        if (a) anyAlpha = true;
        if (d[i] | d[i + 1] | d[i + 2]) anyColor = true;
        if (d[i] > a || d[i + 1] > a || d[i + 2] > a) straight = true;
    }
    if (!anyAlpha && !anyColor) return nullptr;  // blank bitmap: show the placeholder instead
    if (!anyAlpha) {
        for (size_t i = 0; i < d.size(); i += 4) d[i + 3] = 255;  // opaque 24-bit style icon
    } else if (straight) {
        for (size_t i = 0; i < d.size(); i += 4) {  // Direct2D wants premultiplied alpha
            const unsigned a = d[i + 3];
            for (int c = 0; c < 3; ++c) d[i + c] = static_cast<uint8_t>((d[i + c] * a + 127) / 255);
        }
    }
    return px;
}

bool IsImageFile(const std::wstring& path) {
    const std::wstring ext = ExtensionOf(path);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif" || ext == L".ico";
}

// Plugin icons such as "icon.png": decoded with WIC, scaled to fit and centred on a square canvas.
std::shared_ptr<const IconPixels> LoadImageFile(const std::wstring& path, int size) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                factory.PutVoid())))
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  decoder.Put())))
        return nullptr;
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0) return nullptr;
    // .ico files hold several sizes: take the smallest one that is at least `size`, else the largest.
    UINT best = 0, bestWidth = 0;
    for (UINT i = 0; i < frames && i < 32; ++i) {
        ComPtr<IWICBitmapFrameDecode> f;
        UINT w = 0, h = 0;
        if (FAILED(decoder->GetFrame(i, f.Put())) || FAILED(f->GetSize(&w, &h))) continue;
        const UINT want = static_cast<UINT>(size);
        const bool better = bestWidth == 0 || (bestWidth < want ? w > bestWidth : (w >= want && w < bestWidth));
        if (better) {
            best = i;
            bestWidth = w;
        }
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (FAILED(decoder->GetFrame(best, frame.Put())) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
        return nullptr;
    const double scale = std::min(static_cast<double>(size) / w, static_cast<double>(size) / h);
    const UINT tw = std::max<UINT>(1, static_cast<UINT>(std::lround(w * scale)));
    const UINT th = std::max<UINT>(1, static_cast<UINT>(std::lround(h * scale)));
    IWICBitmapSource* source = frame.Get();
    ComPtr<IWICBitmapScaler> scaler;
    if (tw != w || th != h) {
        if (FAILED(factory->CreateBitmapScaler(scaler.Put())) ||
            FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant)))
            return nullptr;
        source = scaler.Get();
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;
    std::vector<uint8_t> scaled(static_cast<size_t>(tw) * th * 4);
    if (FAILED(converter->CopyPixels(nullptr, tw * 4, static_cast<UINT>(scaled.size()), scaled.data()))) return nullptr;
    auto px = std::make_shared<IconPixels>();
    px->width = size;
    px->height = size;
    px->bgra.assign(static_cast<size_t>(size) * size * 4, 0);
    const UINT ox = (static_cast<UINT>(size) - tw) / 2;
    const UINT oy = (static_cast<UINT>(size) - th) / 2;
    for (UINT y = 0; y < th; ++y)
        std::memcpy(&px->bgra[((oy + y) * static_cast<size_t>(size) + ox) * 4], &scaled[static_cast<size_t>(y) * tw * 4],
                    static_cast<size_t>(tw) * 4);
    return px;
}

}  // namespace

void IconCache::Start(HWND notifyWindow, int pixelSize) {
    if (thread_.joinable()) return;
    notify_ = notifyWindow;
    size_ = pixelSize;
    stop_ = false;
    thread_ = std::thread([this] { Worker(); });
}

void IconCache::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::shared_ptr<const IconPixels> IconCache::Get(const std::wstring& path) {
    if (path.empty() || IsNetworkPath(path)) return nullptr;
    const std::wstring key = Fold(path);
    {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            Entry& e = it->second;
            if (e.state != State::Pending || e.urgent) return e.pixels;
            e.urgent = true;  // was only prefetched; jump the queue now that it is visible
        } else {
            entries_.emplace(key, Entry{State::Pending, true, nullptr});
        }
        urgent_.push_back(path);
    }
    cv_.notify_one();
    return nullptr;
}

void IconCache::Prefetch(const std::vector<std::wstring>& paths) {
    {
        std::lock_guard lock(mutex_);
        for (const auto& path : paths) {
            if (path.empty() || IsNetworkPath(path)) continue;
            if (entries_.emplace(Fold(path), Entry{}).second) background_.push_back(path);
        }
    }
    cv_.notify_one();
}

void IconCache::Worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::wstring path;
        bool urgent = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !urgent_.empty() || !background_.empty(); });
            if (stop_) break;
            urgent = !urgent_.empty();
            auto& queue = urgent ? urgent_ : background_;
            path = std::move(queue.front());
            queue.pop_front();
            if (entries_[Fold(path)].state != State::Pending) continue;  // queued twice, already done
        }
        auto pixels = Load(path);
        {
            std::lock_guard lock(mutex_);
            auto& e = entries_[Fold(path)];
            e.state = pixels ? State::Ready : State::Failed;
            e.pixels = std::move(pixels);
            urgent = e.urgent;
        }
        // Visible icons repaint right away; prefetched ones only matter once shown.
        if (urgent && !notifyPending_.exchange(true)) PostMessageW(notify_, WM_APP_ICONS, 0, 0);
    }
    CoUninitialize();
}

std::shared_ptr<const IconPixels> IconCache::Load(const std::wstring& path) const {
    if (IsImageFile(path)) {
        if (auto image = LoadImageFile(path, size_)) return image;
    }
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, kIidImageFactory, factory.PutVoid())))
        return nullptr;
    HBITMAP bmp = nullptr;
    const SIZE size{size_, size_};
    if (FAILED(factory->GetImage(size, SIIGBF_ICONONLY, &bmp)) || !bmp) return nullptr;
    auto pixels = FromHBitmap(bmp);
    DeleteObject(bmp);
    return pixels;
}

}  // namespace beam
