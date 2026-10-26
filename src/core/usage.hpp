// Learns what you open so ranking adapts (frequency, recency, and which item
// you picked for a given query - "c" -> Chrome after you choose it once).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace beam {

class UsageStore {
public:
    explicit UsageStore(std::filesystem::path file = {}) : file_(std::move(file)) {}

    void Load();
    bool Save() const;

    void Record(const std::wstring& key, std::wstring_view query, int64_t nowUnix);
    double Boost(const std::wstring& key, std::wstring_view query, int64_t nowUnix) const;
    std::vector<std::wstring> RecentKeys(size_t max) const;
    void Remove(const std::wstring& key);
    size_t size() const { return items_.size(); }

    static std::wstring NormalizeQuery(std::wstring_view query);

private:
    struct Entry {
        int count = 0;
        int64_t last = 0;
    };
    void Prune();

    std::filesystem::path file_;
    std::unordered_map<std::wstring, Entry> items_;
    std::unordered_map<std::wstring, std::wstring> queryMemory_;
};

}  // namespace beam
