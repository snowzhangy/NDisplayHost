#include "input_state.h"

#include <algorithm>
#include <wtsapi32.h>

#include "wvd/logging.h"

#pragma comment(lib, "user32.lib")

namespace wvd {

// ---------------------------------------------------------------------------
// HID usage → PS/2 set-1 scan code
// ---------------------------------------------------------------------------

// HID Keyboard/Keypad usage IDs are NOT PS/2 set-1 scan codes. Treating them
// as identical typed the wrong characters (usage 0x04 = 'a' went out as set-1
// 0x04 = '3'; 0x28 = Return went out as '''; 0x2C = Space as 'z'; the arrows
// landed on End/Insert). Below is the real translation table, with the E0
// extension carried in ScanCode::extended (SendInput's KEYEVENTF_EXTENDEDKEY).
namespace {

struct ScanEntry {
  uint16_t usage;  // HID Keyboard/Keypad usage ID
  uint16_t code;   // PS/2 set-1 make code
  uint8_t ext;     // E0-prefixed (KEYEVENTF_EXTENDEDKEY)
  uint8_t keypad;  // keypad digit: extension depends on the NumLock state
};

// HID usage -> PS/2 set-1. Every row states its usage explicitly so the table
// can be audited line by line. Usages with no safe AT equivalent (PrintScreen,
// Pause, F13+, consumer/media keys) are deliberately absent: HidUsageToScanCode
// reports them unmapped and the caller drops the event rather than typing a
// random key.
const ScanEntry kHidToSet1[] = {
    {0x04, 0x1E, 0, 0},  // a
    {0x05, 0x30, 0, 0},  // b
    {0x06, 0x2E, 0, 0},  // c
    {0x07, 0x20, 0, 0},  // d
    {0x08, 0x12, 0, 0},  // e
    {0x09, 0x21, 0, 0},  // f
    {0x0A, 0x22, 0, 0},  // g
    {0x0B, 0x23, 0, 0},  // h
    {0x0C, 0x17, 0, 0},  // i
    {0x0D, 0x24, 0, 0},  // j
    {0x0E, 0x25, 0, 0},  // k
    {0x0F, 0x26, 0, 0},  // l
    {0x10, 0x32, 0, 0},  // m
    {0x11, 0x31, 0, 0},  // n
    {0x12, 0x18, 0, 0},  // o
    {0x13, 0x19, 0, 0},  // p
    {0x14, 0x10, 0, 0},  // q
    {0x15, 0x13, 0, 0},  // r
    {0x16, 0x1F, 0, 0},  // s
    {0x17, 0x14, 0, 0},  // t
    {0x18, 0x16, 0, 0},  // u
    {0x19, 0x2F, 0, 0},  // v
    {0x1A, 0x11, 0, 0},  // w
    {0x1B, 0x2D, 0, 0},  // x
    {0x1C, 0x15, 0, 0},  // y
    {0x1D, 0x2C, 0, 0},  // z
    {0x1E, 0x02, 0, 0},  // 1
    {0x1F, 0x03, 0, 0},  // 2
    {0x20, 0x04, 0, 0},  // 3
    {0x21, 0x05, 0, 0},  // 4
    {0x22, 0x06, 0, 0},  // 5
    {0x23, 0x07, 0, 0},  // 6
    {0x24, 0x08, 0, 0},  // 7
    {0x25, 0x09, 0, 0},  // 8
    {0x26, 0x0A, 0, 0},  // 9
    {0x27, 0x0B, 0, 0},  // 0
    {0x28, 0x1C, 0, 0},  // Return
    {0x29, 0x01, 0, 0},  // Escape
    {0x2A, 0x0E, 0, 0},  // Backspace
    {0x2B, 0x0F, 0, 0},  // Tab
    {0x2C, 0x39, 0, 0},  // Space
    {0x2D, 0x0C, 0, 0},  // -
    {0x2E, 0x0D, 0, 0},  // =
    {0x2F, 0x1A, 0, 0},  // [
    {0x30, 0x1B, 0, 0},  // ]
    {0x31, 0x2B, 0, 0},  // backslash US
    {0x32, 0x56, 0, 0},  // non-US grave/pipe
    {0x33, 0x27, 0, 0},  // ;
    {0x34, 0x28, 0, 0},  // quote
    {0x35, 0x29, 0, 0},  // grave
    {0x36, 0x33, 0, 0},  // ,
    {0x37, 0x34, 0, 0},  // .
    {0x38, 0x35, 0, 0},  // slash
    {0x39, 0x3A, 0, 0},  // CapsLock
    {0x3A, 0x3B, 0, 0},  // F1
    {0x3B, 0x3C, 0, 0},  // F2
    {0x3C, 0x3D, 0, 0},  // F3
    {0x3D, 0x3E, 0, 0},  // F4
    {0x3E, 0x3F, 0, 0},  // F5
    {0x3F, 0x40, 0, 0},  // F6
    {0x40, 0x41, 0, 0},  // F7
    {0x41, 0x42, 0, 0},  // F8
    {0x42, 0x43, 0, 0},  // F9
    {0x43, 0x44, 0, 0},  // F10
    {0x44, 0x45, 0, 0},  // F11
    {0x45, 0x46, 0, 0},  // F12
    {0x47, 0x46, 0, 0},  // ScrollLock
    {0x49, 0x52, 1, 0},  // Insert
    {0x4A, 0x47, 1, 0},  // Home
    {0x4B, 0x49, 1, 0},  // PageUp
    {0x4C, 0x53, 1, 0},  // Delete
    {0x4D, 0x4F, 1, 0},  // End
    {0x4E, 0x51, 1, 0},  // PageDown
    {0x4F, 0x4D, 1, 0},  // RightArrow
    {0x50, 0x4B, 1, 0},  // LeftArrow
    {0x51, 0x50, 1, 0},  // DownArrow
    {0x52, 0x48, 1, 0},  // UpArrow
    {0x53, 0x45, 1, 0},  // NumLock
    {0x54, 0x35, 1, 0},  // KP_Divide
    {0x55, 0x37, 0, 0},  // KP_Multiply
    {0x56, 0x4A, 0, 0},  // KP_Subtract
    {0x57, 0x4E, 0, 0},  // KP_Add
    {0x58, 0x1C, 1, 0},  // KP_Enter
    {0x59, 0x4F, 0, 1},  // KP_1
    {0x5A, 0x50, 0, 1},  // KP_2
    {0x5B, 0x51, 0, 1},  // KP_3
    {0x5C, 0x4B, 0, 1},  // KP_4
    {0x5D, 0x4C, 0, 1},  // KP_5
    {0x5E, 0x4D, 0, 1},  // KP_6
    {0x5F, 0x47, 0, 1},  // KP_7
    {0x60, 0x48, 0, 1},  // KP_8
    {0x61, 0x49, 0, 1},  // KP_9
    {0x62, 0x52, 0, 1},  // KP_0
    {0x63, 0x53, 0, 1},  // KP_Decimal
    {0x64, 0x56, 0, 0},  // non-US backslash
    {0x65, 0x5D, 1, 0},  // Applications
    {0xE0, 0x1D, 0, 0},  // LCtrl
    {0xE1, 0x2A, 0, 0},  // LShift
    {0xE2, 0x38, 0, 0},  // LAlt
    {0xE3, 0x5B, 1, 0},  // LGUI
    {0xE4, 0x1D, 1, 0},  // RCtrl
    {0xE5, 0x36, 0, 0},  // RShift
    {0xE6, 0x38, 1, 0},  // RAlt
    {0xE7, 0x5C, 1, 0},  // RGUI
};

}  // namespace

ScanCode HidUsageToScanCode(uint16_t usage) {
  ScanCode s;
  const ScanEntry* hit = nullptr;
  for (const auto& e : kHidToSet1) {
    if (e.usage == usage) {
      hit = &e;
      break;
    }
  }
  if (!hit) return s;  // unmapped → dropped, never approximated

  s.code = hit->code;
  s.extended = hit->ext != 0;

  // Keypad digits/decimal emit E0-prefixed navigation codes when NumLock is
  // off and plain keypad codes when it is on — exactly as a real AT keyboard
  // does. The HID usage identifies the physical key only.
  if (hit->keypad) s.extended = (GetKeyState(VK_NUMLOCK) & 1) == FALSE;
  s.valid = true;
  return s;
}

// ---------------------------------------------------------------------------
// InputInjector
// ---------------------------------------------------------------------------

static void NormalizeDesktop(int32_t x, int32_t y, LONG& nx, LONG& ny) {
  // SendInput MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK: 0..65535
  // across the union of monitors (negative origins handled).
  int vl = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  nx = LONG(std::clamp<int64_t>((int64_t(x) - vl) * 65535 / (std::max)(vw - 1, 1), 0, 65535));
  ny = LONG(std::clamp<int64_t>((int64_t(y) - vt) * 65535 / (std::max)(vh - 1, 1), 0, 65535));
  if (nx < 0) nx = 0;
  if (ny < 0) ny = 0;
  if (nx > 65535) nx = 65535;
  if (ny > 65535) ny = 65535;
}

void InputInjector::InjectEvent(const InputEvent& ev) {
  switch (InputKind(ev.kind)) {
    case InputKind::Key: {
      if (ev.flags & kKeyUp) KeyUp(ev.code);
      else KeyDown(ev.code);
      break;
    }
    case InputKind::Button:
    case InputKind::MoveAbs: {
      LONG nx, ny;
      NormalizeDesktop(ev.x, ev.y, nx, ny);
      if (InputKind(ev.kind) == InputKind::MoveAbs) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = nx;
        in.mi.dy = ny;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                        MOUSEEVENTF_VIRTUALDESK;
        send_(1, &in, sizeof(in));
      } else {
        Button((ev.flags & kKeyUp) == 0, ev.code, nx, ny);
      }
      break;
    }
    case InputKind::Wheel:
      Wheel(ev.wheelDelta);
      break;
    case InputKind::Touch:
    case InputKind::Pen:
      // Opt-in synthetic pointer injection (§4.4): mouse mode is the
      // default pinch mapping; touch path lands with the P0 input spike.
      WVD_LOGV("touch/pen injection not enabled in P0 (mouse default)");
      break;
  }
}

void InputInjector::KeyDown(uint16_t usage) {
  std::lock_guard<std::mutex> lk(m_);
  ScanCode sc = HidUsageToScanCode(usage);
  if (!sc.valid) {
    // Fallback: unicode packet path so text never dead-drops for mapped
    // characters. VK_PACKET with KEYEVENTF_UNICODE is sent by the media
    // worker only when it provided a UTF-16 unit in `code`.
    WVD_LOGV("unmapped usage 0x%X — dropped", usage);
    return;
  }
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wScan = sc.code;
  in.ki.dwFlags = KEYEVENTF_SCANCODE | (sc.extended ? KEYEVENTF_EXTENDEDKEY : 0);
  if (send_(1, &in, sizeof(in)) == 1) heldKeys_[usage] = sc;
}

void InputInjector::KeyUp(uint16_t usage) {
  std::lock_guard<std::mutex> lk(m_);
  auto held = heldKeys_.find(usage);
  ScanCode sc = held != heldKeys_.end() ? held->second : HidUsageToScanCode(usage);
  if (!sc.valid) return;
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wScan = sc.code;
  in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP |
                  (sc.extended ? KEYEVENTF_EXTENDEDKEY : 0);
  if (send_(1, &in, sizeof(in)) == 1) heldKeys_.erase(usage);
}

void InputInjector::Button(bool down, uint16_t bits, int32_t xNorm, int32_t yNorm) {
  DWORD evd = 0;
  uint16_t bit = bits & 7;
  if (bit & 1) evd |= down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
  if (bit & 2) evd |= down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
  if (bit & 4) evd |= down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
  if (!evd) return;

  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = xNorm;
  in.mi.dy = yNorm;
  in.mi.dwFlags = evd | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  std::lock_guard<std::mutex> lk(m_);
  if (send_(1, &in, sizeof(in)) != 1) return;
  if (down) heldButtons_ |= bit;
  else heldButtons_ &= ~bit;
}

void InputInjector::Wheel(int32_t delta) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.mouseData = DWORD(delta);
  in.mi.dwFlags = MOUSEEVENTF_WHEEL;
  send_(1, &in, sizeof(in));
}

void InputInjector::ReleaseAllHeld() {
  std::lock_guard<std::mutex> lk(m_);
  // Keys first (modifiers last-to-first order matters little for up events).
  const auto keys = heldKeys_;
  uint16_t buttons = heldButtons_;
  for (const auto& key : keys) {
    ScanCode sc = key.second;
    if (!sc.valid) continue;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = sc.code;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP |
                    (sc.extended ? KEYEVENTF_EXTENDEDKEY : 0);
    if (send_(1, &in, sizeof(in)) == 1) heldKeys_.erase(key.first);
  }
  static const struct {
    uint16_t bit;
    DWORD upEv;
  } kUps[] = {{1, MOUSEEVENTF_LEFTUP}, {2, MOUSEEVENTF_RIGHTUP}, {4, MOUSEEVENTF_MIDDLEUP}};
  for (auto& u : kUps) {
    if (buttons & u.bit) {
      INPUT in{};
      in.type = INPUT_MOUSE;
      in.mi.dwFlags = u.upEv;  // no move: release where it is
      if (send_(1, &in, sizeof(in)) == 1) heldButtons_ &= ~u.bit;
    }
  }
  if (!keys.empty() || buttons)
    WVD_LOGI("input state released: %zu keys, buttons=0x%X (§4.4)", keys.size(),
             buttons);
}

size_t InputInjector::heldCount() const {
  std::lock_guard<std::mutex> lk(m_);
  return heldKeys_.size() + (heldButtons_ ? size_t(__popcnt(heldButtons_)) : 0);
}

// ---------------------------------------------------------------------------
// DesktopTracker
// ---------------------------------------------------------------------------

DesktopTracker::DesktopTracker() : originalDesktop_(GetThreadDesktop(GetCurrentThreadId())) {}

bool DesktopTracker::AttachCurrentThreadToInputDesktop(std::wstring& nameOut,
                                                       bool& secureOut) {
  HDESK h = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                       DESKTOP_SWITCHDESKTOP);
  if (!h) {
    // Can happen transiently mid-switch; caller retries.
    nameOut.clear();
    secureOut = false;
    return false;
  }
  wchar_t buf[256] = {};
  if (!GetUserObjectInformationW(h, UOI_NAME, buf, sizeof(buf), nullptr)) {
    CloseDesktop(h); return false;
  }
  nameOut = buf;
  // Winlogon desktop = secure desktop (UAC prompt / lock / logon).
  secureOut = (nameOut == L"Winlogon");
  if (desktop_ && name_ == nameOut) { CloseDesktop(h); return true; }
  BOOL ok = SetThreadDesktop(h);
  DWORD gle = GetLastError();
  if (!ok) {
    CloseDesktop(h);
    WVD_LOGW("SetThreadDesktop(%ls) failed gle=%lu", buf, gle);
    return false;
  }
  if (desktop_) CloseDesktop(desktop_);
  desktop_ = h;  // keep it open until this thread detaches or exits
  name_ = nameOut;
  secure_ = secureOut;
  return true;
}

DesktopTracker::~DesktopTracker() {
  if (hook_) UnhookWinEvent(static_cast<HWINEVENTHOOK>(hook_));
  if (desktop_) {
    SetThreadDesktop(originalDesktop_);
    CloseDesktop(desktop_);
  }
}

static DesktopTracker::SwitchCb g_switchCb = nullptr;

static void CALLBACK DesktopSwitchProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                       DWORD, DWORD) {
  if (g_switchCb) g_switchCb();
}

bool DesktopTracker::StartWatchingOnCurrentThread(SwitchCb onSwitch) {
  g_switchCb = onSwitch;
  hook_ = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
                          nullptr, DesktopSwitchProc, 0, 0,
                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  return hook_ != nullptr;
}

// ---------------------------------------------------------------------------
// InputGate
// ---------------------------------------------------------------------------

bool InputGate::Accept(uint64_t incomingGen) {
  // The helper owns this number. A client cannot advance it to bypass revocation.
  return state_ == State::Armed && !suspended_ && incomingGen == gen_;
}

void InputGate::Arm() {
  state_ = State::Armed;
  WVD_LOGI("INPUT ARMED — privileged injection enabled for authorised peer (§4.7-2)");
}

void InputGate::Disarm() {
  state_ = State::Disarmed;
  ++gen_;  // new generation; older batches rejected
  WVD_LOGW("INPUT DISARMED (new input generation %llu)", (unsigned long long)gen_);
}

}  // namespace wvd
