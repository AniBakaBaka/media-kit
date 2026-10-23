#include "video_bridge.h"
#include "gpu.h"
#include "common.h"
#include "comparison_labels.h"
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <map>
#include <mutex>
#include <atomic>
#include <array>
#include <algorithm>

using Microsoft::WRL::ComPtr;
namespace {
std::mutex settingsMutex;
DlssVideoSettings settingsValue;
std::map<int64_t,std::string> statuses;
}
DlssVideoSettings GetDlssVideoSettings() {
  std::lock_guard<std::mutex> lock(settingsMutex); return settingsValue;
}
void SetDlssVideoSettings(DlssVideoSettings settings) {
  std::lock_guard<std::mutex> lock(settingsMutex);
  settings.revision=settingsValue.revision+1; settingsValue=std::move(settings);
  statuses.clear();
}
void SetDlssVideoStatus(int64_t owner,const std::string& status) {
  bool changed=false;
  {
    std::lock_guard<std::mutex> lock(settingsMutex);
    auto& previous=statuses[owner];
    changed=previous.substr(0,previous.find(" ·"))!=status.substr(0,status.find(" ·"));
    previous=status;
  }
  // Console I/O must not hold the mutex used by Flutter's platform thread.
  if(changed)
    LogInfo("[DLSS playback %lld] %s",static_cast<long long>(owner),status.c_str());
}
void RemoveDlssVideoStatus(int64_t owner) {
  std::lock_guard<std::mutex> lock(settingsMutex); statuses.erase(owner);
}
std::string GetDlssVideoStatus() {
  std::lock_guard<std::mutex> lock(settingsMutex);
  if (!settingsValue.enabled) return "已关闭";
  if (statuses.empty()) return "已开启，等待视频画面";
  std::string result;
  for (const auto& entry:statuses) { if (!result.empty()) result+="\n"; result+=entry.second; }
  return result;
}
struct DlssVideoBridge::Impl {
  unsigned w=0,h=0;
  unsigned inputWidth=0,inputHeight=0;
  DlssVideoSettings settings;
  bool fgAvailable=false;
  uint64_t preparedSerial=0,countedSerial=0,rateStart=0,rateLast=0,rateCount=0,rateRealCount=0;
  uint64_t generatedSubmitted=0;
  std::atomic<uint64_t> lastReadSequence{0},generatedRead{0};
  unsigned countedOutputs=0;
  float submittedFps=0,sourceFps=0;
  bool rateReady=false;
  std::atomic<bool> read{false};
  ComPtr<ID3D11Device5> device;
  ComPtr<ID3D11DeviceContext4> context;
  ComPtr<ID3D11Fence> fence;
  Handle completion{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
  uint64_t fenceValue=0;
  ComPtr<ID3D11Texture2D> input,real,generated,source;
  ComPtr<ID3D11Texture2D> previousInput;
  ComPtr<ID3D11ShaderResourceView> previousInputView;
  bool referenceReady=false;
  ComPtr<ID3D11Texture2D> sourceIdentity;
  std::array<ComPtr<ID3D11ShaderResourceView>,2> outputViews;
  ComPtr<ID3D11VertexShader> presentVertex;
  ComPtr<ID3D11PixelShader> presentPixel;
  ComPtr<ID3D11RasterizerState> presentRasterizer;
  ComPtr<ID3D11Buffer> presentConstants;
  ComPtr<ID3D11ShaderResourceView> inputView;
  ComPtr<ID3D11ShaderResourceView> comparisonLabels;
  ComPtr<ID3D11SamplerState> presentSampler;
  std::shared_ptr<const DlssVideoFrame> ready;
  std::unique_ptr<RealtimeGpu> gpu;
  void EnsureComparison() {
    if(comparisonLabels) return;
    comparisonLabels=CreateComparisonLabels(device.Get(),inputWidth,inputHeight,w,h);
    D3D11_TEXTURE2D_DESC desc{}; input->GetDesc(&desc);
    desc.MiscFlags=0; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    Check(device->CreateTexture2D(&desc,nullptr,&previousInput),"Comparison previous input");
    Check(device->CreateShaderResourceView(previousInput.Get(),nullptr,&previousInputView),"Comparison previous input view");
  }
  // Finish copies before either API reuses its interchange texture.
  // Fence events avoid polling sleeps that round up to the OS timer tick.
  void Finish() {
    Check(context->Signal(fence.Get(),++fenceValue),"Signal D3D11 transfer");
    context->Flush();
    if(fence->GetCompletedValue()<fenceValue) {
      Check(fence->SetEventOnCompletion(fenceValue,completion.value),"D3D11 transfer event");
      Require(WaitForSingleObject(completion.value,2000)==WAIT_OBJECT_0,"D3D11 texture transfer timed out");
    }
    Check(device->GetDeviceRemovedReason(),"D3D11 texture transfer");
  }
  void OpenSource(ID3D11Texture2D* texture) {
    if(sourceIdentity.Get()==texture) return;
    ComPtr<IDXGIResource> resource; Check(texture->QueryInterface(IID_PPV_ARGS(&resource)),"Source resource");
    HANDLE handle=nullptr; Check(resource->GetSharedHandle(&handle),"Source shared handle");
    source.Reset(); Check(device->OpenSharedResource(handle,IID_PPV_ARGS(&source)),"Open video texture");
    sourceIdentity=texture;
  }
};
DlssVideoBridge::DlssVideoBridge(ID3D11Texture2D* texture,const DlssVideoSettings& settings,double fps)
    : impl_(std::make_unique<Impl>()) {
  auto& p=*impl_;
  D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
  p.settings=settings; p.fgAvailable=settings.frameGeneration;
  p.inputWidth=desc.Width; p.inputHeight=desc.Height;
  Require(desc.Width>=2 && desc.Height>=2 && desc.Width<=1920 && desc.Height<=1080,
          "DLSS input must be between 2x2 and 1920x1080");
  const unsigned scale=settings.superResolution ? 2u : 1u;
  p.w=desc.Width*scale; p.h=desc.Height*scale;
  ComPtr<ID3D11Device> sourceDevice; texture->GetDevice(&sourceDevice);
  ComPtr<IDXGIDevice> dxgi; Check(sourceDevice.As(&dxgi),"Video DXGI device");
  ComPtr<IDXGIAdapter> adapter; Check(dxgi->GetAdapter(&adapter),"Video adapter");
  DXGI_ADAPTER_DESC adapterDesc{}; Check(adapter->GetDesc(&adapterDesc),"Video adapter details");
  Require(adapterDesc.VendorId==0x10de,"请在 Windows 图形设置中将 AniBaka 设为高性能 NVIDIA GPU，重启后生效");
  ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
  Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"DLSS bridge device");
  Check(device.As(&p.device),"D3D11 fence device"); Check(context.As(&p.context),"D3D11 fence context");
  Require(p.completion.value!=nullptr,"Create D3D11 completion event");
  Check(p.device->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.fence)),"D3D11 transfer fence");
  auto share=[&](unsigned width,unsigned height,DXGI_FORMAT format,
                ComPtr<ID3D11Texture2D>& target,Handle& handle,const char* operation) {
    D3D11_TEXTURE2D_DESC shared{};
    shared.Width=width; shared.Height=height; shared.MipLevels=1; shared.ArraySize=1;
    shared.Format=format; shared.SampleDesc.Count=1; shared.Usage=D3D11_USAGE_DEFAULT;
    shared.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    shared.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
    Check(p.device->CreateTexture2D(&shared,nullptr,&target),operation);
    ComPtr<IDXGIResource1> resource; Check(target.As(&resource),"D3D11 NT shared resource");
    Check(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
        nullptr,&handle.value),"Export D3D11 texture NT handle");
  };
  Handle inputHandle,realHandle,generatedHandle;
  share(desc.Width,desc.Height,DXGI_FORMAT_B8G8R8A8_UNORM,p.input,inputHandle,"Create D3D11 shared video input");
  share(p.w,p.h,DXGI_FORMAT_R8G8B8A8_UNORM,p.real,realHandle,"Create D3D11 shared enhanced output");
  if(settings.frameGeneration)
    share(p.w,p.h,DXGI_FORMAT_R8G8B8A8_UNORM,p.generated,generatedHandle,"Create D3D11 shared generated output");
  const RealtimeSharedTextures shared{inputHandle.value,realHandle.value,generatedHandle.value};
  p.gpu=std::make_unique<RealtimeGpu>(desc.Width,desc.Height,settings.runtime,settings.cache,
      settings.intensity,scale,settings.frameGeneration ? settings.fgRuntime : std::wstring{},fps,
      settings.neuralRendering,adapterDesc.AdapterLuid,shared);
  Check(p.device->CreateShaderResourceView(p.real.Get(),nullptr,&p.outputViews[0]),"Enhanced RGBA source");
  Check(p.device->CreateShaderResourceView(p.input.Get(),nullptr,&p.inputView),"Original comparison source");
  if(settings.comparison) p.EnsureComparison();
  if(settings.frameGeneration)
    Check(p.device->CreateShaderResourceView(p.generated.Get(),nullptr,&p.outputViews[1]),"Generated RGBA source");
  const char vertex[]=R"hlsl(
float4 main(uint id:SV_VertexID):SV_Position {
  float2 p=float2((id==2)?3:-1,(id==1)?3:-1);
  return float4(p,0,1);
})hlsl";
  const char pixel[]=R"hlsl(
Texture2D<float4> source:register(t0);
Texture2D<float4> original:register(t1);
Texture2D<float4> labels:register(t2);
SamplerState linearClamp:register(s0);
cbuffer Controls:register(b0) {
  float sharpness; float comparison; float2 outputSize;
  float submittedFps; float rateReady; float sourceFps; float padding;
}
float4 main(float4 position:SV_Position):SV_Target {
  int2 p=int2(position.xy);
  if(comparison>0) {
    // Same relative position at 720p, 1080p and 4K, including small previews.
    float labelWidth=min(outputSize.x*.25,outputSize.y*1.2);
    float2 labelSize=float2(labelWidth,labelWidth*(240.0/768.0));
    float margin=min(outputSize.x,outputSize.y)*.035;
    bool on=position.x>=outputSize.x*.5;
    float2 origin=float2(on ? outputSize.x-margin-labelSize.x : margin,margin);
    float2 local=position.xy-origin;
    if(all(local>=0) && all(local<labelSize)) {
      float2 uv=local/labelSize;
      float2 panel=uv*float2(768,240);
      if(panel.x>=488 && panel.x<648 && panel.y>=172 && panel.y<236) {
        uint slot=(uint)((panel.x-488)/32);
        uint value=(uint)round(clamp(on ? submittedFps : sourceFps,0,999.9)*10);
        uint digit=11; // Initial sampling window: ---.- rather than fake FPS.
        if(slot==3) digit=10;
        else if(rateReady>0) {
          if(slot==0) digit=value>=1000 ? (value/1000)%10 : 12;
          if(slot==1) digit=value>=100 ? (value/100)%10 : 12;
          if(slot==2) digit=(value/10)%10;
          if(slot==4) digit=value%10;
        }
        uv=float2((digit*32+fmod(panel.x-488,32))/768,(480+panel.y-172)/544);
      } else uv.y=(uv.y*240+(on ? 240 : 0))/544;
      return float4(labels.SampleLevel(linearClamp,uv,0).rgb,1);
    }
    if(abs(position.x-outputSize.x*.5)<1) return float4(1,1,1,1);
    if(position.x<outputSize.x*.5)
      return float4(original.SampleLevel(linearClamp,position.xy/outputSize,0).rgb,1);
  }
  float3 color=source.Load(int3(p,0)).rgb;
  if(sharpness>0) {
    uint w,h; source.GetDimensions(w,h); int2 hi=int2(w-1,h-1);
    float3 n=source.Load(int3(clamp(p+int2(0,-1),0,hi),0)).rgb;
    float3 s=source.Load(int3(clamp(p+int2(0,1),0,hi),0)).rgb;
    float3 e=source.Load(int3(clamp(p+int2(1,0),0,hi),0)).rgb;
    float3 west=source.Load(int3(clamp(p+int2(-1,0),0,hi),0)).rgb;
    // Suppress low-contrast compression noise and keep every channel inside
    // its local range, so subtitles/outlines cannot acquire bright/dark halos.
    float3 localMin=min(color,min(min(n,s),min(e,west)));
    float3 localMax=max(color,max(max(n,s),max(e,west)));
    float contrast=max(localMax.r-localMin.r,max(localMax.g-localMin.g,localMax.b-localMin.b));
    float strength=sharpness*smoothstep(.02,.12,contrast);
    color=clamp(color+clamp((color-(n+s+e+west)*.25)*strength,-.04,.04),localMin,localMax);
  }
  return float4(saturate(color),1);
})hlsl";
  ComPtr<ID3DBlob> vs,ps,errors;
  Check(D3DCompile(vertex,sizeof(vertex)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vs,&errors),"Compile presentation vertex shader");
  Check(D3DCompile(pixel,sizeof(pixel)-1,nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&ps,&errors),"Compile presentation pixel shader");
  Check(p.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&p.presentVertex),"Presentation vertex shader");
  Check(p.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&p.presentPixel),"Presentation pixel shader");
  D3D11_BUFFER_DESC controls{}; controls.ByteWidth=32;
  controls.Usage=D3D11_USAGE_DEFAULT; controls.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  Check(p.device->CreateBuffer(&controls,nullptr,&p.presentConstants),"Presentation controls");
  D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD=D3D11_FLOAT32_MAX;
  Check(p.device->CreateSamplerState(&sampler,&p.presentSampler),"Comparison sampler");
  D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
  Check(p.device->CreateRasterizerState(&raster,&p.presentRasterizer),"Presentation rasterizer");
  p.OpenSource(texture);
  LogInfo("[DLSS bridge] shared textures ready; %ux%u -> %ux%u; intensity %.2f; SR %s; FG %s",
      p.w/scale,p.h/scale,p.w,p.h,static_cast<double>(settings.intensity),
      settings.superResolution ? "on" : "off",settings.frameGeneration ? "on" : "off");
}
DlssVideoBridge::~DlssVideoBridge() = default;
bool DlssVideoBridge::Prepare(ID3D11Texture2D* source,bool reset) {
  auto& p=*impl_;
  p.OpenSource(source);
  p.context->CopyResource(p.input.Get(),p.source.Get()); p.Finish();
  const bool interpolate=p.gpu->Prepare(reset);
  p.gpu->WaitPrepared();
  ++p.preparedSerial;
  return interpolate && p.gpu->InterpolationAllowed();
}
bool DlssVideoBridge::Publish(bool generated) {
  auto& p=*impl_;
  if(generated && p.settings.comparison && !p.referenceReady) return false;
  // Flutter releases our lease as soon as it OPENS the shared texture,
  // before sampling it. COM references on another D3D device are invisible
  // to shared_ptr::use_count, so a published texture must never be rewritten.
  // ANGLE/Direct3D retain the allocation for their own pending GPU reads.
  auto frame=std::make_shared<DlssVideoFrame>(); frame->width=p.w; frame->height=p.h;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width=p.w; desc.Height=p.h; desc.MipLevels=desc.ArraySize=1;
  desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
  desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
  Check(p.device->CreateTexture2D(&desc,nullptr,&frame->texture),"Enhanced Flutter frame");
  ComPtr<IDXGIResource> resource; Check(frame->texture.As(&resource),"Enhanced texture resource");
  Check(resource->GetSharedHandle(&frame->handle),"Enhanced Flutter handle");
  ComPtr<ID3D11RenderTargetView> frameTarget;
  Check(p.device->CreateRenderTargetView(frame->texture.Get(),nullptr,&frameTarget),"Enhanced BGRA target");
  // A shader performs the actual RGBA -> BGRA format conversion. A copy
  // between these unrelated DXGI formats is invalid and can yield black.
  const D3D11_VIEWPORT viewport{0,0,static_cast<float>(p.w),static_cast<float>(p.h),0,1};
  p.context->RSSetViewports(1,&viewport);
  p.context->RSSetState(p.presentRasterizer.Get());
  p.context->IASetInputLayout(nullptr);
  p.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  p.context->VSSetShader(p.presentVertex.Get(),nullptr,0);
  p.context->PSSetShader(p.presentPixel.Get(),nullptr,0);
  const float controls[]={p.settings.sharpness,p.settings.comparison ? 1.f : 0.f,
      static_cast<float>(p.w),static_cast<float>(p.h),p.submittedFps,p.rateReady ? 1.f : 0.f,p.sourceFps,0};
  p.context->UpdateSubresource(p.presentConstants.Get(),0,nullptr,controls,0,0);
  auto constants=p.presentConstants.Get(); p.context->PSSetConstantBuffers(0,1,&constants);
  ID3D11ShaderResourceView* views[]={p.outputViews[generated ? 1 : 0].Get(),
      generated && p.settings.comparison ? p.previousInputView.Get() : p.inputView.Get(),p.comparisonLabels.Get()};
  auto target=frameTarget.Get();
  p.context->PSSetShaderResources(0,3,views);
  auto sampler=p.presentSampler.Get(); p.context->PSSetSamplers(0,1,&sampler);
  p.context->OMSetRenderTargets(1,&target,nullptr);
  p.context->Draw(3,0);
  ID3D11ShaderResourceView* empty[]={nullptr,nullptr,nullptr};
  p.context->PSSetShaderResources(0,3,empty);
  p.context->OMSetRenderTargets(0,nullptr,nullptr);
  // At the midpoint the OFF half holds the last original image, while the
  // ON half advances to the generated image. Advance the OFF reference only
  // when the current real frame is published, never when it is prepared.
  if(!generated && p.settings.comparison) p.context->CopyResource(p.previousInput.Get(),p.input.Get());
  p.Finish();
  if(!generated && p.settings.comparison) p.referenceReady=true;
  frame->sequence=p.preparedSerial*2+(generated ? 0u : 1u); frame->generated=generated;
  std::atomic_store(&p.ready,std::shared_ptr<const DlssVideoFrame>(frame));
  // Count completed, unique output submissions, not the file's nominal FPS
  // or repeated repainting of one image. The OFF half advances only for
  // original frames; the ON half also advances for generated midpoints.
  if(p.countedSerial!=p.preparedSerial) { p.countedSerial=p.preparedSerial; p.countedOutputs=0; }
  const unsigned outputBit=generated ? 2u : 1u;
  if((p.countedOutputs&outputBit)==0) {
    p.countedOutputs|=outputBit;
    if(generated) ++p.generatedSubmitted;
    const auto now=GetTickCount64();
    if(!p.rateStart || now-p.rateLast>2000) {
      p.rateStart=now; p.rateCount=p.rateRealCount=0; p.rateReady=false;
    } else {
      ++p.rateCount;
      if(!generated) ++p.rateRealCount;
      if(now-p.rateStart>=1000) {
        p.submittedFps=static_cast<float>(p.rateCount*1000.0/(now-p.rateStart));
        p.sourceFps=static_cast<float>(p.rateRealCount*1000.0/(now-p.rateStart));
        p.rateReady=true; p.rateStart=now; p.rateCount=p.rateRealCount=0;
      }
    }
    p.rateLast=now;
  }
  return true;
}
std::shared_ptr<const DlssVideoFrame> DlssVideoBridge::AcquireFrame() {
  auto frame=std::atomic_load(&impl_->ready);
  if(frame) {
    impl_->read=true;
    if(impl_->lastReadSequence.exchange(frame->sequence)!=frame->sequence && frame->generated)
      ++impl_->generatedRead;
  }
  return frame;
}
unsigned DlssVideoBridge::width() const { return impl_->w; }
unsigned DlssVideoBridge::height() const { return impl_->h; }
bool DlssVideoBridge::wasRead() const { return impl_->read.load(); }
uint64_t DlssVideoBridge::generatedSubmitted() const { return impl_->generatedSubmitted; }
uint64_t DlssVideoBridge::generatedRead() const { return impl_->generatedRead.load(); }
bool DlssVideoBridge::CanReuse(unsigned width,unsigned height,const DlssVideoSettings& settings) const {
  const auto& p=*impl_;
  return width==p.inputWidth && height==p.inputHeight &&
      settings.runtime==p.settings.runtime && settings.cache==p.settings.cache &&
      settings.superResolution==p.settings.superResolution &&
      (!settings.frameGeneration || (p.fgAvailable && settings.fgRuntime==p.settings.fgRuntime));
}
void DlssVideoBridge::UpdateSettings(const DlssVideoSettings& settings) {
  auto& p=*impl_;
  if(settings.comparison) p.EnsureComparison();
  if(settings.comparison!=p.settings.comparison) {
    p.rateStart=p.rateLast=p.rateCount=p.rateRealCount=0; p.rateReady=false; p.referenceReady=false;
  }
  if(!settings.neuralRendering) p.gpu->SetNeuralRenderingEnabled(false);
  if(settings.intensity!=p.settings.intensity) p.gpu->SetIntensity(settings.intensity);
  p.gpu->SetFrameGenerationEnabled(settings.frameGeneration);
  p.gpu->SetNeuralRenderingEnabled(settings.neuralRendering);
  // Preserve the path of an initialized FG model even while it is bypassed.
  p.settings.intensity=settings.intensity; p.settings.sharpness=settings.sharpness;
  p.settings.neuralRendering=settings.neuralRendering; p.settings.comparison=settings.comparison;
  p.settings.frameGeneration=settings.frameGeneration;
}
