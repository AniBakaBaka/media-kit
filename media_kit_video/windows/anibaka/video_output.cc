// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "video_output.h"

#include <algorithm>
#include <cmath>
#include <chrono>

// Limit the frame size to 1080p in software rendering.
// This is for performance reasons & to avoid allocating too much memory.
#define SW_RENDERING_MAX_WIDTH 1920
#define SW_RENDERING_MAX_HEIGHT 1080
#define SW_RENDERING_PIXEL_BUFFER_SIZE \
  (SW_RENDERING_MAX_WIDTH) * (SW_RENDERING_MAX_HEIGHT) * (4)

VideoOutput::VideoOutput(int64_t handle,
                         VideoOutputConfiguration configuration,
                         flutter::PluginRegistrarWindows* registrar,
                         ThreadPool* thread_pool_ref)
    : handle_(reinterpret_cast<mpv_handle*>(handle)),
      width_(configuration.width),
      height_(configuration.height),
      configuration_(configuration),
      registrar_(registrar),
      thread_pool_ref_(thread_pool_ref) {
  // The constructor must be invoked through the thread pool, because
  // |ANGLESurfaceManager| & libmpv render context creation can conflict with
  // the existing |Render| or |Resize| calls from another |VideoOutput|
  // instances (which will result in access violation).
  auto future = thread_pool_ref_->Post([&]() {
    mpv_set_option_string(handle_, "video-sync", "audio");
    mpv_set_option_string(handle_, "video-timing-offset", "0");
    // First try to initialize video playback with hardware acceleration &
    // |ANGLESurfaceManager|, use S/W API as fallback.
    auto is_hardware_acceleration_enabled = false;
    // Attempt to use H/W rendering.
    if (configuration.enable_hardware_acceleration) {
      try {
        // OpenGL context needs to be set before |mpv_render_context_create|.
        surface_manager_ = std::make_unique<ANGLESurfaceManager>(
            static_cast<int32_t>(width_.value_or(1)),
            static_cast<int32_t>(height_.value_or(1)));
        surface_manager_->MakeCurrent(true);
        Resize(width_.value_or(1), height_.value_or(1));
        mpv_opengl_init_params gl_init_params{
            [](auto, auto name) {
              return reinterpret_cast<void*>(eglGetProcAddress(name));
            },
            nullptr,
        };
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        // Create render context.
        if (mpv_render_context_create(&render_context_, handle_, params) == 0) {
          mpv_render_context_set_update_callback(
              render_context_,
              [](void* context) {
                // Notify Flutter that a new frame is available. The actual
                // rendering will take place in the |Render| method, which will
                // be called by Flutter on the render thread.
                auto that = reinterpret_cast<VideoOutput*>(context);
                that->NotifyRender();
              },
              reinterpret_cast<void*>(this));
          // Set flag to true, indicating that H/W rendering is supported.
          is_hardware_acceleration_enabled = true;
          std::cout << "media_kit: VideoOutput: Using H/W rendering."
                    << std::endl;
        }
      } catch (...) {
        // Do nothing.
        // Likely received an |std::runtime_error| from |ANGLESurfaceManager|,
        // which indicates that H/W rendering is not supported.
      }
    }
    if (!is_hardware_acceleration_enabled) {
      std::cout << "media_kit: VideoOutput: Using S/W rendering." << std::endl;
      // Allocate a "large enough" buffer ahead of time.
      pixel_buffer_ =
          std::make_unique<uint8_t[]>(SW_RENDERING_PIXEL_BUFFER_SIZE);
      Resize(width_.value_or(1), height_.value_or(1));
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      if (mpv_render_context_create(&render_context_, handle_, params) == 0) {
        mpv_render_context_set_update_callback(
            render_context_,
            [](void* context) {
              // Notify Flutter that a new frame is available. The actual
              // rendering will take place in the |Render| method, which will be
              // called by Flutter on the render thread.
              auto that = reinterpret_cast<VideoOutput*>(context);
              that->NotifyRender();
            },
            reinterpret_cast<void*>(this));
      }
    }
  });
  future.wait();
}

VideoOutput::~VideoOutput() {
  destroyed_ = true;
  RemoveDlssVideoStatus(reinterpret_cast<int64_t>(handle_));
  auto promise = std::promise<void>();
  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(
        texture_id_, [&, texture_id = texture_id_]() {
          // Add one more task into the thread pool queue & exit the destructor
          // only when it gets executed. This will ensure that all the tasks
          // posted to the thread pool i.e. render or resize before this are
          // executed (and won't reference the dead object anymore), most
          // notably |CheckAndResize| & |Render|.
          auto future = thread_pool_ref_->Post([&, id = texture_id]() {
            std::cout << "media_kit: VideoOutput: Free Texture: " << id
                      << std::endl;
            std::cout << "VideoOutput::~VideoOutput: "
                      << reinterpret_cast<int64_t>(handle_) << std::endl;
            ResetDlss();
            std::lock_guard<std::mutex> lock(textures_mutex_);
            texture_variants_.clear();
            // H/W
            textures_.clear();
            // S/W
            pixel_buffer_textures_.clear();
            // Free (call destructor) |ANGLESurfaceManager| through the thread
            // pool. This will ensure synchronized EGL or ANGLE usage & won't
            // conflict with |Render| or |CheckAndResize| of other
            // |VideoOutput|s.
            surface_manager_.reset(nullptr);
            promise.set_value();
          });
        });
  }

  promise.get_future().wait();
  texture_id_ = 0;
  RemoveDlssVideoStatus(reinterpret_cast<int64_t>(handle_));

  thread_pool_ref_->Post([render_context = render_context_]() {
    mpv_render_context_free(render_context);
  });
}

void VideoOutput::ResetDlss() {
  dlss_applied_revision_=0;
  std::shared_ptr<DlssVideoBridge> retired;
  {
    std::lock_guard<std::mutex> lock(textures_mutex_);
    dlss_visible_=false; retired=std::move(dlss_);
  }
  // NGX destruction can wait for the GPU. Never hold Flutter's texture lock.
}

void VideoOutput::NotifyRender() {
  if (destroyed_ || render_queued_.exchange(true)) {
    return;
  }
  thread_pool_ref_->Post([this] {
    render_queued_=false;
    if (destroyed_) return;
    const auto settings=GetDlssVideoSettings();
    if(settings.enabled || dlss_) {
      char* media=mpv_get_property_string(handle_,"path");
      const std::string path=media ? media : "";
      if(media) mpv_free(media);
      if(path!=dlss_media_path_) {
        dlss_media_path_=path; dlss_failed_=false; previous_target_=0; previous_position_=-1;
        ResetDlss();
      }
    }
    if(settings.revision!=dlss_revision_) {
      dlss_revision_=settings.revision; dlss_failed_=false; previous_target_=0; previous_position_=-1;
      // The player switch bypasses processing while retaining this video's
      // pipeline. Closing the laboratory master gate releases GPU memory.
      if(!settings.enabled && settings.runtime.empty()) ResetDlss();
      else { std::lock_guard<std::mutex> lock(textures_mutex_); dlss_visible_=false; }
      mpv_set_property_string(handle_,"video-timing-offset",settings.enabled ? "0.1" : "0");
    }
    CheckAndResize(); Render();
  });
}

void VideoOutput::Render() {
  if (texture_id_) {
    const auto settings=GetDlssVideoSettings();
    mpv_render_frame_info frame{};
    mpv_render_param frameInfo{MPV_RENDER_PARAM_NEXT_FRAME_INFO,&frame};
    if(settings.enabled) mpv_render_context_get_info(render_context_,frameInfo);
    // H/W
    if (surface_manager_ != nullptr) {
      try {
      surface_manager_->Draw([&]() {
        mpv_opengl_fbo fbo{
            0,
            surface_manager_->width(),
            surface_manager_->height(),
            0,
        };
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, nullptr},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        int block=settings.enabled ? 0 : 1;
        params[1].data=&block;
        mpv_render_context_render(render_context_, params);
      },settings.enabled);
      // All D3D commands execute here, never in Flutter's raster callback.
      surface_manager_->Read();
      } catch(const std::exception& error) {
        dlss_failed_=true;
        SetDlssVideoStatus(reinterpret_cast<int64_t>(handle_),std::string("普通画面：视频纹理同步失败：")+error.what());
        ResetDlss();
        return;
      }
      if (settings.enabled) Enhance(frame);
    }
    // S/W
    if (pixel_buffer_ != nullptr) {
      if(settings.enabled) SetDlssVideoStatus(reinterpret_cast<int64_t>(handle_),"普通画面：当前使用软件渲染，DLSS 需要 NVIDIA 硬件渲染");
      int32_t size[]{
          static_cast<int32_t>(pixel_buffer_textures_.at(texture_id_)->width),
          static_cast<int32_t>(pixel_buffer_textures_.at(texture_id_)->height),
      };
      auto pitch = 4 * size[0];
      mpv_render_param params[]{
          {MPV_RENDER_PARAM_SW_SIZE, size},
          {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
          {MPV_RENDER_PARAM_SW_POINTER, pixel_buffer_.get()},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      mpv_render_context_render(render_context_, params);
    }
    try {
      // Notify Flutter that a new frame is available.
      registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_id_);
    } catch (...) {
      // Prevent any redundant exceptions if the texture is unregistered etc.
    }
  }
}

void VideoOutput::Enhance(const mpv_render_frame_info& frame) {
  if (dlss_failed_) return;
  const auto owner=reinterpret_cast<int64_t>(handle_);
  try {
    const auto settings=GetDlssVideoSettings();
    if (!settings.enabled || settings.revision!=dlss_revision_) return;
    double fps=0,position=0;
    mpv_get_property(handle_,"container-fps",MPV_FORMAT_DOUBLE,&fps);
    mpv_get_property(handle_,"playback-time",MPV_FORMAT_DOUBLE,&position);
    if(!std::isfinite(fps) || fps<=0) {
      mpv_get_property(handle_,"estimated-vf-fps",MPV_FORMAT_DOUBLE,&fps);
    }
    if(!std::isfinite(fps) || fps<=0) {
      SetDlssVideoStatus(owner,"普通画面：等待可用的片源帧率"); return;
    }
    if(fps>60) throw std::runtime_error("片源帧率超过 60 fps");
    auto transfer=mpv_get_property_string(handle_,"video-params/gamma");
    const bool hdr=transfer && (strcmp(transfer,"pq")==0 || strcmp(transfer,"hlg")==0 || strcmp(transfer,"st2084")==0);
    if(transfer) mpv_free(transfer);
    if(hdr) throw std::runtime_error("HDR 视频暂不支持 DLSS 视频增强");
    std::shared_ptr<DlssVideoBridge> bridge;
    { std::lock_guard<std::mutex> lock(textures_mutex_); bridge=dlss_; }
    const bool configurationChanged=dlss_applied_revision_!=settings.revision;
    // A repeat must reuse the completed image, not feed the same sample into
    // DLSS again with Reset=1 and throw away its temporal reconstruction.
    if(bridge && !configurationChanged && previous_position_>=0 &&
        ((frame.flags&MPV_RENDER_FRAME_INFO_REPEAT)!=0 ||
         (frame.flags&MPV_RENDER_FRAME_INFO_PRESENT)==0)) return;
    if(bridge && dlss_applied_revision_!=settings.revision) {
      if(bridge->CanReuse(surface_manager_->width(),surface_manager_->height(),settings)) {
        bridge->UpdateSettings(settings);
      } else {
        ResetDlss(); bridge.reset();
      }
    }
    if(!bridge) {
      SetDlssVideoStatus(owner,"正在按所选效果初始化当前视频的 NVIDIA 增强…");
      bridge=std::make_shared<DlssVideoBridge>(surface_manager_->source_texture(),settings,fps);
      std::lock_guard<std::mutex> lock(textures_mutex_); dlss_=bridge;
    }
    dlss_applied_revision_=settings.revision;
    const bool timedDiscontinuity=frame.target_time>0 && previous_target_>0 &&
        (frame.target_time<=previous_target_ || frame.target_time-previous_target_>250000);
    const bool reset=timedDiscontinuity || previous_position_<0 ||
        position<previous_position_ || position-previous_position_>.25 ||
        (frame.flags&MPV_RENDER_FRAME_INFO_REDRAW)!=0;
    const int64_t midpoint=previous_target_+(frame.target_time-previous_target_)/2;
    const bool interpolate=bridge->Prepare(surface_manager_->source_texture(),reset);
    int seeking=0; double currentPosition=position;
    mpv_get_property(handle_,"seeking",MPV_FORMAT_FLAG,&seeking);
    mpv_get_property(handle_,"playback-time",MPV_FORMAT_DOUBLE,&currentPosition);
    if(seeking || std::abs(currentPosition-position)>.5) {
      previous_target_=0; previous_position_=-1;
      std::lock_guard<std::mutex> lock(textures_mutex_); dlss_visible_=false; return;
    }
    auto waitUntil=[&](int64_t target) {
      // No advanced-control mode: the mpv core remains free to handle seeks and
      // pause while this worker paces output using its audio-clock deadline.
      while(!destroyed_ && target>mpv_get_time_us(handle_)) {
        if(GetDlssVideoSettings().revision!=dlss_revision_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return !destroyed_ && GetDlssVideoSettings().revision==dlss_revision_;
    };
    const auto now=mpv_get_time_us(handle_);
    std::string fgState=reset ? "等待连续原帧" : !interpolate ? "运行库要求跳过" : "处理完成太晚，已跳过";
    if(interpolate && (midpoint<=0 || frame.target_time<=0 || frame.target_time>=now+150000))
      fgState="显示时间不可用，已跳过";
    if(interpolate && midpoint>0 && frame.target_time>now+2000 && frame.target_time<now+150000) {
      if(!waitUntil(midpoint)) return;
      if(mpv_get_time_us(handle_)<frame.target_time-2000) {
        if(bridge->Publish(true)) {
          { std::lock_guard<std::mutex> lock(textures_mutex_); dlss_visible_=true; }
          registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_id_);
          fgState="已提交中间帧";
        } else fgState="对比参考帧尚未就绪，已跳过";
      }
    }
    if(frame.target_time>now && frame.target_time<now+150000 && !waitUntil(frame.target_time)) return;
    if(GetDlssVideoSettings().revision!=dlss_revision_ || destroyed_) return;
    if(!bridge->Publish(false)) return;
    { std::lock_guard<std::mutex> lock(textures_mutex_); dlss_visible_=true; }
    previous_target_=frame.target_time; previous_position_=position;
    temporal_frames_=reset ? 1 : temporal_frames_+1;
    if(dlss_failed_) return;
    std::string activeStatus=bridge->wasRead()
        ? "增强画面已提交" : "增强计算完成，等待画面提交";
    activeStatus+=settings.neuralRendering ? " · NR 改绘开启" : " · 原画直接输入";
    activeStatus+=settings.superResolution ? " · 2 倍超分" : " · 超分关闭";
    activeStatus+=" · 输入 "+std::to_string(surface_manager_->width())+" × "+std::to_string(surface_manager_->height());
    activeStatus+=" → 输出 "+std::to_string(bridge->width())+" × "+std::to_string(bridge->height());
    activeStatus+=" · 时序累计 "+std::to_string(temporal_frames_)+" 帧";
    if(settings.comparison) activeStatus+=" · 对比：左原帧 / 右增强与插帧";
    activeStatus+=settings.frameGeneration
        ? " · FG："+fgState+" · 生成帧提交 "+std::to_string(bridge->generatedSubmitted())+
            " / 播放器读取 "+std::to_string(bridge->generatedRead())
        : " · 帧生成关闭";
    SetDlssVideoStatus(owner,activeStatus);
  } catch(const std::exception& error) {
    dlss_failed_=true;
    ResetDlss();
    mpv_set_property_string(handle_,"video-timing-offset","0");
    SetDlssVideoStatus(owner,std::string("普通画面：")+error.what());
  }
}

void VideoOutput::SetTextureUpdateCallback(
    std::function<void(int64_t, int64_t, int64_t)> callback) {
  texture_update_callback_ = callback;
  texture_update_callback_(texture_id_, GetVideoWidth(), GetVideoHeight());
}

void VideoOutput::SetSize(std::optional<int64_t> width,
                          std::optional<int64_t> height) {
  thread_pool_ref_->Post([&, width, height]() {
    if (width.has_value()) {
      // H/W
      if (surface_manager_ != nullptr) {
        width_ = width.value();
      }
      // S/W
      if (pixel_buffer_ != nullptr) {
        // Limit width if software rendering is being used.
        width_ = std::clamp(width.value(), static_cast<int64_t>(0),
                            static_cast<int64_t>(SW_RENDERING_MAX_WIDTH));
      }
    } else {
      width_ = std::nullopt;
    }
    if (height.has_value()) {
      // H/W
      if (surface_manager_ != nullptr) {
        height_ = height.value();
      }
      // S/W
      if (pixel_buffer_ != nullptr) {
        // Limit width if software rendering is being used.
        height_ = std::clamp(height.value(), static_cast<int64_t>(0),
                             static_cast<int64_t>(SW_RENDERING_MAX_HEIGHT));
      }
    } else {
      height_ = std::nullopt;
    }
  });
}

void VideoOutput::CheckAndResize() {
  // Check if a new texture with different dimensions is needed.
  auto required_width = GetVideoWidth(), required_height = GetVideoHeight();
  if (required_width < 1 || required_height < 1) {
    // Invalid.
    return;
  }
  int64_t current_width = -1, current_height = -1;
  if (surface_manager_ != nullptr) {
    current_width = surface_manager_->width();
    current_height = surface_manager_->height();
  }
  if (pixel_buffer_ != nullptr) {
    current_width = pixel_buffer_textures_.at(texture_id_)->width;
    current_height = pixel_buffer_textures_.at(texture_id_)->height;
  }
  // Currently rendered video output dimensions.
  // Either H/W or S/W rendered.
  assert(current_width > 0);
  assert(current_height > 0);
  if (required_width == current_width && required_height == current_height) {
    // No creation of new texture required.
    return;
  }
  Resize(required_width, required_height);
}

void VideoOutput::Resize(int64_t required_width, int64_t required_height) {
  const auto settings=GetDlssVideoSettings();
  if(dlss_ && settings.enabled &&
      !dlss_->CanReuse(static_cast<unsigned>(required_width),static_cast<unsigned>(required_height),settings)) ResetDlss();
  previous_target_=0; previous_position_=-1;
  std::cout << required_width << " " << required_height << std::endl;
  // Unregister previously registered texture & delete underlying objects.
  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(
        texture_id_, [&, id = texture_id_]() {
          if (id) {
            std::cout << "media_kit: VideoOutput: Free Texture: " << id
                      << std::endl;
            std::lock_guard<std::mutex> lock(textures_mutex_);
            if (destroyed_) {
              return;
            }
            if (texture_variants_.find(id) != texture_variants_.end()) {
              texture_variants_.erase(id);
            }
            // H/W
            if (textures_.find(id) != textures_.end()) {
              textures_.erase(id);
            }
            // S/W
            if (pixel_buffer_textures_.find(id) !=
                pixel_buffer_textures_.end()) {
              pixel_buffer_textures_.erase(id);
            }
          }
        });
    texture_id_ = 0;
  }
  // H/W
  if (surface_manager_ != nullptr) {
    // Destroy internal ID3D11Texture2D & EGLSurface & create new with updated
    // dimensions while preserving previous EGLDisplay & EGLContext.
    surface_manager_->SetSize(static_cast<int32_t>(required_width),
                              static_cast<int32_t>(required_height));
    auto texture = std::make_unique<FlutterDesktopGpuSurfaceDescriptor>();
    texture->struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    texture->handle = surface_manager_->handle();
    texture->width = texture->visible_width = surface_manager_->width();
    texture->height = texture->visible_height = surface_manager_->height();
    texture->release_context = nullptr;
    texture->release_callback = [](void*) {};
    texture->format = kFlutterDesktopPixelFormatBGRA8888;
    auto texture_variant =
        std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [&, enhancedDescriptor=std::make_shared<FlutterDesktopGpuSurfaceDescriptor>()](auto, auto) {
              std::lock_guard<std::mutex> lock(textures_mutex_);
              if (texture_id_) {
                if(dlss_visible_ && dlss_ && GetDlssVideoSettings().enabled) {
                  // Keep the resource alive until Flutter opens it. The callback
                  // is not GPU completion; published pixels are never rewritten.
                  struct Lease {
                    std::shared_ptr<const DlssVideoFrame> frame;
                  };
                  auto lease=std::make_unique<Lease>(); lease->frame=dlss_->AcquireFrame();
                  if(!lease->frame) return textures_.at(texture_id_).get();
                  // Flutter invokes release_callback while binding the image,
                  // then still reads visible_width/height from this descriptor.
                  // Keep the descriptor alive with the registered texture.
                  auto& d=*enhancedDescriptor; d.struct_size=sizeof(d);
                  d.handle=lease->frame->handle;
                  d.width=d.visible_width=lease->frame->width;
                  d.height=d.visible_height=lease->frame->height;
                  d.format=kFlutterDesktopPixelFormatBGRA8888;
                  d.release_context=lease.get(); d.release_callback=[](void* p) { delete static_cast<Lease*>(p); };
                  auto result=&d; lease.release(); return result;
                }
                return textures_.at(texture_id_).get();
              } else {
                return (FlutterDesktopGpuSurfaceDescriptor*)nullptr;
              }
            }));
    // Register new texture.
    texture_id_ =
        registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    std::cout << "media_kit: VideoOutput: Create Texture: " << texture_id_
              << std::endl;
    std::lock_guard<std::mutex> lock(textures_mutex_);
    textures_.emplace(std::make_pair(texture_id_, std::move(texture)));
    texture_variants_.emplace(
        std::make_pair(texture_id_, std::move(texture_variant)));
    // Notify public texture update callback.
    texture_update_callback_(texture_id_, required_width, required_height);
  }
  // S/W
  if (pixel_buffer_ != nullptr) {
    auto pixel_buffer_texture = std::make_unique<FlutterDesktopPixelBuffer>();
    pixel_buffer_texture->buffer = pixel_buffer_.get();
    pixel_buffer_texture->width = required_width;
    pixel_buffer_texture->height = required_height;
    pixel_buffer_texture->release_context = nullptr;
    pixel_buffer_texture->release_callback = [](void*) {};
    auto texture_variant = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([&](auto, auto) {
          std::lock_guard<std::mutex> lock(textures_mutex_);
          if (texture_id_) {
            return pixel_buffer_textures_.at(texture_id_).get();
          } else {
            return (FlutterDesktopPixelBuffer*)nullptr;
          }
        }));
    // Register new texture.
    texture_id_ =
        registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    std::cout << "media_kit: VideoOutput: Create Texture: " << texture_id_
              << std::endl;
    std::lock_guard<std::mutex> lock(textures_mutex_);
    pixel_buffer_textures_.emplace(
        std::make_pair(texture_id_, std::move(pixel_buffer_texture)));
    texture_variants_.emplace(
        std::make_pair(texture_id_, std::move(texture_variant)));
    // Notify public texture update callback.
    texture_update_callback_(texture_id_, required_width, required_height);
  }
}

int64_t VideoOutput::GetVideoWidth() {
  // Fixed width.
  if (width_) {
    return width_.value();
  }
  // Video resolution dependent width.
  int64_t width = 0;
  int64_t height = 0;

  mpv_node params;
  mpv_get_property(handle_, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  const auto settings=GetDlssVideoSettings();
  if(settings.enabled && !dlss_failed_ && width>0 && height>0) {
    const double maxHeight=static_cast<double>(settings.maxHeight);
    const double ratio=std::min({1.0,(maxHeight*16.0/9.0)/width,maxHeight/height});
    return std::max<int64_t>(2,static_cast<int64_t>(width*ratio)/2*2);
  }

  if (pixel_buffer_ != nullptr) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect-ratio.
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return SW_RENDERING_MAX_WIDTH;
    }
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return width / height * SW_RENDERING_MAX_HEIGHT;
    }
  }

  return width;
}

int64_t VideoOutput::GetVideoHeight() {
  // Fixed height.
  if (height_) {
    return height_.value();
  }
  // Video resolution dependent height.
  int64_t width = 0;
  int64_t height = 0;

  mpv_node params;
  mpv_get_property(handle_, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  const auto settings=GetDlssVideoSettings();
  if(settings.enabled && !dlss_failed_ && width>0 && height>0) {
    const double maxHeight=static_cast<double>(settings.maxHeight);
    const double ratio=std::min({1.0,(maxHeight*16.0/9.0)/width,maxHeight/height});
    return std::max<int64_t>(2,static_cast<int64_t>(height*ratio)/2*2);
  }

  if (pixel_buffer_ != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect-ratio.
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return SW_RENDERING_MAX_HEIGHT;
    }
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return height / width * SW_RENDERING_MAX_WIDTH;
    }
  }

  return height;
}
