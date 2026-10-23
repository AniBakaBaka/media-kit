#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// OFF/ON panels (768x240 each), followed by a 64px digit strip. Static artwork
// and resolutions are drawn once; the shader composes live FPS from digits.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateComparisonLabels(
    ID3D11Device* device,unsigned inputWidth,unsigned inputHeight,unsigned outputWidth,unsigned outputHeight);
