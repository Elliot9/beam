// Background icon loader. Icons come from IShellItemImageFactory, so .lnk,
// .exe, folders and packaged apps (shell:AppsFolder\...) all work.
#pragma once

#include "win/common.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace beam {

struct IconPixels {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // premultiplied, top-down
};

class IconCache {
public:
    ~IconCache() { Stop(); }

    void Start(HWND notifyWindow, int pixelSize);
    void Stop();

    // Loaded pixels, or nullptr while loading / when the icon is unavailable.
    // The first call for a path queues it; the notify window then receives WM_APP_ICONS.
    std::shared_ptr<const IconPixels> Get(const std::wstring& path);
    void Prefetch(const std::vector<std::wstring>& paths);
    void AcknowledgeNotification() { notifyPending_ = false; }

private:
    enum class State { Pending, Ready, Failed };
    struct Entry {
        State state = State::Pending;
        bool urgent = false;  // someone is waiting to draw it
        std::shared_ptr<const IconPixels> pixels;
    };

    void Worker();
    std::shared_ptr<const IconPixels> Load(const std::wstring& path) const;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::wstring, Entry> entries_;
    std::deque<std::wstring> urgent_;
    std::deque<std::wstring> background_;
    std::thread thread_;
    bool stop_ = false;
    HWND notify_ = nullptr;
    int size_ = 48;
    std::atomic<bool> notifyPending_{false};
};

}  // namespace beam
