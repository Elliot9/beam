#include "core/favorites.hpp"

#include <algorithm>
#include <utility>

#include "core/json.hpp"
#include "core/settings.hpp"
#include "core/text.hpp"

namespace beam {

void FavoritesStore::Load() {
    keys_.clear();
    if (file_.empty()) return;
    std::string text;
    if (!ReadTextFile(file_, text)) return;
    const auto json = Json::Parse(text);
    if (!json || !json->IsObject()) return;
    const Json* list = json->Find("favorites");
    if (!list || !list->IsArray()) return;
    for (const auto& item : list->Items()) {
        if (!item.IsString() || item.AsString().empty()) continue;
        Add(Utf8ToWide(item.AsString()));
    }
}

bool FavoritesStore::Save() const {
    if (file_.empty()) return false;
    Json list = Json::MakeArray();
    for (const auto& key : keys_) list.Push(Json(WideToUtf8(key)));
    Json root = Json::MakeObject();
    root.Set("favorites", std::move(list));
    return WriteTextFileAtomic(file_, root.Dump());
}

int FavoritesStore::IndexOf(const std::wstring& key) const {
    const auto it = std::find(keys_.begin(), keys_.end(), key);
    return it == keys_.end() ? -1 : static_cast<int>(it - keys_.begin());
}

bool FavoritesStore::Add(const std::wstring& key) {
    if (key.empty() || Contains(key)) return false;
    keys_.push_back(key);
    return true;
}

bool FavoritesStore::Remove(const std::wstring& key) {
    const int i = IndexOf(key);
    if (i < 0) return false;
    keys_.erase(keys_.begin() + i);
    return true;
}

bool FavoritesStore::Toggle(const std::wstring& key) {
    if (Remove(key)) return false;
    Add(key);
    return Contains(key);
}

bool FavoritesStore::Move(const std::wstring& key, int delta) {
    const int i = IndexOf(key);
    const int j = i + delta;
    if (i < 0 || delta == 0 || j < 0 || j >= static_cast<int>(keys_.size())) return false;
    std::wstring moved = std::move(keys_[i]);
    keys_.erase(keys_.begin() + i);
    keys_.insert(keys_.begin() + j, std::move(moved));
    return true;
}

}  // namespace beam
