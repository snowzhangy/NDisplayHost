// WVD IddCx monitor creation + swap-chain draining (plan §4.1).
//
// Two responsibilities:
//  1. Publish the monitor with a correct EDID, then tell the OS it arrived.
//  2. Keep the OS-produced redirection swapchain DRAINED. This is the
//     performance-critical loop: frames must be acquired and released promptly
//     or the whole desktop stalls. In Path D (P0 default) the driver does not
//     encode — it releases each buffer immediately and the media worker captures
//     the redirection surface with Desktop Duplication.
#include "Driver.h"

#include "Trace.h"
using Microsoft::WRL::ComPtr;

namespace {

// Registry path the device owner may use to pin the encoder GPU before the
// first swapchain arrives (§4.1 "pin the render adapter"). Reading the LUID in
// the DRIVER from a kernel-only API was wrong; the owner writes a value and we
// merely read it, and in any case the authoritative LUID comes from
// EvtIddCxMonitorAssignSwapChain.
constexpr wchar_t kRegRenderAdapterLuid[] = L"RenderAdapterLuid";

NTSTATUS ReadPinnedRenderAdapter(WDFDEVICE device, LUID* outLuid) {
  if (!device || !outLuid) return STATUS_INVALID_PARAMETER;
  WDFKEY key = nullptr;
  NTSTATUS status =
      WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                               WDF_NO_OBJECT_ATTRIBUTES, &key);
  if (!NT_SUCCESS(status)) return status;

  UNICODE_STRING name;
  RtlInitUnicodeString(&name, kRegRenderAdapterLuid);
  ULONG type = 0, size = 0;
  status = WdfRegistryQueryValue(key, &name, sizeof(LUID), outLuid, &size, &type);
  if (NT_SUCCESS(status) && (type != REG_BINARY || size != sizeof(LUID)))
    status = STATUS_INVALID_PARAMETER;
  WdfRegistryClose(key);
  return status;
}

}  // namespace

// ---------------------------------------------------------------------------
// Monitor creation
// ---------------------------------------------------------------------------

NTSTATUS WvdCreateMonitor(AdapterContext* adapter, UINT connectorIndex) {
  if (!adapter || !adapter->Adapter) return STATUS_INVALID_DEVICE_STATE;

  WvdEdid edid;
  // Serial is unique per monitor so the OS can distinguish them and keep
  // per-monitor scaling/mode state (§4.1).
  WvdBuildEdid(edid, kWvdReferenceModes[0], 0x57564400u + connectorIndex,
               kWvdMonitorPpi, kWvdMonitorName);

  IDDCX_MONITOR_INFO info = {};
  info.Size = sizeof(info);
  info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = sizeof(edid);
  info.MonitorDescription.pData = edid.data;

  // Container ID identifies the monitor across reboots; keep it stable and
  // distinct per connector so the owner's saved topology still matches.
  info.MonitorContainerId.Data1 = 0x57564400u;
  info.MonitorContainerId.Data2 = 0x0001;
  info.MonitorContainerId.Data3 = USHORT(connectorIndex);
  info.MonitorContainerId.Data4[0] = 'W';
  info.MonitorContainerId.Data4[1] = 'V';
  info.MonitorContainerId.Data4[2] = 'D';
  info.MonitorContainerId.Data4[3] = 0;
  info.MonitorContainerId.Data4[4] = 0;
  info.MonitorContainerId.Data4[5] = 0;
  info.MonitorContainerId.Data4[6] = 0;
  info.MonitorContainerId.Data4[7] = BYTE(connectorIndex);

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContextWrapper);
  attr.EvtCleanupCallback = [](WDFOBJECT Object) {
    auto* w = WdfObjectGet_MonitorContextWrapper(Object);
    if (w) w->Cleanup();
  };

  IDARG_IN_MONITORCREATE in = {};
  in.ObjectAttributes = &attr;
  in.pMonitorInfo = &info;

  IDARG_OUT_MONITORCREATE out = {};
  NTSTATUS status = IddCxMonitorCreate(adapter->Adapter, &in, &out);
  if (!NT_SUCCESS(status)) {
    TRACE("IddCxMonitorCreate failed 0x%x\n", status);
    return status;
  }

  auto* wrapper = WdfObjectGet_MonitorContextWrapper(out.MonitorObject);
  wrapper->pContext = new MonitorContext();
  if (!wrapper->pContext) return STATUS_NO_MEMORY;
  wrapper->pContext->Monitor = out.MonitorObject;
  wrapper->pContext->Adapter = adapter;
  if (connectorIndex < WVD_MONITOR_COUNT) adapter->Monitors[connectorIndex] = wrapper->pContext;

  // Modes are supplied by the ParseMonitorDescription / QueryTargetModes
  // callbacks, so there is no mode list to publish here; the OS parses this
  // EDID and asks us for modes during arrival.
  IDARG_OUT_MONITORARRIVAL arrival = {};
  status = IddCxMonitorArrival(out.MonitorObject, &arrival);
  if (!NT_SUCCESS(status)) {
    TRACE("IddCxMonitorArrival failed 0x%x\n", status);
    return status;
  }

  // Optionally pin the encoder GPU before the first frame. The authoritative
  // value still arrives with AssignSwapChain; this only narrows the race where
  // the OS picks a different adapter.
  LUID pinned = {};
  if (NT_SUCCESS(ReadPinnedRenderAdapter(adapter->WdfDevice, &pinned))) {
    adapter->RenderAdapterLuid = pinned;
    adapter->RenderAdapterKnown = true;
    TRACE("pinned render adapter %u:%u from the device registry\n", pinned.HighPart,
          pinned.LowPart);
  }

  TRACE("monitor %u arrived (target id %u)\n", connectorIndex, arrival.MonitorTargetId);
  return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swap-chain processing
// ---------------------------------------------------------------------------
//
// The OS hands us a swapchain of redirection surfaces. We MUST call
// IddCxSwapChainSetDevice once, then keep calling
// IddCxSwapChainReleaseAndAcquireBuffer / IddCxSwapChainFinishedProcessingFrame
// in a loop; anything else lets the queue back up and freezes the desktop.

void MonitorContext::StartSwapchainProcessing(IDDCX_SWAPCHAIN swapchain,
                                              const LUID& renderAdapterLuid,
                                              HANDLE newFrameEvent) {
  StopSwapchainProcessing();

  Swapchain = swapchain;
  RenderAdapterLuid = renderAdapterLuid;
  FrameArrivalEvent = newFrameEvent;
  if (Adapter) {
    Adapter->RenderAdapterLuid = renderAdapterLuid;
    Adapter->RenderAdapterKnown = true;
  }
  StopRequested = false;
  DrainThread.reset(new std::thread([this] { DrainLoop(); }));
}

void MonitorContext::StopSwapchainProcessing() {
  if (DrainThread) {
    StopRequested = true;
    DrainThread->join();
    DrainThread.reset();
  }
  Swapchain = nullptr;
  FrameArrivalEvent = nullptr;
}

void MonitorContext::DrainLoop() {
  if (!Swapchain) return;

  // Give IddCx a device to allocate its redirection surfaces on. We open the
  // adapter the OS named, so the surfaces live on the GPU that also feeds the
  // encoder (§4.1 Path D).
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    WdfObjectDelete(Swapchain);
    Swapchain = nullptr;
    return;
  }

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d = {};
    a->GetDesc1(&d);
    if (d.AdapterLuid.HighPart == RenderAdapterLuid.HighPart &&
        d.AdapterLuid.LowPart == RenderAdapterLuid.LowPart) {
      adapter = a;
      break;
    }
  }

  if (!adapter) {
    WdfObjectDelete(Swapchain);
    Swapchain = nullptr;
    return;  // never silently allocate on a different GPU
  }

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL fl = {};
  hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                : D3D_DRIVER_TYPE_HARDWARE,
                         nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &fl,
                         &context);
  if (FAILED(hr)) {
    // Deleting the swapchain tells the OS to try again with another adapter.
    WdfObjectDelete(Swapchain);
    Swapchain = nullptr;
    return;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  hr = device.As(&dxgiDevice);
  if (FAILED(hr)) {
    WdfObjectDelete(Swapchain);
    Swapchain = nullptr;
    return;
  }

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();
  hr = IddCxSwapChainSetDevice(Swapchain, &setDevice);
  if (FAILED(hr)) {
    WdfObjectDelete(Swapchain);
    Swapchain = nullptr;
    return;
  }

  // Acquire/release loop (§4.1). Path D: the buffer is released immediately;
  // Desktop Duplication in the media worker is what reads the pixels.
  for (;;) {
    if (StopRequested) break;

    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
    hr = IddCxSwapChainReleaseAndAcquireBuffer(Swapchain, &buffer);

    if (hr == E_PENDING) {
      // Nothing queued: wait for the arrival event (or stop).
      HANDLE handles[1] = {FrameArrivalEvent};
      DWORD wait = WaitForMultipleObjects(1, handles, FALSE, 100);
      if (wait == WAIT_FAILED) break;
      continue;
    }
    if (FAILED(hr)) break;  // swapchain abandoned / ACCESS_LOST

    ComPtr<IDXGIResource> acquired;
    acquired.Attach(buffer.MetaData.pSurface);  // we own this reference
    if (!acquired) break;

    // ---- Path S spike (NOT the P0 default, §4.1) --------------------------
    // To evaluate the copy-into-our-own-ring path, copy this surface into a
    // service-owned shared ring and bump the content generation. Rules that
    // must hold: every entry is a FULL frame; the ring is created by the media
    // service and shared into the driver by handle (never the reverse); the
    // content generation advances on mode change, adapter change, unassign,
    // TDR and device removal. Promote only with a measured >=3 ms p95 win.
    // ------------------------------------------------------------------------

    acquired.Reset();  // release our reference before telling the OS

    // Path D: presents complete as soon as the OS has the surface back.
    HRESULT hrDone = IddCxSwapChainFinishedProcessingFrame(Swapchain);
    if (FAILED(hrDone)) break;

    // Frame statistics for §8 measurement (encode/send sub-timings are reported
    // by the media worker; here we only mark that the frame was consumed).
    // IddCxSwapChainReportFrameStatistics(Swapchain, &stats);
  }

  // Always delete the swapchain when the loop ends so the OS can hand us a new
  // one if the monitor is still live.
  if (Swapchain) {
    WdfObjectDelete(Swapchain);
    Swapchain = nullptr;
  }
}
