#include "capture/dda_capture.h"
#include "preprocess/nv12_convert.h"
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
using namespace wvd;
int main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
    nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
  Nv12Converter converter;
  CHECK(SUCCEEDED(converter.Init(device.Get(), context.Get())));
  struct Case { uint32_t bgra; int y, u, v; };
  const Case cases[] = {{0xFFFF0000,54,98,255}, {0xFF0000FF,18,255,116},
                        {0xFF00FF00,182,29,12}, {0xFFFFFFFF,255,128,128},
                        {0xFF000000,0,128,128}};
  for (const auto& sample : cases) {
    uint32_t pixels[4] = {sample.bgra,sample.bgra,sample.bgra,sample.bgra};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 2; desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels, 8, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)));
    CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv)));
    for (auto filter : {ChromaFilter::Box2x2, ChromaFilter::TextAware}) {
      std::vector<uint8_t> result;
      CHECK(SUCCEEDED(converter.Convert(srv.Get(), 2, 2, filter, result)));
      CHECK(result.size() == 6);
      for (unsigned i = 0; i < 4; ++i) CHECK(abs(int(result[i]) - sample.y) <= 1);
      CHECK(abs(int(result[4]) - sample.u) <= 1);
      CHECK(abs(int(result[5]) - sample.v) <= 1);
      CHECK(converter.Convert(srv.Get(), 1, 2, filter, result) == E_INVALIDARG);
    }
  }
  puts("GPU BGRA-to-NV12 BT.709 full-range colors passed");

  // Full-size picture through the session's path: ring slot (CPU upload) →
  // NV12 texture → CPU readback. Catches pitch/plane-layout and pool bugs
  // that a 2×2 solid colour cannot (row pitch is padded at this width).
  {
    const UINT w = 1280, h = 720;
    FrameRing ring;
    CHECK(SUCCEEDED(ring.Init(device.Get(), w, h, 3)));
    std::vector<uint32_t> frame(size_t(w) * h);
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x)
        frame[size_t(y) * w + x] =
            0xFF000000u | ((x & 0xFF) << 16) | (((y * 3) & 0xFF) << 8) | ((x ^ y) & 0xFF);
    const UINT slot = ring.AcquireWriteSlot();
    ring.WriteCpuFrame(slot, reinterpret_cast<const uint8_t*>(frame.data()), w * 4);
    ID3D11Texture2D* nv12 = nullptr;
    CHECK(SUCCEEDED(converter.ConvertToTexture(ring.SlotSrv(slot), w, h, w, h,
                                               ChromaFilter::Box2x2, &nv12)));
    std::vector<uint8_t> out;
    CHECK(SUCCEEDED(converter.Readback(nv12, out)));
    CHECK(out.size() == size_t(w) * h * 3 / 2);
    auto luma = [](uint32_t p) {
      const double r = ((p >> 16) & 0xFF) / 255.0, g = ((p >> 8) & 0xFF) / 255.0,
                   b = (p & 0xFF) / 255.0;
      return int(std::lround((0.2126 * r + 0.7152 * g + 0.0722 * b) * 255.0));
    };
    int worst = 0;
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x)
        worst = (std::max)(worst, abs(int(out[size_t(y) * w + x]) - luma(frame[size_t(y) * w + x])));
    if (worst > 1) {
      int shown = 0;
      for (UINT y = 0; y < h && shown < 12; y += 37)
        for (UINT x = 0; x < w && shown < 12; x += 211) {
          const int got = out[size_t(y) * w + x], want = luma(frame[size_t(y) * w + x]);
          fprintf(stderr, "  (%u,%u) got=%d want=%d\n", x, y, got, want);
          ++shown;
        }
      fprintf(stderr, "worst luma error %d\n", worst);
    }
    CHECK(worst <= 1);
    // Chroma plane: every sample is the 2×2 box average of its quad.
    auto chroma = [](uint32_t p, int which) {
      const double r = ((p >> 16) & 0xFF) / 255.0, g = ((p >> 8) & 0xFF) / 255.0,
                   b = (p & 0xFF) / 255.0;
      const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      return which == 0 ? (b - y) / 1.8556 + 0.5 : (r - y) / 1.5748 + 0.5;
    };
    int worstUv = 0;
    for (UINT cy = 0; cy < h / 2; ++cy)
      for (UINT cx = 0; cx < w / 2; ++cx)
        for (int c = 0; c < 2; ++c) {
          double sum = 0;
          for (UINT dy = 0; dy < 2; ++dy)
            for (UINT dx = 0; dx < 2; ++dx)
              sum += chroma(frame[size_t(cy * 2 + dy) * w + cx * 2 + dx], c);
          const int want = int(std::lround(sum / 4.0 * 255.0));
          const int got = out[size_t(w) * h + size_t(cy) * w + cx * 2 + c];
          worstUv = (std::max)(worstUv, abs(got - want));
        }
    if (worstUv > 1) fprintf(stderr, "worst chroma error %d\n", worstUv);
    CHECK(worstUv <= 1);
    puts("GPU 1280x720 ring -> NV12 texture -> readback matches per pixel");

    // Scaled to half size: a uniform picture stays uniform.
    std::fill(frame.begin(), frame.end(), 0xFF0000FFu);
    ring.WriteCpuFrame(slot, reinterpret_cast<const uint8_t*>(frame.data()), w * 4);
    CHECK(SUCCEEDED(converter.ConvertToTexture(ring.SlotSrv(slot), w, h, w / 2, h / 2,
                                               ChromaFilter::TextAware, &nv12)));
    CHECK(SUCCEEDED(converter.Readback(nv12, out)));
    CHECK(out.size() == size_t(w / 2) * (h / 2) * 3 / 2);
    for (size_t i = 0; i < size_t(w / 2) * (h / 2); ++i) CHECK(abs(int(out[i]) - 18) <= 1);
    CHECK(abs(int(out[size_t(w / 2) * (h / 2)]) - 255) <= 1);
    puts("GPU scaled conversion (1280x720 -> 640x360) passed");
  }
}
