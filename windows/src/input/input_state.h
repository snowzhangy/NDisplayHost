// Input injector + desktop attach + input generations (§4.4).
//
// The injector thread is the ONLY thread that calls SendInput; it is bound
// to the current INPUT desktop (OpenInputDesktop + SetThreadDesktop) and is
// re-bound on every desktop switch. All held state is RELEASED before an
// input generation changes, and events from older generations are rejected.
#pragma once

#include <map>
#include <mutex>

#include "wvd/protocol.h"

namespace wvd {

// Maps a USB HID usage (key) to a PS/2 set-1 scan code + E0 prefix.
// Unmapped usages are dropped; HID usages are never treated as scan codes.
struct ScanCode {
  uint16_t code = 0;
  bool extended = false;
  bool valid = false;
};
ScanCode HidUsageToScanCode(uint16_t usage);

class InputInjector {
 public:
  using SendFn = UINT(WINAPI*)(UINT, LPINPUT, int);
  explicit InputInjector(SendFn send = ::SendInput) : send_(send) {}
  // Mouse/keyboard via SendInput (scan codes; mouse absolute VIRTUALDESK).
  void InjectEvent(const InputEvent& ev);
  void ReleaseAllHeld();  // key-ups, button-ups, touch cancels (§4.4)

  size_t heldCount() const;

 private:
  void KeyDown(uint16_t usage);
  void KeyUp(uint16_t usage);
  void Button(bool down, uint16_t bits, int32_t xNorm, int32_t yNorm);
  void Wheel(int32_t delta);

  mutable std::mutex m_;
  std::map<uint16_t, ScanCode> heldKeys_;
  uint16_t heldButtons_ = 0;
  SendFn send_;
};

// Desktop tracker: keeps a worker thread attached to the input desktop and
// reports switches (UAC, lock, user switch) so the gate can release state.
class DesktopTracker {
 public:
  using SwitchCb = void (*)();

  DesktopTracker();
  ~DesktopTracker();
  // Binds the CURRENT thread to the input desktop; call on the injector
  // thread itself (SetThreadDesktop only works for the calling thread and
  // fails if it owns windows/hooks — the injector thread owns none).
  bool AttachCurrentThreadToInputDesktop(std::wstring& name, bool& secure);
  // Watch on a SEPARATE notification thread. Hooks/windows prevent a thread
  // from switching desktops. onSwitch must only signal the injector thread.
  bool StartWatchingOnCurrentThread(SwitchCb onSwitch);
  const std::wstring& desktopName() const { return name_; }
  bool secure() const { return secure_; }

 private:
  std::wstring name_;
  bool secure_ = false;
  void* hook_ = nullptr;
  HDESK desktop_ = nullptr;
  HDESK originalDesktop_ = nullptr;
};

// Input gate: armed state + generations + stuck-input bookkeeping (§4.4/§4.7).
class InputGate {
 public:
  enum class State { Disarmed, Armed };

  // Caller serializes all gate access with the input queue/state mutex.
  bool Accept(uint64_t incomingGen);
  void Arm();   // consent granted (tray)
  void Disarm();// kill switch / idle timeout / network change
  void Suspend(bool suspend) { suspended_ = suspend; }
  bool suspended() const { return suspended_; }
  State state() const { return state_; }
  uint64_t generation() const { return gen_; }

 private:
  State state_ = State::Disarmed;
  uint64_t gen_ = 1;
  bool suspended_ = true;
};

}  // namespace wvd
