#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <string>

// This software contains source code provided by NVIDIA Corporation.
// NGX owns these parameter maps and feature handles; release before shutdown.
class NgxFeatures {
 public:
  ~NgxFeatures();
  void Initialize(ID3D12GraphicsCommandList* list,
                  unsigned width, unsigned height, unsigned scale,
                  bool frameGeneration, double fps);
  void SuperResolve(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
                    ID3D12Resource* depth, ID3D12Resource* motion,
                    ID3D12Resource* output, bool reset);
  void Generate(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
                ID3D12Resource* depth, ID3D12Resource* motion,
                ID3D12Resource* output, ID3D12Resource* disable, bool reset);
  void Close();
 private:
  NVSDK_NGX_Parameter *sr_ = nullptr, *fg_ = nullptr, *caps_ = nullptr;
  NVSDK_NGX_Handle *srHandle_ = nullptr, *fgHandle_ = nullptr;
  float identity_[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float projection_[16]{}, inverse_[16]{};
};
