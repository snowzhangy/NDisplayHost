// Power keep-awake (§4.2): PowerCreateRequest + BOTH display-required and
// system-required; display-required alone does not keep the system awake.
//
// Held by the owner service on request, and by the media worker for as long
// as a headset is connected: once the display idles off, Windows stops
// composing the desktop and the stream freezes on its last picture (Apple's
// host takes the equivalent display-sleep assertion for every session).
#pragma once

#include "wvd/platform.h"

namespace wvd {

class KeepAwake {
 public:
  bool Start(const wchar_t* reason, bool keepDisplay = true);
  void Stop();
  bool active() const { return h_ != nullptr; }

 private:
  HANDLE h_ = nullptr;
  bool displayReq_ = false;
};

}  // namespace wvd
