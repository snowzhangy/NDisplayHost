// WVD IddCx virtual display driver — UMDF 2 (plan §4.1).
//
// Structure follows Microsoft's IndirectDisplay sample
// (video/IndirectDisplay in Windows-driver-samples); that sample is the
// reference for the UMDF/IddCx call sequence:
//
//   DriverEntry
//     └─ EvtDeviceAdd
//          ├─ WdfDeviceInitSetPnpPowerEventCallbacks (D0Entry)
//          ├─ IDD_CX_CLIENT_CONFIG_INIT + EvtIddCx* callbacks
//          ├─ IddCxDeviceInitConfig(pDeviceInit, &cfg)   <-- callbacks wired HERE
//          ├─ WdfDeviceCreate
//          └─ IddCxDeviceInitialize(Device)
//   EvtDeviceD0Entry        → IddCxAdapterInitAsync  (adapter starts ASYNC)
//   EvtIddCxAdapterInitFinished → create monitors, IddCxMonitorArrival
//   EvtIddCxMonitorAssignSwapChain → start the frame-drain loop
//
// Modes come from the ParseMonitorDescription / GetDefaultDescriptionModes /
// QueryTargetModes / CommitModes callbacks, never from a "mode list" API.
// Frames are consumed by IddCxSwapChainSetDevice + a
// ReleaseAndAcquireBuffer/FinishedProcessingFrame loop.
//
// BUILD: requires the WDK (iddcx.h). It is deliberately NOT part of the CMake
// user-mode build. Symbol spellings are taken from the current public IddCx
// header; reconcile at compile time.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <wrl.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "Trace.h"
#include "Edid.h"

// ---------------------------------------------------------------------------
// WVD policy constants
// ---------------------------------------------------------------------------

// Monitors this adapter publishes (P0: one; the owner may add more later).
constexpr UINT WVD_MONITOR_COUNT = 1;

// ---------------------------------------------------------------------------
// Driver contexts
// ---------------------------------------------------------------------------

struct MonitorContext;

// WDF allocates contexts as raw memory and never runs C++ constructors, so the
// contexts below hold a pointer to the real object (created with new) and
// release it in an EvtCleanupCallback — same pattern as the sample.

// Per-adapter state.
struct AdapterContext {
  WDFDEVICE WdfDevice = nullptr;
  IDDCX_ADAPTER Adapter = nullptr;

  // Path D (P0 default): the OS-owned redirection surface is captured by the
  // media worker through Desktop Duplication; the driver does not encode.
  // Path S (copy into a service-owned ring) is a measured spike (§4.1).
  bool PathSpikeEnabled = false;

  // Adapter the encoder lives on, as told to us by the OS at AssignSwapChain.
  // The media worker reads this back so it opens its encoder on the same GPU.
  LUID RenderAdapterLuid{};
  bool RenderAdapterKnown = false;

  MonitorContext* Monitors[WVD_MONITOR_COUNT] = {};
};

struct AdapterContextWrapper {
  AdapterContext* pContext = nullptr;
  void Cleanup() {
    delete pContext;
    pContext = nullptr;
  }
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContextWrapper,
                                   WdfObjectGet_AdapterContextWrapper)

// Per-monitor state, including the swap-chain processing loop that keeps the
// OS-produced frames drained (§4.1: never let the swapchain back up).
struct MonitorContext {
  ~MonitorContext() { StopSwapchainProcessing(); }
  IDDCX_MONITOR Monitor = nullptr;
  IDDCX_SWAPCHAIN Swapchain = nullptr;
  HANDLE FrameArrivalEvent = nullptr;
  std::unique_ptr<std::thread> DrainThread;
  std::atomic<bool> StopRequested{false};
  AdapterContext* Adapter = nullptr;
  LUID RenderAdapterLuid{};

  // Last accepted mode (for reporting).
  ULONG CurrentWidth = 0, CurrentHeight = 0, CurrentVSync = 0;

  void StartSwapchainProcessing(IDDCX_SWAPCHAIN swapchain,
                                const LUID& renderAdapterLuid,
                                HANDLE newFrameEvent);
  void StopSwapchainProcessing();
  void DrainLoop();
};

struct MonitorContextWrapper {
  MonitorContext* pContext = nullptr;
  void Cleanup() {
    delete pContext;
    pContext = nullptr;
  }
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContextWrapper,
                                   WdfObjectGet_MonitorContextWrapper)

// ---------------------------------------------------------------------------
// WDF / IddCx callbacks (implemented in Driver.cpp / Monitor.cpp)
// ---------------------------------------------------------------------------

EVT_WDF_DRIVER_DEVICE_ADD WvdEvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY WvdEvtDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED WvdEvtAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES WvdEvtAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION WvdEvtParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES WvdEvtMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES WvdEvtMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN WvdEvtMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN WvdEvtMonitorUnassignSwapChain;

// Monitor.cpp
NTSTATUS WvdCreateMonitor(AdapterContext* adapter, UINT connectorIndex);
