#include "core/usage.hpp"

#include <algorithm>
#include <cmath>

#include "core/json.hpp"
#include "core/settings.hpp"
#include "core/text.hpp"

namespace beam {
namespace {
constexpr size_t kMaxItems = 2000;
constexpr size_t kMaxQueryMemory = 1000;
}  // namespace

std::wstring UsageStore::NormalizeQuery(std::wstring_view query) {
    std::wstring q = Fold(Trim(query));
    if (q.size() > 32) q.resize(32);
    return q;
}

void UsageStore::Record(const std::wstring& key, std::wstring_view query, int64_t nowUnix) {
    if (key.empty()) return;
    auto& e = items_[key];
    e.count += 1;
    e.last = nowUnix;
    const std::wstring q = NormalizeQuery(query);
    if (!q.empty()) queryMemory_[q] = key;
    Prune();
}

double UsageStore::Boost(const std::wstring& key, std::wstring_view query, int64_t nowUnix) const {
    double boost = 0;
    if (const auto it = items_.find(key); it != items_.end()) {
        boost += std::min(120.0, 35.0 * std::log(1.0 + it->second.count));
        const int64_t age = nowUnix - it->second.last;
        if (age < 3600) boost += 40;
        else if (age < 86400) boost += 25;
        else if (age < 7 * 86400) boost += 10;
    }
    const std::wstring q = NormalizeQuery(query);
    if (!q.empty()) {
        if (const auto it = queryMemory_.find(q); it != queryMemory_.end() && it->second == key) boost += 250;
    }
    return boost;
}

std::vector<std::wstring> UsageStore::RecentKeys(size_t max) const {
    std::vector<std::pair<int64_t, const std::wstring*>> order;
    order.reserve(items_.size());
    for (const auto& [k, e] : items_) order.emplace_back(e.last, &k);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : *a.second < *b.second;
    });
    std::vector<std::wstring> keys;
    for (size_t i = 0; i < order.size() && keys.size() < max; ++i) keys.push_back(*order[i].second);
    return keys;
}

void UsageStore::Remove(const std::wstring& key) {
    items_.erase(key);
    for (auto it = queryMemory_.begin(); it != queryMemory_.end();) {
        if (it->second == key) it = queryMemory_.erase(it);
        else ++it;
    }
}

void UsageStore::Prune() {
    if (items_.size() > kMaxItems) {
        std::vector<std::pair<int64_t, std::wstring>> order;
        for (const auto& [k, e] : items_) order.emplace_back(e.last, k);
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size() - kMaxItems; ++i) items_.erase(order[i].second);
    }
    if (queryMemory_.size() > kMaxQueryMemory) {
        // Drop memories whose target is least recently used.
        std::vector<std::pair<int64_t, std::wstring>> order;
        for (const auto& [q, k] : queryMemory_) {
            const auto it = items_.find(k);
            order.emplace_back(it == items_.end() ? 0 : it->second.last, q);
        }
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size() - kMaxQueryMemory; ++i) queryMemory_.erase(order[i].second);
    }
}

void UsageStore::Load() {
    items_.clear();
    queryMemory_.clear();
    if (file_.empty()) return;
    std::string text;
    if (!ReadTextFile(file_, text)) return;
    const auto json = Json::Parse(text);
    if (!json || !json->IsObject()) return;
    if (const Json* items = json->Find("items"); items && items->IsObject()) {
        for (const auto& [k, v] : items->Members()) {
            if (!v.IsObject()) continue;
            Entry e;
            if (const Json* c = v.Find("count")) e.count = static_cast<int>(c->AsNumber());
            if (const Json* l = v.Find("last")) e.last = static_cast<int64_t>(l->AsNumber());
            if (e.count > 0) items_[Utf8ToWide(k)] = e;
        }
    }
    if (const Json* mem = json->Find("queryMemory"); mem && mem->IsObject()) {
        for (const auto& [q, k] : mem->Members())
            if (k.IsString()) queryMemory_[Utf8ToWide(q)] = Utf8ToWide(k.AsString());
    }
}

bool UsageStore::Save() const {
    if (file_.empty()) return false;
    Json root = Json::MakeObject();
    Json items = Json::MakeObject();
    // Stable output order makes the file diff-friendly.
    std::vector<const std::wstring*> keys;
    for (const auto& [k, e] : items_) keys.push_back(&k);
    std::sort(keys.begin(), keys.end(), [](auto a, auto b) { return *a < *b; });
    for (const auto* k : keys) {
        const Entry& e = items_.at(*k);
        Json o = Json::MakeObject();
        o.Set("count", e.count);
        o.Set("last", static_cast<double>(e.last));
        items.Set(WideToUtf8(*k), std::move(o));
    }
    root.Set("items", std::move(items));
    Json mem = Json::MakeObject();
    std::vector<const std::wstring*> qs;
    for (const auto& [q, k] : queryMemory_) qs.push_back(&q);
    std::sort(qs.begin(), qs.end(), [](auto a, auto b) { return *a < *b; });
    for (const auto* q : qs) mem.Set(WideToUtf8(*q), Json(WideToUtf8(queryMemory_.at(*q))));
    root.Set("queryMemory", std::move(mem));
    return WriteTextFileAtomic(file_, root.Dump());
}

}  // namespace beam
