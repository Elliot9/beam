// Shared Windows includes, a minimal COM smart pointer and app-wide messages.
#pragma once

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <string>
#include <utility>

namespace beam {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(const ComPtr& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) {
            if (o.p_) o.p_->AddRef();
            Reset();
            p_ = o.p_;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            Reset();
            p_ = std::exchange(o.p_, nullptr);
        }
        return *this;
    }

    void Reset() {
        if (p_) {
            T* p = p_;
            p_ = nullptr;
            p->Release();
        }
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() {
        Reset();
        return &p_;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// RAII for HANDLEs from OpenProcess & co.
class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE h = nullptr) : h_(h) {}
    ~UniqueHandle() {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

// Messages between Beam's own windows and threads.
constexpr UINT WM_APP_SHOW = WM_APP + 1;          // host: second instance asks us to show
constexpr UINT WM_APP_TRAY = WM_APP + 2;          // host: notification-area callback
constexpr UINT WM_APP_INDEX_DONE = WM_APP + 3;    // host: lParam = std::vector<AppEntry>* (owned)
constexpr UINT WM_APP_ICONS = WM_APP + 4;         // launcher: icon(s) finished loading
constexpr UINT WM_APP_RENDER = WM_APP + 5;        // launcher: coalesced repaint
constexpr UINT WM_APP_RECORDING = WM_APP + 6;     // host: wParam != 0 while a hotkey box records
constexpr UINT WM_APP_DEFERRED = WM_APP + 7;      // host: run queued App::Defer callbacks
constexpr UINT WM_APP_PLUGIN_RESULT = WM_APP + 8; // launcher: lParam = PluginResult* (owned)

constexpr wchar_t kHostWindowClass[] = L"Beam.Host.5f1c";
constexpr wchar_t kLauncherWindowClass[] = L"Beam.Launcher.5f1c";
constexpr wchar_t kAppName[] = L"Beam";

}  // namespace beam
