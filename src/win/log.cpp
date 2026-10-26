#include "win/log.hpp"

#include <cstdio>
#include <mutex>

#include "core/text.hpp"

namespace beam::log {
namespace {
std::mutex g_mutex;
std::wstring g_path;
constexpr LONGLONG kMaxBytes = 512 * 1024;
}  // namespace

void Init(const std::wstring& directory) {
    std::lock_guard lock(g_mutex);
    g_path = directory + L"\\beam.log";
}

void Write(const wchar_t* level, const std::wstring& message) {
    std::lock_guard lock(g_mutex);
    if (g_path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &info)) {
        const LONGLONG size = (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > kMaxBytes) {
            const std::wstring old = g_path + L".old";
            MoveFileExW(g_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    HANDLE file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[48];
    std::snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, t.wMilliseconds);
    std::string line = stamp + WideToUtf8(level) + " " + WideToUtf8(message) + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = len ? std::wstring(buffer, len) : L"Error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    return text;
}

std::wstring HResultText(HRESULT hr) {
    wchar_t code[16];
    std::swprintf(code, 16, L"0x%08X", static_cast<unsigned>(hr));
    return ErrorText(static_cast<DWORD>(hr)) + L" (" + code + L")";
}

}  // namespace beam::log
