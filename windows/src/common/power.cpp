#include "wvd/power.h"

#include "wvd/logging.h"

namespace wvd {

bool KeepAwake::Start(const wchar_t* reason, bool keepDisplay) {
  if (h_) return true;
  REASON_CONTEXT ctx{};
  ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
  // POWER_REQUEST_CONTEXT_SIMPLE_STRING is required: with Flags = 0 the call
  // fails ERROR_INVALID_PARAMETER (87) and keep-awake never engages.
  ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
  ctx.Reason.SimpleReasonString = LPWSTR(reason);

  // PowerCreateRequest returns INVALID_HANDLE_VALUE (not NULL) on failure.
  HANDLE h = PowerCreateRequest(&ctx);
  if (h == INVALID_HANDLE_VALUE || !h) {
    WVD_LOGE("PowerCreateRequest failed gle=%lu", GetLastError());
    return false;
  }
  BOOL ok = PowerSetRequest(h, PowerRequestSystemRequired);
  if (ok && keepDisplay)
    ok = PowerSetRequest(h, PowerRequestDisplayRequired);
  if (!ok) {
    WVD_LOGE("PowerSetRequest failed gle=%lu", GetLastError());
    CloseHandle(h);
    return false;
  }
  h_ = h;
  displayReq_ = keepDisplay;
  WVD_LOGI("keep-awake active (system%s)", keepDisplay ? "+display" : "");
  return true;
}

void KeepAwake::Stop() {
  if (!h_) return;
  PowerClearRequest(h_, PowerRequestSystemRequired);
  if (displayReq_) PowerClearRequest(h_, PowerRequestDisplayRequired);
  CloseHandle(h_);
  h_ = nullptr;
  WVD_LOGI("keep-awake released");
}

}  // namespace wvd
