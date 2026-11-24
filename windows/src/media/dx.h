// D3D11 device helpers for the media worker.
#pragma once

#include <wrl/client.h>

#include <string>
#include <vector>

#include "wvd/platform.h"

#include <d3d11.h>
#include <dxgi1_6.h>

namespace wvd {

using Microsoft::WRL::ComPtr;

struct DxAdapterInfo {
  UINT index = 0;
  std::wstring name;
  LUID luid{};
  bool isSoftware = false;
  uint64_t vramMB = 0;
  DXGI_GPU_PREFERENCE preference = DXGI_GPU_PREFERENCE_UNSPECIFIED;
};

struct DxOutputInfo {
  UINT adapterIndex = 0;
  std::wstring monitorName;  // \\.\DISPLAYn
  std::wstring monitorDevicePath; // monitor interface; independent of GDI display numbering
  RECT desktopCoords{};      // virtual-desktop rect
  UINT modeWidth = 0, modeHeight = 0, modeRateHz = 0;
  LUID adapterLuid{};
};

// Factory + adapter enumeration.
ComPtr<IDXGIFactory1> CreateDxgiFactory();
std::vector<DxAdapterInfo> EnumerateAdapters(IDXGIFactory1* factory);
std::vector<DxOutputInfo> EnumerateOutputs(IDXGIFactory1* factory);

// Create a D3D11 device on the adapter that owns `output`. The DDA device
// MUST be created on the output's adapter (plan §4.1 Path D).
HRESULT CreateDeviceForAdapter(IDXGIFactory1* factory, const LUID& luid,
                               ComPtr<ID3D11Device>& device,
                               ComPtr<ID3D11DeviceContext>& ctx);

// Turn on D3D11 immediate-context serialisation. The capture thread copies
// frames while the encode thread runs the NV12 convert on the SAME immediate
// context, which is not thread-safe by default (corrupted frames / device
// lost). Call once right after every device creation.
void EnableD3D11ThreadSafety(ID3D11Device* device);

// Ask the GPU scheduler to favour this device's work over other clients
// (best effort; see dx.cpp).
void RaiseGpuPriority(ID3D11Device* device);

// Once per streaming process: GPU scheduling class and CPU priority class
// above a normal-priority game, so a game that saturates the GPU or the CPU
// does not delay capture, encode or the network threads (see dx.cpp).
void RaiseProcessPriority();

// Enumerate attached outputs, then select by exact GDI name or monitor
// interface. Empty selects the primary display.
bool FindOutput(IDXGIFactory1* factory, const wchar_t* monitorNameOrNull,
                DxOutputInfo& out);

// Exact GDI name (full or DISPLAYn), monitor interface, or empty for primary.
// A missing explicit selection never falls back to a different screen.
bool SelectOutput(const std::vector<DxOutputInfo>& outputs, const wchar_t* selector,
                  DxOutputInfo& out);

// The IDXGIOutput named `deviceName` on the adapter `luid` (for DuplicateOutput).
ComPtr<IDXGIOutput> FindDxgiOutput(IDXGIFactory1* factory, const LUID& luid,
                                   const std::wstring& deviceName);

}  // namespace wvd
