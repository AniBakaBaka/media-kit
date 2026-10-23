#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <functional>
#include <memory>

// NVIDIA's desktop OpenGL driver can import CUDA frames. ANGLE's GL objects
// belong to D3D11 and cannot be registered by cuGraphicsGLRegisterImage.
// NV_DX_interop2 exports the final GL framebuffer back to Flutter/DLSS as D3D11.
class NativeGLSurface {
 public:
  static std::unique_ptr<NativeGLSurface> TryCreate(int width, int height);
  ~NativeGLSurface();
  void Resize(int width, int height);
  void Draw(const std::function<void()>& render);
  void Read();
  void MakeCurrent(bool current);
  void* Resolve(const char* name) const;
  unsigned framebuffer() const { return framebuffer_; }
  HANDLE handle() const { return handle_; }
  ID3D11Texture2D* source() const { return input_.Get(); }
  ID3D11Texture2D* output() const { return output_.Get(); }

 private:
  NativeGLSurface() = default;
  bool Initialize();
  void ReleaseTextures();
  HWND window_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC gl_ = nullptr;
  HANDLE interop_device_ = nullptr, interop_texture_ = nullptr, handle_ = nullptr;
  unsigned framebuffer_ = 0, texture_ = 0;
  bool locked_ = false;
  HMODULE opengl_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> input_, output_;
};
