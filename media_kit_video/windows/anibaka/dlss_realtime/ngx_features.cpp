#include "ngx_features.h"
#include <nvsdk_ngx_defs_dlssg.h>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
void CheckNgx(int result, const char* operation) {
  if (result != 1) {
    char hex[16]; std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(result));
    throw std::runtime_error(std::string(operation) + " failed (" + hex + ")");
  }
}
void Available(NVSDK_NGX_Parameter* caps, const char* name) {
  int available = 0, reason = 0, driver = 0;
  caps->Get((std::string(name)+".Available").c_str(), &available);
  if (available == 1) return;
  caps->Get((std::string(name)+".FeatureInitResult").c_str(), &reason);
  caps->Get((std::string(name)+".NeedsUpdatedDriver").c_str(), &driver);
  throw std::runtime_error(std::string("NVIDIA ") + name +
      " unavailable; check GPU/driver/runtime. FeatureInitResult=" +
      std::to_string(reason) + ", NeedsUpdatedDriver=" + std::to_string(driver));
}
}
NgxFeatures::~NgxFeatures() { Close(); }
void NgxFeatures::Close() {
  if (srHandle_) NVSDK_NGX_D3D12_ReleaseFeature(srHandle_);
  if (fgHandle_) NVSDK_NGX_D3D12_ReleaseFeature(fgHandle_);
  srHandle_ = fgHandle_ = nullptr;
  for (auto* p : {sr_, fg_, caps_}) if (p) NVSDK_NGX_D3D12_DestroyParameters(p);
  sr_ = fg_ = caps_ = nullptr;
}
void NgxFeatures::Initialize(ID3D12GraphicsCommandList* list,
    unsigned w, unsigned h, unsigned scale, bool fg, double fps) {
  if (scale == 1 && !fg) return;
  const auto allocate = NVSDK_NGX_D3D12_AllocateParameters;
  const auto capability = NVSDK_NGX_D3D12_GetCapabilityParameters;
  const auto create = NVSDK_NGX_D3D12_CreateFeature;
  CheckNgx(capability(&caps_), "NGX capability query");
  if (!caps_) throw std::runtime_error("NGX capability parameters missing");
  if (scale > 1) {
    Available(caps_, "SuperSampling");
    CheckNgx(allocate(&sr_), "SR parameters");
    if (!sr_) throw std::runtime_error("NGX SR parameters missing");
    sr_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    sr_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    sr_->Set(NVSDK_NGX_Parameter_Width, w);
    sr_->Set(NVSDK_NGX_Parameter_Height, h);
    sr_->Set(NVSDK_NGX_Parameter_OutWidth, w*scale);
    sr_->Set(NVSDK_NGX_Parameter_OutHeight, h*scale);
    sr_->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxPerf));
    sr_->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    sr_->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0u);
    CheckNgx(create(list, NVSDK_NGX_Feature_SuperSampling, sr_, &srHandle_), "DLSS SR creation");
    sr_->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
    sr_->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
    sr_->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    sr_->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    sr_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, w);
    sr_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, h);
    sr_->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, static_cast<float>(1000/fps));
    sr_->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
    sr_->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
  }
  if (!fg) return;
  Available(caps_, "FrameGeneration");
  CheckNgx(allocate(&fg_), "FG parameters");
  if (!fg_) throw std::runtime_error("NGX FG parameters missing");
  fg_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
  fg_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
  fg_->Set(NVSDK_NGX_Parameter_Width, w*scale);
  fg_->Set(NVSDK_NGX_Parameter_Height, h*scale);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, w);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, h);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
  CheckNgx(create(list, NVSDK_NGX_Feature_FrameGeneration, fg_, &fgHandle_), "DLSS FG creation");
  // A video is a flat orthographic plane with estimated screen-space motion.
  // It has no engine depth or temporal jitter. Projection and inverse agree.
  projection_[0]=projection_[5]=projection_[15]=1.f;
  projection_[10]=1.f/999.9f; projection_[14]=-.1f/999.9f;
  inverse_[0]=inverse_[5]=inverse_[15]=1.f;
  inverse_[10]=999.9f; inverse_[14]=.1f;
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, static_cast<void*>(projection_));
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, static_cast<void*>(inverse_));
  for (const auto* key : {NVSDK_NGX_DLSSG_Parameter_ClipToLensClip,
       NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip})
    fg_->Set(key, static_cast<void*>(identity_));
  for (const auto* key : {"DLSSG.JitterOffsetX", "DLSSG.JitterOffsetY",
       "DLSSG.CameraPinholeOffsetX", "DLSSG.CameraPinholeOffsetY",
       "DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ",
       "DLSSG.CameraUpX", "DLSSG.CameraUpZ", "DLSSG.CameraRightY",
       "DLSSG.CameraRightZ", "DLSSG.CameraFwdX", "DLSSG.CameraFwdY"}) fg_->Set(key, 0.f);
  for (const auto* key : {"DLSSG.CameraUpY", "DLSSG.CameraRightX", "DLSSG.CameraFwdZ"}) fg_->Set(key, 1.f);
  // DLSS-FG Programming Guide 310.7: pixel motion uses scale (1,1).
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_CameraNear, .1f);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFar, 1000.f);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_CameraFOV, 1.04719755f);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, static_cast<float>(w)/h);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_MvecInvalidValue, std::numeric_limits<float>::max());
  for (const auto* key : {"DLSSG.MultiFrameCount", "DLSSG.MultiFrameIndex",
       "DLSSG.CameraMotionIncluded", "DLSSG.OrthoProjection"}) fg_->Set(key, 1u);
  for (const auto* key : {"DLSSG.ColorBuffersHDR", "DLSSG.DepthInverted",
       "DLSSG.NotRenderingGameFrames", "DLSSG.MvecDilated", "DLSSG.MenuDetectionEnabled"}) fg_->Set(key, 0u);
  for (const auto* name : {"MVecs", "Depth", "HUDLess", "InputBackbuffer", "OutputInterpolated"}) {
    const std::string prefix=std::string("DLSSG.")+name+"Subrect";
    const bool input=std::string(name)=="MVecs" || std::string(name)=="Depth";
    fg_->Set((prefix+"BaseX").c_str(), 0u); fg_->Set((prefix+"BaseY").c_str(), 0u);
    fg_->Set((prefix+"Width").c_str(), w*(input ? 1 : scale));
    fg_->Set((prefix+"Height").c_str(), h*(input ? 1 : scale));
  }
}
void NgxFeatures::SuperResolve(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
    ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, bool reset) {
  sr_->Set(NVSDK_NGX_Parameter_Color, color);
  sr_->Set(NVSDK_NGX_Parameter_Depth, depth);
  sr_->Set(NVSDK_NGX_Parameter_MotionVectors, motion);
  sr_->Set(NVSDK_NGX_Parameter_Output, output);
  sr_->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
  CheckNgx(NVSDK_NGX_D3D12_EvaluateFeature_C(list, srHandle_, sr_, nullptr), "DLSS SR evaluation");
}
void NgxFeatures::Generate(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
    ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
    ID3D12Resource* disable, bool reset) {
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, color);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, color);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, output);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, disable);
  fg_->Set(NVSDK_NGX_DLSSG_Parameter_Reset, reset ? 1u : 0u);
  CheckNgx(NVSDK_NGX_D3D12_EvaluateFeature_C(list, fgHandle_, fg_, nullptr), "DLSS FG evaluation");
}
