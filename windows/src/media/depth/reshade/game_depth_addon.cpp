#include "depth/game_depth_ipc.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <reshade.hpp>

#include <array>
#include <algorithm>
#include <cstring>

using namespace reshade::api;
using namespace wvd::game_depth;
using Microsoft::WRL::ComPtr;

namespace {

constexpr char kEffect[] = "NDisplayGameDepth.addonfx";

int64_t Qpc() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

constexpr uint32_t Align(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

struct Readback {
  resource buffer{};
  uint64_t capacity = 0;
  uint64_t fenceValue = 0;
  Frame frame;
  ComPtr<ID3D12Resource> color;
  Handle colorHandle;
  uint64_t colorId = 0, consumerRelease = 0;
  uint32_t depthPitch = 0;
};

struct __declspec(uuid("e9f06082-1f6e-4f32-becc-6f660c2d1f29")) CaptureState {
  Mapping mapping;
  Handle pipe, host;
  uint64_t producerId = 0, nextTexture = 0;
  bool registered = false;
  int64_t frequency = 0;
  int64_t nextOpen = 0, nextCapture = 0;
  Request request;
  fence gpuFence{};
  ComPtr<ID3D12Fence> releaseFence;
  Handle releaseHandle;
  uint64_t nextFence = 0;
  std::array<Readback, kSlotCount> slots;
  Readback* queued = nullptr;
  bool failed = false;
  bool depthLogged = false;
};

void Init(effect_runtime* runtime) {
  if (runtime->get_device()->get_api() != device_api::d3d12) return;
  auto* state = runtime->create_private_data<CaptureState>();
  state->producerId = Qpc();
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  state->frequency = frequency.QuadPart;
}

void Destroy(effect_runtime* runtime) {
  auto* state = runtime->get_private_data<CaptureState>();
  if (!state) return;
  auto* device = runtime->get_device();
  if (state->gpuFence.handle) {
    runtime->get_command_queue()->wait_idle();
    for (const auto& slot : state->slots)
      if (slot.buffer.handle) device->destroy_resource(slot.buffer);
    device->destroy_fence(state->gpuFence);
  }
  runtime->destroy_private_data<CaptureState>();
}

void RememberReleases(CaptureState& state, const SharedFrames& shared) {
  if (shared.producerId != state.producerId) return;
  for (size_t i = 0; i < state.slots.size(); ++i)
    state.slots[i].consumerRelease =
        (std::max)(state.slots[i].consumerRelease, shared.releases[i]);
}

bool ReadRequest(effect_runtime* runtime, CaptureState& state, int64_t now, Request& request) {
  const auto window = static_cast<HWND>(runtime->get_hwnd());
  if (!window) return false;
  if (!state.mapping) {
    if (now < state.nextOpen || window != GetForegroundWindow()) return false;
    // A replacement host must not signal over a still-pending release on
    // another GPU queue. Wait by polling, with the game free to render.
    if (state.releaseFence) {
      const uint64_t completed = state.releaseFence->GetCompletedValue();
      if (completed == UINT64_MAX) {
        state.failed = true;
        return false;
      }
      for (const auto& slot : state.slots)
        if (slot.consumerRelease != UINT64_MAX && slot.consumerRelease > completed) return false;
    }
    if (!state.pipe) {
      state.pipe.Reset(CreateFileW(PipeName(window).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    }
    if (!state.pipe || !ImportOffer(state.pipe.Get(), state.mapping, state.host)) {
      if (state.pipe && !PeekNamedPipe(state.pipe.Get(), nullptr, 0, nullptr, nullptr, nullptr))
        state.pipe.Reset();
      state.nextOpen = now + state.frequency / 20;
      return false;
    }
  }
  {
    Mapping::Access access(state.mapping);
    if (!access.frames) return false;
    auto& shared = *access.frames;
    RememberReleases(state, shared);
    request = shared.request;
    if (request.token && request.expires > now &&
        WaitForSingleObject(state.host.Get(), 0) == WAIT_TIMEOUT) {
      auto* device = reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native());
      if (window != GetForegroundWindow() || request.window != reinterpret_cast<uint64_t>(window) ||
          !request.fps || !SupportedSize(request.width, request.height) ||
          !SameAdapter(request.adapter, device->GetAdapterLuid())) return false;
      if (!state.registered) {
        shared.frame = {};
        shared.releases = {};
        shared.producerId = state.producerId;
        shared.releaseFence = 0;
        state.registered = true;
      }
      if (shared.producerId == state.producerId) return true;
      // A replacement ReShade runtime owns the mapping. This instance can
      // no longer recover the old release values, so it must stop writing.
      state.failed = true;
      return false;
    }
  }
  state.pipe.Reset();
  state.host.Reset();
  state.mapping.Close();
  state.registered = false;
  return false;
}

bool PrepareColor(ID3D12Device* device, CaptureState& state, Readback& slot,
                  uint32_t width, uint32_t height, DXGI_FORMAT format) {
  if (!state.releaseFence) {
    HANDLE handle = nullptr;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
        FAILED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
      return false;
    state.releaseFence = std::move(fence);
    state.releaseHandle.Reset(handle);
  }
  if (slot.color) {
    const auto desc = slot.color->GetDesc();
    if (desc.Width == width && desc.Height == height && desc.Format == format) return true;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = format;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> texture;
  HANDLE handle = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&texture))) ||
      FAILED(device->CreateSharedHandle(texture.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
    return false;
  slot.color = std::move(texture);
  slot.colorHandle.Reset(handle);
  slot.colorId = ++state.nextTexture;
  return true;
}

void Publish(device* device, CaptureState& state, const Readback& slot, int64_t now) {
  if (now - slot.frame.qpc >= state.frequency / 10) return;
  void* mapped = nullptr;
  if (slot.frame.depthValid &&
      !device->map_buffer_region(slot.buffer, 0, slot.capacity, map_access::read_only, &mapped)) return;
  {
    Mapping::Access access(state.mapping);
    if (access.frames && access.frames->request.token == slot.frame.token &&
        access.frames->request.expires > now && access.frames->producerId == state.producerId) {
      auto& shared = *access.frames;
      Frame frame = slot.frame;
      if (frame.depthValid) {
        const uint32_t width = frame.width / 4, height = frame.height / 16 * 4;
        const auto* src = static_cast<const uint8_t*>(mapped);
        const uint16_t first = *reinterpret_cast<const uint16_t*>(src);
        uint16_t delta = 0;
        for (uint32_t y = 0; y < height; ++y, src += slot.depthPitch) {
          const auto* row = reinterpret_cast<const uint16_t*>(src);
          std::memcpy(shared.depth + size_t(y) * width, row, size_t(width) * sizeof(*row));
          for (uint32_t x = 0; x < width; ++x) delta |= row[x] ^ first;
        }
        // A cleared or placeholder depth texture contains no scene geometry.
        frame.depthValid = delta != 0;
      }
      shared.releaseFence = reinterpret_cast<uint64_t>(state.releaseHandle.Get());
      shared.frame = frame;
      if (frame.depthValid && !state.depthLogged) {
        reshade::log::message(reshade::log::level::info, "NDisplay: exporting rendered colour and scene depth.");
        state.depthLogged = true;
      }
    }
  }
  if (mapped) device->unmap_buffer_region(slot.buffer);
}

void Begin(effect_runtime* runtime, command_list*, resource_view, resource_view) {
  auto* state = runtime->get_private_data<CaptureState>();
  if (!state) return;
  const auto uniform = runtime->find_uniform_variable(kEffect, "NDisplayCaptureEnabled");
  if (state->failed) {
    if (uniform.handle) runtime->set_uniform_value_bool(uniform, false);
    return;
  }
  const int64_t now = Qpc();
  Request request;
  const bool active = ReadRequest(runtime, *state, now, request);
  if (state->request.token != request.token || !active) state->nextCapture = 0;
  state->request = active ? request : Request{};
  auto* device = runtime->get_device();
  if (state->gpuFence.handle) {
    const uint64_t done = device->get_completed_fence_value(state->gpuFence);
    if (done == UINT64_MAX) {
      state->failed = true;
      return;
    }
    const Readback* newest = nullptr;
    for (auto& slot : state->slots) {
      if (!slot.fenceValue || slot.fenceValue > done) continue;
      if (active && slot.frame.token == request.token && (!newest || slot.frame.qpc > newest->frame.qpc))
        newest = &slot;
      slot.fenceValue = 0;
    }
    if (newest) Publish(device, *state, *newest, now);
  }

  if (uniform.handle) runtime->set_uniform_value_bool(uniform, active && now >= state->nextCapture);
}

bool SupportedColor(format f) {
  return f == format::r8g8b8a8_unorm || f == format::r8g8b8a8_unorm_srgb ||
         f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb ||
         f == format::r10g10b10a2_unorm;
}

DXGI_FORMAT SharedColorFormat(format f) {
  if (f == format::r10g10b10a2_unorm) return DXGI_FORMAT_R10G10B10A2_UNORM;
  if (f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb)
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  return DXGI_FORMAT_R8G8B8A8_UNORM;
}

void Rendered(effect_runtime* runtime, effect_technique technique, command_list* list,
              resource_view target, resource_view) {
  auto* state = runtime->get_private_data<CaptureState>();
  if (!state || state->failed || !state->request.token || state->queued) return;
  const auto capture = runtime->find_technique(kEffect, "NDisplayCaptureDepth");
  if (technique != capture) return;
  const int64_t now = Qpc();
  if (now < state->nextCapture || now >= state->request.expires) return;

  auto* device = runtime->get_device();
  const resource color = device->get_resource_from_view(target);
  const auto colorDesc = device->get_resource_desc(color).texture;
  const uint32_t width = state->request.width, height = state->request.height;
  if (colorDesc.width != width || colorDesc.height != height || colorDesc.samples != 1 ||
      !SupportedColor(colorDesc.format)) return;

  const auto input = runtime->find_texture_variable(kEffect, "NDisplayDepthInput");
  const auto output = runtime->find_texture_variable(kEffect, "NDisplayDepthMap");
  resource_view inputView{}, outputView{};
  runtime->get_texture_binding(input, &inputView, nullptr);
  runtime->get_texture_binding(output, &outputView, nullptr);
  resource depth{};
  if (inputView.handle && outputView.handle) {
    const auto scene = device->get_resource_desc(device->get_resource_from_view(inputView)).texture;
    const resource map = device->get_resource_from_view(outputView);
    const auto mapDesc = device->get_resource_desc(map).texture;
    // The main scene can be scaled by the game, but must keep the output's aspect ratio.
    const double aspect = double(scene.width) * height / (double(scene.height) * width);
    if (aspect >= 0.97 && aspect <= 1.03 && mapDesc.format == format::r16_unorm &&
        mapDesc.width == width / 4 && mapDesc.height == height / 16 * 4) depth = map;
  }

  if (!state->gpuFence.handle && !device->create_fence(0, fence_flags::none, &state->gpuFence)) {
    state->failed = true;
    return;
  }
  size_t index = state->slots.size();
  {
    Mapping::Access access(state->mapping);
    if (!access.frames || access.frames->producerId != state->producerId ||
        access.frames->request.token != state->request.token) return;
    RememberReleases(*state, *access.frames);
    const uint64_t released = state->releaseFence ? state->releaseFence->GetCompletedValue() : 0;
    if (released == UINT64_MAX) {
      state->failed = true;
      return;
    }
    for (size_t i = 0; i < state->slots.size(); ++i) {
      const auto& candidate = state->slots[i];
      const auto& current = access.frames->frame;
      if (!candidate.fenceValue && (!current.sequence || current.slot != i) &&
          candidate.consumerRelease <= released) {
        index = i;
        break;
      }
    }
  }
  if (index == state->slots.size()) return;  // never wait for the host or GPU
  auto& slot = state->slots[index];
  auto* native = reinterpret_cast<ID3D12Device*>(device->get_native());
  if (!PrepareColor(native, *state, slot, width, height, SharedColorFormat(colorDesc.format))) {
    reshade::log::message(reshade::log::level::warning,
                         "NDisplay: shared GPU colour is unavailable; desktop capture continues.");
    state->failed = true;
    return;
  }

  slot.frame = {state->request.token, now, width, height, depth.handle != 0, uint32_t(index),
                state->nextFence + 1, reinterpret_cast<uint64_t>(slot.colorHandle.Get()),
                slot.colorId};
  slot.depthPitch = Align((width / 4) * 2, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const uint64_t bytes = uint64_t(slot.depthPitch) * (height / 16 * 4);
  if (depth.handle && slot.capacity < bytes) {
    if (slot.buffer.handle) device->destroy_resource(slot.buffer);
    slot.capacity = 0;
    slot.buffer = {};
    const resource_desc desc(bytes, memory_heap::readback, resource_usage::copy_dest);
    if (!device->create_resource(desc, nullptr, resource_usage::copy_dest, &slot.buffer)) return;
    slot.capacity = bytes;
  }

  const resource sharedColor{reinterpret_cast<uint64_t>(slot.color.Get())};
  // The shared surface crosses the API boundary in COMMON, after production
  // completed and before the host signals release of its D3D11 copy.
  list->barrier(sharedColor, resource_usage::general, resource_usage::copy_dest);
  list->barrier(color, resource_usage::render_target, resource_usage::copy_source);
  list->copy_texture_region(color, 0, nullptr, sharedColor, 0, nullptr);
  list->barrier(color, resource_usage::copy_source, resource_usage::render_target);
  list->barrier(sharedColor, resource_usage::copy_dest, resource_usage::general);
  if (depth.handle) {
    list->barrier(depth, resource_usage::shader_resource, resource_usage::copy_source);
    list->copy_texture_to_buffer(depth, 0, nullptr, slot.buffer, 0);
    list->barrier(depth, resource_usage::copy_source, resource_usage::shader_resource);
  }
  state->queued = &slot;
  state->nextCapture = now + state->frequency * 97 / (100 * state->request.fps);
}

void Present(effect_runtime* runtime) {
  auto* state = runtime->get_private_data<CaptureState>();
  if (!state || !state->queued) return;
  state->queued->fenceValue = ++state->nextFence;
  if (!runtime->get_command_queue()->signal(state->gpuFence, state->nextFence)) state->failed = true;
  state->queued = nullptr;
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "NDisplay Game Depth";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Streams paired colour and depth to NDisplayHost.";

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    if (!reshade::register_addon(module)) return FALSE;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(Init);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(Destroy);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(Begin);
    reshade::register_event<reshade::addon_event::reshade_render_technique>(Rendered);
    reshade::register_event<reshade::addon_event::reshade_present>(Present);
  } else if (reason == DLL_PROCESS_DETACH) {
    reshade::unregister_addon(module);
  }
  return TRUE;
}
