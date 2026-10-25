#include "native_gl_surface.h"
#include <GL/gl.h>
#include <dxgi.h>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
template<class T> T GL(const NativeGLSurface& surface, const char* name) {
  const auto proc = surface.Resolve(name);
  if (!proc) throw std::runtime_error(name);
  return reinterpret_cast<T>(proc);
}
using OpenDevice = HANDLE(WINAPI*)(void*);
using CloseDevice = BOOL(WINAPI*)(HANDLE);
using RegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using UnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
using LockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
using Textures = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteTextures = void(APIENTRY*)(GLsizei, const GLuint*);
using Bind = void(APIENTRY*)(GLenum, GLuint);
using Attach = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using Status = GLenum(APIENTRY*)(GLenum);
constexpr GLenum Framebuffer = 0x8D40, ColorAttachment = 0x8CE0;
void Require(bool ok, const char* operation) {
  if (!ok) throw std::runtime_error(operation);
}
}

void* NativeGLSurface::Resolve(const char* name) const {
  const auto proc = wglGetProcAddress(name);
  const auto value = reinterpret_cast<intptr_t>(proc);
  return value != 0 && value != 1 && value != 2 && value != 3 && value != -1
      ? reinterpret_cast<void*>(proc)
      : reinterpret_cast<void*>(::GetProcAddress(opengl_, name));
}

std::unique_ptr<NativeGLSurface> NativeGLSurface::TryCreate(int width, int height) {
  auto surface = std::unique_ptr<NativeGLSurface>(new NativeGLSurface());
  // Compatibility is decided from the driver/vendor and extension support,
  // never from frame rate, workload or the selected enhancement quality.
  try {
    if (!surface->Initialize()) return nullptr;
    surface->Resize(width, height);
  } catch (const std::exception& error) {
    std::cout << "AniBaka: NVIDIA GL interop unavailable: " << error.what() << '\n';
    return nullptr;
  }
  std::cout << "AniBaka: native NVIDIA OpenGL / CUDA / D3D11 interop renderer\n";
  return surface;
}

bool NativeGLSurface::Initialize() {
  opengl_ = LoadLibraryW(L"opengl32.dll");
  if (!opengl_) return false;
  static std::once_flag registered;
  std::call_once(registered, [] {
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AniBakaNativeGL";
    RegisterClassW(&wc);
  });
  // Offscreen context only; this window is never shown or used for UI.
  window_ = CreateWindowW(L"AniBakaNativeGL", L"", WS_POPUP, 0, 0, 1, 1,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window_) return false;
  dc_ = GetDC(window_);
  PIXELFORMATDESCRIPTOR format{};
  format.nSize = sizeof(format); format.nVersion = 1;
  format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32;
  const int pixel = ChoosePixelFormat(dc_, &format);
  if (!pixel || !SetPixelFormat(dc_, pixel, &format)) return false;
  gl_ = wglCreateContext(dc_);
  if (!gl_ || !wglMakeCurrent(dc_, gl_)) return false;
  const auto getString = GL<const GLubyte*(APIENTRY*)(GLenum)>(*this, "glGetString");
  const auto vendor = reinterpret_cast<const char*>(getString(GL_VENDOR));
  if (!vendor || !std::strstr(vendor, "NVIDIA")) return false;
  if (!Resolve("wglDXOpenDeviceNV") || !Resolve("wglDXRegisterObjectNV") ||
      !Resolve("wglDXLockObjectsNV") || !Resolve("wglDXUnlockObjectsNV")) return false;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "Open DXGI factory");
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
    if (desc.VendorId == 0x10de) break;
    adapter.Reset();
  }
  if (!adapter) return false;
  Require(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      &device_, nullptr, &context_)), "Create native GL sharing device");
  interop_device_ = GL<OpenDevice>(*this, "wglDXOpenDeviceNV")(device_.Get());
  Require(interop_device_ != nullptr, "Open NVIDIA D3D11 interop device");
  return true;
}

void NativeGLSurface::MakeCurrent(bool current) {
  Require(wglMakeCurrent(current ? dc_ : nullptr, current ? gl_ : nullptr) == TRUE,
      "Set native OpenGL context");
}

void NativeGLSurface::ReleaseTextures() {
  if (interop_texture_) {
    if (locked_) GL<LockObjects>(*this, "wglDXUnlockObjectsNV")(interop_device_, 1, &interop_texture_);
    locked_ = false;
    GL<UnregisterObject>(*this, "wglDXUnregisterObjectNV")(interop_device_, interop_texture_);
    interop_texture_ = nullptr;
  }
  if (framebuffer_) GL<DeleteTextures>(*this, "glDeleteFramebuffers")(1, &framebuffer_);
  if (texture_) GL<DeleteTextures>(*this, "glDeleteTextures")(1, &texture_);
  framebuffer_ = texture_ = 0;
  input_.Reset(); output_.Reset(); handle_ = nullptr;
}

void NativeGLSurface::Resize(int width, int height) {
  MakeCurrent(true);
  ReleaseTextures();
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(width); desc.Height = static_cast<UINT>(height);
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  Require(SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &input_)), "Create GL input texture");
  Require(SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &output_)), "Create GL output texture");
  Microsoft::WRL::ComPtr<IDXGIResource> resource;
  Require(SUCCEEDED(output_.As(&resource)) && SUCCEEDED(resource->GetSharedHandle(&handle_)), "Export GL texture");
  GL<Textures>(*this, "glGenTextures")(1, &texture_);
  interop_texture_ = GL<RegisterObject>(*this, "wglDXRegisterObjectNV")(
      interop_device_, input_.Get(), texture_, GL_TEXTURE_2D, 1 /* WGL_ACCESS_READ_WRITE_NV */);
  Require(interop_texture_ != nullptr, "Register NVIDIA shared GL texture");
  Require(GL<LockObjects>(*this, "wglDXLockObjectsNV")(interop_device_, 1, &interop_texture_) == TRUE, "Lock shared GL texture");
  locked_ = true;
  GL<Textures>(*this, "glGenFramebuffers")(1, &framebuffer_);
  GL<Bind>(*this, "glBindFramebuffer")(Framebuffer, framebuffer_);
  GL<Attach>(*this, "glFramebufferTexture2D")(Framebuffer, ColorAttachment, GL_TEXTURE_2D, texture_, 0);
  Require(GL<Status>(*this, "glCheckFramebufferStatus")(Framebuffer) == 0x8CD5, "Native framebuffer incomplete");
  Require(GL<LockObjects>(*this, "wglDXUnlockObjectsNV")(interop_device_, 1, &interop_texture_) == TRUE, "Unlock shared GL texture");
  locked_ = false;
}

void NativeGLSurface::Draw(const std::function<void()>& render) {
  MakeCurrent(true);
  Require(GL<LockObjects>(*this, "wglDXLockObjectsNV")(interop_device_, 1, &interop_texture_) == TRUE, "Lock native frame");
  locked_ = true;
  try {
    GL<Bind>(*this, "glBindFramebuffer")(Framebuffer, framebuffer_);
    render();
  } catch (...) {
    GL<LockObjects>(*this, "wglDXUnlockObjectsNV")(interop_device_, 1, &interop_texture_);
    locked_ = false;
    throw;
  }
  Require(GL<LockObjects>(*this, "wglDXUnlockObjectsNV")(interop_device_, 1, &interop_texture_) == TRUE, "Unlock native frame");
  locked_ = false;
  MakeCurrent(false);
}

void NativeGLSurface::Read() {
  context_->CopyResource(output_.Get(), input_.Get());
  context_->Flush();
}

NativeGLSurface::~NativeGLSurface() {
  if (gl_) {
    try {
      if (wglMakeCurrent(dc_, gl_)) {
        ReleaseTextures();
        if (interop_device_) GL<CloseDevice>(*this, "wglDXCloseDeviceNV")(interop_device_);
      }
    } catch (...) {
      // Device loss must not throw from destruction or terminate the app.
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(gl_);
  }
  if (dc_) ReleaseDC(window_, dc_);
  if (window_) DestroyWindow(window_);
  if (opengl_) FreeLibrary(opengl_);
}
