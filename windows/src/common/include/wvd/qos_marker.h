// WMM / qWAVE traffic marking (§6.3): QOSAddSocketToFlow(QOSTrafficTypeAudioVideo).
#pragma once

#include "wvd/platform.h"

namespace wvd {

// Mark a connected/created socket as interactive video for the wireless
// media WMM classifier. Best-effort: logs and continues on failure.
void QosMarkInteractiveVideo(SOCKET s);
void QosUnmarkSocket(SOCKET s);

}  // namespace wvd
