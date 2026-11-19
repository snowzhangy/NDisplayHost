// WVD IddCx adapter + monitor provider (UMDF 2). Plan §4.1.
//
// Sequence (mirrors Microsoft's IndirectDisplay sample — the reference for
// correct IddCx usage):
//   DriverEntry → EvtDeviceAdd → IDD_CX_CLIENT_CONFIG + callbacks →
//   IddCxDeviceInitConfig → WdfDeviceCreate → IddCxDeviceInitialize.
//   D0Entry → IddCxAdapterInitAsync (adapter start is ASYNCHRONOUS).
//   EvtIddCxAdapterInitFinished → create monitors + IddCxMonitorArrival.
//
// Everything the previous draft got wrong is corrected here: the callback
// struct is actually passed, the adapter is started at power-on, monitors are
// created only after init finishes, and modes are served from callbacks.
#include "Driver.h"

#include "Trace.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

// ---------------------------------------------------------------------------
// Reference data
// ---------------------------------------------------------------------------

namespace {

// A stable name lets `wvd-owner --arrange WVD...` find the monitor. It goes in
// the EDID's 0xFC monitor-name descriptor.
// (kWvdMonitorName / kWvdMonitorPpi live in Driver.h: the owner matches on the
// name, and the PPI drives the EDID physical size.)

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& m, ULONG width, ULONG height,
                    ULONG vSyncHz, bool monitorMode) {
  const auto timing = WvdSignalTiming({width, height, vSyncHz});
  m.activeSize = {width, height};
  m.totalSize = {width + timing.hBlank, height + timing.vBlank};
  m.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  m.AdditionalSignalInfo.videoStandard = 255;  // unspecified
  m.vSyncFreq.Numerator = vSyncHz;
  m.vSyncFreq.Denominator = 1;
  m.hSyncFreq.Numerator = UINT32(timing.pixelRate);
  m.hSyncFreq.Denominator = m.totalSize.cx;
  m.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  m.pixelRate = timing.pixelRate;
  m.vSyncFreq.Numerator = UINT32(timing.pixelRate);
  m.vSyncFreq.Denominator = m.totalSize.cx * m.totalSize.cy;
}

IDDCX_MONITOR_MODE MakeMonitorMode(const WvdMode& vm, IDDCX_MONITOR_MODE_ORIGIN origin) {
  IDDCX_MONITOR_MODE mode = {};
  mode.Size = sizeof(mode);
  mode.Origin = origin;
  FillSignalInfo(mode.MonitorVideoSignalInfo, vm.width, vm.height, vm.vSync, true);
  return mode;
}

IDDCX_TARGET_MODE MakeTargetMode(const WvdMode& vm) {
  IDDCX_TARGET_MODE mode = {};
  mode.Size = sizeof(mode);
  FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, vm.width, vm.height,
                 vm.vSync, false);
  return mode;
}

}  // namespace

// ---------------------------------------------------------------------------
// Driver / device lifetime
// ---------------------------------------------------------------------------

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                                PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG config;
  WDF_OBJECT_ATTRIBUTES attrs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
  WDF_DRIVER_CONFIG_INIT(&config, WvdEvtDeviceAdd);

  NTSTATUS status = WdfDriverCreate(DriverObject, RegistryPath, &attrs, &config,
                                    WDF_NO_HANDLE);
  if (!NT_SUCCESS(status)) TRACE("WdfDriverCreate failed 0x%x\n", status);
  return status;
}

_Use_decl_annotations_ NTSTATUS WvdEvtDeviceAdd(WDFDRIVER Driver,
                                                PWDFDEVICE_INIT DeviceInit) {
  UNREFERENCED_PARAMETER(Driver);

  // Only D0Entry matters: the adapter (and therefore the monitors) is created
  // when the device powers on.
  WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
  pnpPower.EvtDeviceD0Entry = WvdEvtDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

  // All IddCx callbacks are registered through this config struct — passing
  // none (as an earlier draft did) means the driver is never called back and
  // no monitor is ever created.
  IDD_CX_CLIENT_CONFIG iddConfig;
  IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
  iddConfig.EvtIddCxAdapterInitFinished = WvdEvtAdapterInitFinished;
  iddConfig.EvtIddCxParseMonitorDescription = WvdEvtParseMonitorDescription;
  iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = WvdEvtMonitorGetDefaultModes;
  iddConfig.EvtIddCxMonitorQueryTargetModes = WvdEvtMonitorQueryModes;
  iddConfig.EvtIddCxAdapterCommitModes = WvdEvtAdapterCommitModes;
  iddConfig.EvtIddCxMonitorAssignSwapChain = WvdEvtMonitorAssignSwapChain;
  iddConfig.EvtIddCxMonitorUnassignSwapChain = WvdEvtMonitorUnassignSwapChain;

  NTSTATUS status = IddCxDeviceInitConfig(DeviceInit, &iddConfig);
  if (!NT_SUCCESS(status)) {
    TRACE("IddCxDeviceInitConfig failed 0x%x\n", status);
    return status;
  }

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AdapterContextWrapper);
  attr.EvtCleanupCallback = [](WDFOBJECT Object) {
    auto* wrapper = WdfObjectGet_AdapterContextWrapper(Object);
    if (wrapper) wrapper->Cleanup();
  };

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&DeviceInit, &attr, &device);
  if (!NT_SUCCESS(status)) {
    TRACE("WdfDeviceCreate failed 0x%x\n", status);
    return status;
  }

  status = IddCxDeviceInitialize(device);

  auto* wrapper = WdfObjectGet_AdapterContextWrapper(device);
  wrapper->pContext = new AdapterContext();
  if (!wrapper->pContext) return STATUS_NO_MEMORY;
  wrapper->pContext->WdfDevice = device;
  return status;
}

_Use_decl_annotations_ NTSTATUS WvdEvtDeviceD0Entry(WDFDEVICE Device,
                                                    WDF_POWER_DEVICE_STATE Previous) {
  UNREFERENCED_PARAMETER(Previous);
  auto* wrapper = WdfObjectGet_AdapterContextWrapper(Device);
  AdapterContext* adapter = wrapper->pContext;
  if (!adapter || adapter->Adapter) return STATUS_SUCCESS;  // already started

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);
  caps.MaxMonitorsSupported = WVD_MONITOR_COUNT;
  caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
  caps.EndPointDiagnostics.pEndPointFriendlyName = L"WVD Virtual Display";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"WVD Project";
  caps.EndPointDiagnostics.pEndPointModelName = L"WVD Virtual Display 1.0";

  IDDCX_ENDPOINT_VERSION ver = {};
  ver.Size = sizeof(ver);
  ver.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &ver;
  caps.EndPointDiagnostics.pHardwareVersion = &ver;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AdapterContextWrapper);

  IDARG_IN_ADAPTER_INIT in = {};
  in.WdfDevice = Device;
  in.pCaps = &caps;
  in.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT out = {};
  // Adapter initialisation is asynchronous: monitors may only be created from
  // EvtIddCxAdapterInitFinished, never here.
  NTSTATUS status = IddCxAdapterInitAsync(&in, &out);
  if (NT_SUCCESS(status)) {
    adapter->Adapter = out.AdapterObject;
    // The adapter gets its own context wrapper so the callbacks can find us.
    auto* adapterWrapper = WdfObjectGet_AdapterContextWrapper(out.AdapterObject);
    if (adapterWrapper) adapterWrapper->pContext = adapter;
  } else {
    TRACE("IddCxAdapterInitAsync failed 0x%x\n", status);
  }
  return status;
}

// ---------------------------------------------------------------------------
// Adapter callbacks
// ---------------------------------------------------------------------------

_Use_decl_annotations_ NTSTATUS WvdEvtAdapterInitFinished(
    IDDCX_ADAPTER Adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* InArgs) {
  if (!NT_SUCCESS(InArgs->AdapterInitStatus)) {
    TRACE("adapter init failed 0x%x\n", InArgs->AdapterInitStatus);
    return STATUS_SUCCESS;
  }
  auto* wrapper = WdfObjectGet_AdapterContextWrapper(Adapter);
  AdapterContext* adapter = wrapper ? wrapper->pContext : nullptr;
  if (!adapter) return STATUS_SUCCESS;

  for (UINT i = 0; i < WVD_MONITOR_COUNT; ++i) {
    NTSTATUS status = WvdCreateMonitor(adapter, i);
    if (!NT_SUCCESS(status)) TRACE("monitor %u create failed 0x%x\n", i, status);
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS WvdEvtAdapterCommitModes(
    IDDCX_ADAPTER Adapter, const IDARG_IN_COMMITMODES* InArgs) {
  UNREFERENCED_PARAMETER(Adapter);
  // IddCx owns the redirection swapchains; for Path D there is nothing to
  // reconfigure here. A mode change makes the media worker rebuild its ring and
  // bump the layout/codec generations (§4.1), driven by its own ACCESS_LOST.
  for (UINT i = 0; i < InArgs->PathCount; ++i) {
    const IDDCX_PATH& p = InArgs->pPaths[i];
    if (p.Flags & IDDCX_PATH_FLAGS_ACTIVE) {
      TRACE("commit: monitor %p active %ux%u\n", p.MonitorObject,
            p.TargetVideoSignalInfo.activeSize.cx, p.TargetVideoSignalInfo.activeSize.cy);
    }
  }
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS WvdEvtParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* InArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* OutArgs) {
  // The EDID we publish is a standard EDID 1.4 block. We still serve the mode
  // list ourselves so the OS sees exactly the reference modes (§4.1) rather
  // than whatever a parser derives from the detailed timings.
  if (InArgs->MonitorDescription.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID)
    return STATUS_INVALID_PARAMETER;

  OutArgs->MonitorModeBufferOutputCount = kWvdReferenceModeCount;
  OutArgs->PreferredMonitorModeIdx = 0;
  if (!InArgs->pMonitorModes) return STATUS_SUCCESS;
  if (InArgs->MonitorModeBufferInputCount < kWvdReferenceModeCount) {
    // Count-only query is allowed with a zero-size buffer.
    return InArgs->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL
                                                   : STATUS_SUCCESS;
  }
  for (UINT i = 0; i < kWvdReferenceModeCount; ++i)
    InArgs->pMonitorModes[i] =
        MakeMonitorMode(kWvdReferenceModes[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS WvdEvtMonitorGetDefaultModes(
    IDDCX_MONITOR Monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* InArgs,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* OutArgs) {
  UNREFERENCED_PARAMETER(Monitor);
  // "Default" modes are what the OS uses when the descriptor carries none;
  // report the safe low-resolution subset first.
  static const WvdMode kDefaultModes[] = {{1920, 1080, 60}, {1280, 720, 60}};
  constexpr UINT kCount = ARRAYSIZE(kDefaultModes);
  OutArgs->DefaultMonitorModeBufferOutputCount = kCount;
  OutArgs->PreferredMonitorModeIdx = 0;

  if (InArgs->DefaultMonitorModeBufferInputCount == 0) {
    OutArgs->DefaultMonitorModeBufferOutputCount = kCount;
    return STATUS_SUCCESS;
  }
  if (InArgs->DefaultMonitorModeBufferInputCount < kCount)
    return STATUS_BUFFER_TOO_SMALL;

  for (UINT i = 0; i < kCount; ++i)
    InArgs->pDefaultMonitorModes[i] =
        MakeMonitorMode(kDefaultModes[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
  OutArgs->DefaultMonitorModeBufferOutputCount = kCount;
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS WvdEvtMonitorQueryModes(
    IDDCX_MONITOR Monitor, const IDARG_IN_QUERYTARGETMODES* InArgs,
    IDARG_OUT_QUERYTARGETMODES* OutArgs) {
  UNREFERENCED_PARAMETER(Monitor);
  OutArgs->TargetModeBufferOutputCount = kWvdReferenceModeCount;
  if (InArgs->TargetModeBufferInputCount == 0) return STATUS_SUCCESS;
  if (InArgs->TargetModeBufferInputCount < kWvdReferenceModeCount)
    return STATUS_BUFFER_TOO_SMALL;
  for (UINT i = 0; i < kWvdReferenceModeCount; ++i)
    InArgs->pTargetModes[i] = MakeTargetMode(kWvdReferenceModes[i]);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS WvdEvtMonitorAssignSwapChain(
    IDDCX_MONITOR Monitor, const IDARG_IN_SETSWAPCHAIN* InArgs) {
  auto* wrapper = WdfObjectGet_MonitorContextWrapper(Monitor);
  MonitorContext* mon = wrapper ? wrapper->pContext : nullptr;
  if (!mon) return STATUS_INVALID_DEVICE_STATE;

  // The OS tells us which adapter renders this monitor. Remember it: the media
  // worker must open its DDA/encoder on the SAME adapter, and a mismatch shows
  // up as ACCESS_LOST or a black picture (§4.1 Path D).
  if (mon->Adapter) {
    mon->Adapter->RenderAdapterLuid = InArgs->RenderAdapterLuid;
    mon->Adapter->RenderAdapterKnown = true;
  }
  mon->StartSwapchainProcessing(InArgs->hSwapChain, InArgs->RenderAdapterLuid,
                                InArgs->hNextSurfaceAvailable);
  return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS WvdEvtMonitorUnassignSwapChain(IDDCX_MONITOR Monitor) {
  auto* wrapper = WdfObjectGet_MonitorContextWrapper(Monitor);
  if (wrapper && wrapper->pContext) wrapper->pContext->StopSwapchainProcessing();
  return STATUS_SUCCESS;
}
