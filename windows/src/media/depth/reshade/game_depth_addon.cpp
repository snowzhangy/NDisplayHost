#include "depth/game_depth_ipc.h"

#include <d3d12.h>
#include <reshade.hpp>

#include <array>
#include <cstring>

using namespace reshade::api;
using namespace wvd::game_depth;

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
  format colorFormat{};
  uint32_t colorPitch = 0, depthPitch = 0, depthOffset = 0;
};

struct __declspec(uuid("e9f06082-1f6e-4f32-becc-6f660c2d1f29")) CaptureState {
  Mapping mapping;
  int64_t frequency = 0;
  int64_t nextOpen = 0, nextCapture = 0;
  Request request;
  fence gpuFence{};
  uint64_t nextFence = 0;
  std::array<Readback, 3> slots;
  Readback* queued = nullptr;
  bool failed = false;
  bool depthLogged = false;
};

void Init(effect_runtime* runtime) {
  if (runtime->get_device()->get_api() != device_api::d3d12) return;
  auto* state = runtime->create_private_data<CaptureState>();
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

bool ReadRequest(effect_runtime* runtime, CaptureState& state, int64_t now, Request& request) {
  if (now < state.nextOpen) return false;
  if (!state.mapping.Open(false)) {
    state.nextOpen = now + state.frequency / 4;
    return false;
  }
  const auto window = static_cast<HWND>(runtime->get_hwnd());
  if (!window || window != GetForegroundWindow()) return false;
  Mapping::Access access(state.mapping);
  if (!access.frames) return false;
  request = access.frames->request;
  if (!request.token || request.expires <= now || request.window != reinterpret_cast<uint64_t>(window) ||
      !request.fps || !SupportedSize(request.width, request.height)) return false;
  auto* device = reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native());
  return SameAdapter(request.adapter, device->GetAdapterLuid());
}

void CopyColor(const Readback& slot, const uint8_t* src, uint32_t* dst) {
  const uint32_t width = slot.frame.width;
  for (uint32_t y = 0; y < slot.frame.height; ++y, src += slot.colorPitch, dst += width) {
    const auto* row = reinterpret_cast<const uint32_t*>(src);
    if (slot.colorFormat == format::b8g8r8a8_unorm || slot.colorFormat == format::b8g8r8a8_unorm_srgb) {
      std::memcpy(dst, row, size_t(width) * sizeof(*dst));
    } else if (slot.colorFormat == format::r10g10b10a2_unorm) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t rgba = row[x];
        dst[x] = 0xff000000u | ((rgba & 0x3fcu) << 14) |
                 ((rgba >> 4) & 0xff00u) | ((rgba >> 22) & 0xffu);
      }
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t rgba = row[x];
        dst[x] = (rgba & 0xff00ff00u) | ((rgba & 0xffu) << 16) | ((rgba >> 16) & 0xffu);
      }
    }
  }
}

void Publish(device* device, CaptureState& state, const Readback& slot, int64_t now) {
  if (now - slot.frame.qpc >= state.frequency / 10) return;
  void* mapped = nullptr;
  if (!device->map_buffer_region(slot.buffer, 0, slot.capacity, map_access::read_only, &mapped)) return;
  {
    Mapping::Access access(state.mapping);
    if (access.frames && access.frames->request.token == slot.frame.token &&
        access.frames->request.expires > now) {
      auto& shared = *access.frames;
      const auto* bytes = static_cast<const uint8_t*>(mapped);
      CopyColor(slot, bytes, shared.bgra);
      Frame frame = slot.frame;
      if (frame.depthValid) {
        const uint32_t width = frame.width / 4, height = frame.height / 16 * 4;
        const auto* src = bytes + slot.depthOffset;
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
      shared.frame = frame;
      if (frame.depthValid && !state.depthLogged) {
        reshade::log::message(reshade::log::level::info, "NDisplay: exporting rendered colour and scene depth.");
        state.depthLogged = true;
      }
    }
  }
  device->unmap_buffer_region(slot.buffer);
}

void Begin(effect_runtime* runtime, command_list*, resource_view, resource_view) {
  auto* state = runtime->get_private_data<CaptureState>();
  if (!state || state->failed) return;
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

  const auto uniform = runtime->find_uniform_variable(kEffect, "NDisplayCaptureEnabled");
  if (uniform.handle) runtime->set_uniform_value_bool(uniform, active && now >= state->nextCapture);
}

bool SupportedColor(format f) {
  return f == format::r8g8b8a8_unorm || f == format::r8g8b8a8_unorm_srgb ||
         f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb ||
         f == format::r10g10b10a2_unorm;
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
  Readback* slot = nullptr;
  for (auto& candidate : state->slots) {
    if (!candidate.fenceValue) { slot = &candidate; break; }
  }
  if (!slot) return;  // the GPU is behind; keep rendering and skip this export

  slot->frame = {state->request.token, now, width, height, depth.handle != 0};
  slot->colorFormat = colorDesc.format;
  slot->colorPitch = Align(width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  slot->depthOffset = Align(slot->colorPitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  slot->depthPitch = Align((width / 4) * 2, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const uint64_t bytes = slot->depthOffset + uint64_t(slot->depthPitch) * (height / 16 * 4);
  if (slot->capacity < bytes) {
    if (slot->buffer.handle) device->destroy_resource(slot->buffer);
    slot->capacity = 0;
    slot->buffer = {};
    const resource_desc desc(bytes, memory_heap::readback, resource_usage::copy_dest);
    if (!device->create_resource(desc, nullptr, resource_usage::copy_dest, &slot->buffer)) return;
    slot->capacity = bytes;
  }

  list->barrier(color, resource_usage::render_target, resource_usage::copy_source);
  list->copy_texture_to_buffer(color, 0, nullptr, slot->buffer, 0);
  list->barrier(color, resource_usage::copy_source, resource_usage::render_target);
  if (depth.handle) {
    list->barrier(depth, resource_usage::shader_resource, resource_usage::copy_source);
    list->copy_texture_to_buffer(depth, 0, nullptr, slot->buffer, slot->depthOffset);
    list->barrier(depth, resource_usage::copy_source, resource_usage::shader_resource);
  }
  state->queued = slot;
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
