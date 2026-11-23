#pragma once

#include "wire1_channel.h"

namespace wvd {

// This worker owns the message window and polls it alongside the listener.
class WorkstationWatcher {
 public:
  ~WorkstationWatcher();
  void Start();
  WorkstationState Poll();

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  HWND window_ = nullptr;
  bool locked_ = false;
};

}  // namespace wvd
