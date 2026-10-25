#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <string>
#include <mutex>
#include <utility>

inline std::string Widen2Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
      static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
      result.data(), size, nullptr, nullptr);
  return result;
}

inline void LogInfo(const char* format, ...) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}
#define LogWarn LogInfo
inline void LogDebug(const char*, ...) {}

inline void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char code[32];
    sprintf_s(code, " (0x%08lX)", static_cast<unsigned long>(result));
    throw std::runtime_error(std::string(operation) + code);
  }
}
inline void Require(bool result, const char* operation) {
  if (!result) throw std::runtime_error(std::string(operation) +
      " (Windows " + std::to_string(GetLastError()) + ")");
}

struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  Handle& operator=(Handle&& other) noexcept {
    reset(std::exchange(other.value, nullptr)); return *this;
  }
  ~Handle() { reset(); }
  void reset(HANDLE next = nullptr) {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    value = next;
  }
};
