#pragma once
// Shared synchronization from AniBaka's GPLv3 GPU backend; see dlss_realtime/LICENSE.
#include <d3d11_4.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <cstdint>

class GpuCompletion {
 public:
  explicit GpuCompletion(ID3D11DeviceContext* context) {
    Check(context->QueryInterface(IID_PPV_ARGS(&context_)));
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    Check(device.As(&device_));
    Check(device_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) throw std::runtime_error("Cannot create GPU completion event");
  }
  ~GpuCompletion() { CloseHandle(event_); }
  GpuCompletion(const GpuCompletion&) = delete;
  GpuCompletion& operator=(const GpuCompletion&) = delete;
  void Wait() {
    Check(context_->Signal(fence_.Get(), ++serial_));
    context_->Flush();
    if (fence_->GetCompletedValue() < serial_) {
      Check(fence_->SetEventOnCompletion(serial_, event_));
      if (WaitForSingleObject(event_, 2000) != WAIT_OBJECT_0) throw std::runtime_error("GPU completion timed out");
    }
    Check(device_->GetDeviceRemovedReason());
  }
 private:
  static void Check(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("D3D11 completion failed: " + std::to_string(result));
  }
  Microsoft::WRL::ComPtr<ID3D11Device5> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
  Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
  HANDLE event_ = nullptr;
  uint64_t serial_ = 0;
};
