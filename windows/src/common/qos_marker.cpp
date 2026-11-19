#include "wvd/qos_marker.h"

#include <qos2.h>

#include <mutex>

#include "wvd/logging.h"

#pragma comment(lib, "qwave.lib")
#pragma comment(lib, "ws2_32.lib")

namespace wvd {

namespace {
std::mutex g_qosMutex;
HANDLE g_qos = nullptr;  // one qWAVE handle per process
SOCKET g_markedSocket = INVALID_SOCKET;
QOS_FLOWID g_flow = 0;
}  // namespace

void QosMarkInteractiveVideo(SOCKET s) {
  std::lock_guard<std::mutex> lk(g_qosMutex);
  // A qWAVE handle is REQUIRED: QOSAddSocketToFlow(nullptr, ...) fails with
  // ERROR_INVALID_HANDLE (6) on every call.
  if (!g_qos) {
    QOS_VERSION qv{};
    qv.MajorVersion = 1;
    qv.MinorVersion = 0;
    if (!QOSCreateHandle(&qv, &g_qos)) {
      g_qos = nullptr;
      WVD_LOGW("QOSCreateHandle failed gle=%lu (continuing without WMM marking)",
               GetLastError());
      return;
    }
  }
  if (g_flow && g_markedSocket != INVALID_SOCKET) {
    // Previous session's socket (already closed by now): drop its flow first.
    QOSRemoveSocketFromFlow(g_qos, g_markedSocket, g_flow, 0);
    g_flow = 0;
    g_markedSocket = INVALID_SOCKET;
  }
  QOS_FLOWID flowId = 0;
  if (!QOSAddSocketToFlow(g_qos, s, nullptr, QOSTrafficTypeAudioVideo, 0, &flowId)) {
    WVD_LOGW("QOSAddSocketToFlow failed gle=%lu (continuing without WMM marking)",
             GetLastError());
    return;
  }
  g_markedSocket = s;
  g_flow = flowId;
  WVD_LOGI("qWAVE: media socket marked QOSTrafficTypeAudioVideo flow=%lu", flowId);
}

void QosUnmarkSocket(SOCKET s) {
  std::lock_guard<std::mutex> lk(g_qosMutex);
  if (g_qos && g_flow && (s == INVALID_SOCKET || s == g_markedSocket)) {
    // Removal needs the same qWAVE handle the flow was added with.
    QOSRemoveSocketFromFlow(g_qos, g_markedSocket, g_flow, 0);
    g_flow = 0;
    g_markedSocket = INVALID_SOCKET;
  }
}

}  // namespace wvd
