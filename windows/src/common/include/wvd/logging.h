// WVD logging: leveled, thread-safe, stderr + optional file.
// (ETW providers come with the driver; user-mode keeps it simple in P0.)
#pragma once

#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <string>

namespace wvd::log {

enum class Level : int { Error = 0, Warn = 1, Info = 2, Verbose = 3 };

void Init(const char* ident, Level level = Level::Info);
void SetLevel(Level level);
Level GetLevel();
// Optional mirror to file (append). Empty path disables.
void SetFile(const wchar_t* path);

void Write(Level lvl, const char* fmt, ...);
void WriteV(Level lvl, const char* fmt, va_list ap);

inline void E(const char* fmt, ...) { va_list ap; va_start(ap, fmt); WriteV(Level::Error, fmt, ap); va_end(ap); }
inline void W(const char* fmt, ...) { va_list ap; va_start(ap, fmt); WriteV(Level::Warn, fmt, ap); va_end(ap); }
inline void I(const char* fmt, ...) { va_list ap; va_start(ap, fmt); WriteV(Level::Info, fmt, ap); va_end(ap); }
inline void V(const char* fmt, ...) { va_list ap; va_start(ap, fmt); WriteV(Level::Verbose, fmt, ap); va_end(ap); }

}  // namespace wvd::log

#ifndef WVD_LOGE
#define WVD_LOGE(...) ::wvd::log::E(__VA_ARGS__)
#define WVD_LOGW(...) ::wvd::log::W(__VA_ARGS__)
#define WVD_LOGI(...) ::wvd::log::I(__VA_ARGS__)
#define WVD_LOGV(...) ::wvd::log::V(__VA_ARGS__)
#endif

// Log-and-return helper for HRESULT-style functions.
#define WVD_RETURN_ON_FAIL(expr, ...)                                        \
  do {                                                                       \
    HRESULT hr_ = (expr);                                                    \
    if (FAILED(hr_)) {                                                       \
      WVD_LOGE("%s failed: hr=0x%08lX", #expr, static_cast<unsigned long>(hr_));                          \
      return hr_;                                                            \
    }                                                                        \
  } while (0)
