// Pinned items in a user-defined order (%APPDATA%\Beam\favorites.json).
// Keys are the same usage keys as UsageStore ("app:...", "cmd:...", "ql:...", "plugin:...").
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace beam {

class FavoritesStore {
public:
    explicit FavoritesStore(std::filesystem::path file = {}) : file_(std::move(file)) {}

    void Load();
    bool Save() const;

    const std::vector<std::wstring>& Keys() const { return keys_; }
    bool Contains(const std::wstring& key) const { return IndexOf(key) >= 0; }
    int IndexOf(const std::wstring& key) const;
    bool Add(const std::wstring& key);     // appended at the end; false if already pinned
    bool Remove(const std::wstring& key);
    bool Toggle(const std::wstring& key);  // returns whether the key is pinned afterwards
    bool Move(const std::wstring& key, int delta);  // false when not pinned or already at the edge

private:
    std::filesystem::path file_;
    std::vector<std::wstring> keys_;
};

}  // namespace beam
