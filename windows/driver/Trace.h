// Minimal trace helper for the WVD UMDF driver.
//
// UMDF 2 drivers have no console, so traces go to the kernel debug buffer
// (visible in WinDbg `!dbgprint` / DbgView) and to ETW when tracing is enabled.
// Release builds compile the traces out.
#pragma once

#ifndef WVD_TRACE_ENABLE
#ifdef _DEBUG
#define WVD_TRACE_ENABLE 1
#else
#define WVD_TRACE_ENABLE 0
#endif
#endif


#if WVD_TRACE_ENABLE
// UMDF runs in user mode; use its debug output API, not kernel DbgPrintEx.
#include <cstdio>
#define TRACE(...)                                          \
  do {                                                      \
    char traceMessage[1024];                                 \
    _snprintf_s(traceMessage, _TRUNCATE, "[wvdidd] " __VA_ARGS__); \
    OutputDebugStringA(traceMessage);                        \
  } while (0)
#else
#define TRACE(...) do { } while (0)
#endif
