#pragma once
#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string>
#include <cstdint>
#include <wrl/client.h>

// A completed, immutable image lease. It never owns an NGX instance or context.
struct DlssVideoFrame {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  HANDLE handle=nullptr;
  unsigned width=0,height=0;
  uint64_t sequence=0;
  bool generated=false;
};

struct DlssVideoSettings {
  bool enabled=false;
  float intensity=.7f;
  float sharpness=.35f;
  int maxHeight=1080;
  bool superResolution=true, frameGeneration=true;
  bool neuralRendering=false, comparison=false;
  std::wstring runtime, cache, fgRuntime;
  uint64_t revision=0;
};
DlssVideoSettings GetDlssVideoSettings();
void SetDlssVideoSettings(DlssVideoSettings settings);
void SetDlssVideoStatus(int64_t owner, const std::string& status);
void RemoveDlssVideoStatus(int64_t owner);
std::string GetDlssVideoStatus();

// D3D11 (media_kit) -> shared D3D12 input -> NR/SR/FG -> D3D11 Flutter texture.
// GPU work stays on the video worker. AcquireFrame never waits for the GPU.
class DlssVideoBridge {
 public:
  DlssVideoBridge(ID3D11Texture2D* source, const DlssVideoSettings& settings, double fps);
  ~DlssVideoBridge();
  bool Prepare(ID3D11Texture2D* source, bool reset);
  bool Publish(bool generated);
  bool CanReuse(unsigned width,unsigned height,const DlssVideoSettings& settings) const;
  void UpdateSettings(const DlssVideoSettings& settings);
  std::shared_ptr<const DlssVideoFrame> AcquireFrame();
  unsigned width() const;
  unsigned height() const;
  bool wasRead() const;
  uint64_t generatedSubmitted() const;
  uint64_t generatedRead() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
