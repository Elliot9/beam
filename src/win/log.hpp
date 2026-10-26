#pragma once

#include <windows.h>

#include <string>

namespace beam::log {

void Init(const std::wstring& directory);  // %APPDATA%\Beam\beam.log
void Write(const wchar_t* level, const std::wstring& message);
inline void Info(const std::wstring& message) { Write(L"INFO", message); }
inline void Error(const std::wstring& message) { Write(L"ERROR", message); }

std::wstring ErrorText(DWORD error);
std::wstring HResultText(HRESULT hr);

}  // namespace beam::log
