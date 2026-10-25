// Headless Windows integration probe for the real libmpv/GL/D3D11 hot path.
// Runs identical model/shader selections on ANGLE and native NVIDIA OpenGL.
#include "angle_surface_manager.h"
#include "video_state.h"
#include <render_gl.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <mmsystem.h>

namespace {
bool Enabled(const wchar_t* name) {
  wchar_t value[2]{};
  return GetEnvironmentVariableW(name, value, 2) > 0;
}
std::string Escape(const std::string& value) { return "%" + std::to_string(value.size()) + "%" + value; }
void Command(mpv_handle* mpv, std::initializer_list<const char*> values) {
  std::vector<const char*> command(values); command.push_back(nullptr);
  if (mpv_command(mpv, command.data()) < 0) throw std::runtime_error("mpv command failed");
}
std::string Property(mpv_handle* mpv, const char* name) {
  const auto value = mpv_get_property_string(mpv, name);
  const std::string result = value ? value : "";
  mpv_free(value); return result;
}
}

int main(int argc, char** argv) {
  timeBeginPeriod(1);
  if (argc < 7) {
    std::cerr << "clip runtime angle|native shaders-root seconds output-width [raw-frame-path]\n";
    return 2;
  }
  const std::string clip = argv[1], runtime = argv[2], shaders = argv[4];
  const bool native = std::string(argv[3]) == "native";
  const int seconds = std::stoi(argv[5]), width = std::stoi(argv[6]), height = width * 9 / 16;
  ANGLESurfaceManager surface(width, height, native);
  surface.MakeCurrent(true);
  auto mpv = mpv_create();
  const auto option = [&](const char* name, const std::string& value) {
    if (mpv_set_option_string(mpv, name, value.c_str()) < 0) throw std::runtime_error(name);
  };
  option("vo", "libmpv"); option("ao", "null"); option("terminal", "no");
  const bool directml = runtime.find("directML") != std::string::npos;
  option("hwdec", directml ? "d3d11va" : "nvdec"); option("loop-file", "inf"); option("idle", "yes");
  option("sharpen", "1"); option("msg-level", "all=warn");
  option("gpu-shader-cache", "yes");
  option("gpu-shader-cache-dir", runtime + "/probe-gpu-shaders");
  if (Enabled(L"ANIBAKA_PROBE_UNTIMED")) {
    option("untimed", "yes"); option("audio", "no");
  }
  if (shaders != "none") {
    std::string chain;
    for (const auto name : {"Anime4K_Clamp_Highlights.glsl", "AniBaka_Clear_v1.glsl",
        "Anime4K_Restore_CNN_VL.glsl", "Anime4K_Upscale_CNN_x2_VL.glsl",
        "Anime4K_Restore_CNN_M.glsl", "Anime4K_AutoDownscalePre_x2.glsl",
        "Anime4K_AutoDownscalePre_x4.glsl", "Anime4K_Upscale_CNN_x2_M.glsl"}) {
      if (!chain.empty()) chain += ';'; chain += shaders + '/' + name;
    }
    option("glsl-shaders", chain);
  }
  if (mpv_initialize(mpv) < 0) return 3;
  VideoStateObserver metadata(mpv); metadata.Start([] {});
  mpv_opengl_init_params gl{[](void* context, const char* name) {
    return static_cast<ANGLESurfaceManager*>(context)->Resolve(name);
  }, &surface};
  mpv_render_param init[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl}, {MPV_RENDER_PARAM_INVALID, nullptr}};
  mpv_render_context* render = nullptr;
  if (mpv_render_context_create(&render, mpv, init) < 0) return 4;
  mpv_opengl_fbo fbo{surface.framebuffer(), width, height, 0};
  mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo}, {MPV_RENDER_PARAM_INVALID, nullptr}};
  Command(mpv, {"loadfile", clip.c_str()});
  const std::string inference = runtime + "/animejanai/inference";
  const std::string filter = "@anibaka_aji:animejanai:lib=" + Escape(inference + "/aji.dll") +
      ":conf=" + Escape(runtime + "/animejanai.conf") + ":model-dir=" + Escape(runtime + "/animejanai/onnx") +
      ":trtexec=" + Escape(inference + "/trtexec.exe") + ":stats=" + Escape(runtime + "/native-probe.log") +
      ":slot=1003:output-444=no";
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now(); auto measured = started;
  bool attached = false, active = false;
  unsigned frames = 0, updates = 0;
  double first = 0, render_ms = 0;
  double first_frame_ms = 0;
  std::string start_drops;
  while (Clock::now() - started < std::chrono::minutes(5)) {
    if (!attached && Property(mpv, "video-params/w") == "1920") {
      const bool software = !directml && Property(mpv, "hwdec-current") != "nvdec";
      const std::string chain = (software ? "@anibaka_aji_input:lavfi=[format=pix_fmts=nv12|p010le,hwupload_cuda]," : "") + filter;
      Command(mpv, {"vf", "add", chain.c_str()}); attached = true;
    }
    if (!(mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
    }
    mpv_render_frame_info frame{};
    mpv_render_context_get_info(render, {MPV_RENDER_PARAM_NEXT_FRAME_INFO, &frame});
    const auto before = Clock::now();
    surface.Draw([&] { mpv_render_context_render(render, params); });
    surface.Read();
    const auto state = metadata.Snapshot();
    if (first_frame_ms == 0 && state.width == 3840) {
      first_frame_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    }
    if (!active && state.width == 3840 && Clock::now() - started > std::chrono::seconds(3)) {
      active = true; measured = Clock::now(); first = state.position;
      start_drops = Property(mpv, "frame-drop-count");
    }
    if (active) {
      ++updates;
      render_ms += std::chrono::duration<double, std::milli>(Clock::now() - before).count();
      if ((frame.flags & MPV_RENDER_FRAME_INFO_PRESENT) && !(frame.flags & MPV_RENDER_FRAME_INFO_REPEAT)) ++frames;
      if (Clock::now() - measured >= std::chrono::seconds(seconds)) break;
    }
  }
  const auto state = metadata.Snapshot();
  const double elapsed = std::chrono::duration<double>(Clock::now() - measured).count();
  std::cout << "backend=" << argv[3] << " nativeFbo=" << surface.framebuffer() << " active=" << active
      << " frames=" << frames << " seconds=" << elapsed << " fps=" << frames / elapsed
      << " posDelta=" << state.position - first << " renderMeanMs=" << render_ms / (updates ? updates : 1)
      << " firstFrameMs=" << first_frame_ms
      << " startDrops=" << start_drops << " finalDrops=" << Property(mpv, "frame-drop-count")
      << " avsync=" << Property(mpv, "avsync") << "\n";
  std::cout << "hwdec=" << Property(mpv, "hwdec-current")
      << " interop=" << Property(mpv, "hwdec-interop") << '\n';
  if (argc > 7 && active) {
    Command(mpv, {"set", "pause", "yes"}); Command(mpv, {"seek", "30", "absolute+exact"});
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
      if (mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME) {
        surface.Draw([&] { mpv_render_context_render(render, params); }); surface.Read();
      }
      const auto current = metadata.Snapshot();
      if (!current.seeking && current.position >= 29.99 && current.position < 30.1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        surface.Draw([&] { mpv_render_context_render(render, params); }); surface.Read();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device; surface.source_texture()->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{}; surface.source_texture()->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 5;
    context->CopyResource(staging.Get(), surface.source_texture());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return 6;
    std::ofstream output(argv[7], std::ios::binary);
    for (UINT y = 0; y < desc.Height; ++y)
      output.write(static_cast<const char*>(map.pData) + y * map.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    std::cout << "Captured rendered BGRA frame at " << metadata.Snapshot().position << "\n";
  }
  if (Enabled(L"ANIBAKA_PROBE_LIFECYCLE") && active) {
    Command(mpv, {"set", "pause", "yes"});
    const auto waitWidth = [&](int expected) {
      const auto deadline = Clock::now() + std::chrono::seconds(8);
      while (Clock::now() < deadline) {
        if (mpv_render_context_update(render) & MPV_RENDER_UPDATE_FRAME) {
          surface.Draw([&] { mpv_render_context_render(render, params); }); surface.Read();
        }
        if (metadata.Snapshot().width == expected) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      throw std::runtime_error("Observed video size did not update");
    };
    Command(mpv, {"vf-command", "anibaka_aji", "slot", "0"});
    Command(mpv, {"seek", "0", "relative+exact"});
    waitWidth(1920);
    surface.SetSize(width / 2, height / 2);
    fbo = {surface.framebuffer(), width / 2, height / 2, 0};
    Command(mpv, {"vf-command", "anibaka_aji", "slot", "1003"});
    Command(mpv, {"seek", "0", "relative+exact"});
    waitWidth(3840);
    surface.SetSize(width, height);
    fbo = {surface.framebuffer(), width, height, 0};
    surface.Draw([&] { mpv_render_context_render(render, params); }); surface.Read();
    if (metadata.Snapshot().path != Property(mpv, "path")) throw std::runtime_error("Stale media path");
    std::cout << "PASS: property events, paused slot switch, GPU surface resize and restore\n";
  }
  metadata.Stop();
  mpv_render_context_free(render);
  // metadata owns a secondary client until its destructor; destroy the owner
  // without sending terminate (which would wait for that same client).
  mpv_destroy(mpv);
  timeEndPeriod(1);
  return active ? 0 : 7;
}
