#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <windows.h>

// D3D11 owns the interchange textures. D3D12 imports their NT handles.
// Output textures only need copy access; NGX UAVs remain private to D3D12.
struct RealtimeSharedTextures {
  HANDLE input=nullptr, real=nullptr, generated=nullptr;
};

// Owns persistent NR/SR/FG features and a three-slot D3D12 submission ring.
// media_kit supplies BGRA textures; enhanced pixels stay in GPU memory.
class RealtimeGpu {
 public:
  RealtimeGpu(unsigned width, unsigned height,
              const std::wstring& runtime, const std::wstring& cache,
              float intensity, unsigned scale, const std::wstring& fgRuntime,
              double fps, bool neuralRendering, const LUID& adapter,
              const RealtimeSharedTextures& shared);
  ~RealtimeGpu();
  // Prepare once, then present the midpoint and real image at their deadlines.
  bool Prepare(bool reset);
  void SetIntensity(float intensity);
  void SetFrameGenerationEnabled(bool enabled);
  void SetNeuralRenderingEnabled(bool enabled);
  bool Prepared();
  void WaitPrepared();
  bool InterpolationAllowed();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
