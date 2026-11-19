// Product identity of the Windows display host, for logs and diagnostics.
// The numbers come from the WVM_HOST_* variables in windows/CMakeLists.txt;
// the fallbacks keep this header usable in the separate WDK driver build.
#pragma once

#include <string>

#define WVM_STRINGIFY2(x) #x
#define WVM_STRINGIFY(x) WVM_STRINGIFY2(x)

#ifndef WVM_HOST_VERSION_MAJOR
#define WVM_HOST_VERSION_MAJOR 0
#endif
#ifndef WVM_HOST_VERSION_MINOR
#define WVM_HOST_VERSION_MINOR 0
#endif
#ifndef WVM_HOST_VERSION_PATCH
#define WVM_HOST_VERSION_PATCH 0
#endif
#ifndef WVM_HOST_BUILD
#define WVM_HOST_BUILD 0
#endif

namespace wvd {

// Product name the user sees (window title, tray tooltip, docs).
inline constexpr const char* kHostProduct = "NDisplayHostWin";

// "1.0.0 (build 1)" — log this once per process start so any captured log or
// screenshot says which host build produced it.
inline const std::string& HostVersion() {
  static const std::string version =
      std::string(WVM_STRINGIFY(WVM_HOST_VERSION_MAJOR) "."
                  WVM_STRINGIFY(WVM_HOST_VERSION_MINOR) "."
                  WVM_STRINGIFY(WVM_HOST_VERSION_PATCH)) +
      " (build " WVM_STRINGIFY(WVM_HOST_BUILD) ")";
  return version;
}

}  // namespace wvd
