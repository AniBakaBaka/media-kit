#include "video_state.h"
#include <cstring>
#include <stdexcept>
#include <iterator>

namespace {
enum Property : uint64_t { Dimensions = 1, Path, Fps, EstimatedFps, Position, Seeking, Gamma };
constexpr uint64_t kCachedProperty = 32;
constexpr const char* kPropertyNames[] = {
    "vf", "hwdec-current", "video-params/w", "video-params/h",
    "video-params/pixelformat", "video-out-params/w", "video-out-params/h",
    "user-data/anibaka-renderer-active", "time-pos", "seeking",
    "sharpen", "brightness", "contrast", "saturation", "gamma"};
std::mutex registry_mutex;
std::map<int64_t, std::weak_ptr<VideoProperties>> registry;
}

std::optional<std::map<std::string, std::string>> GetVideoProperties(int64_t owner) {
  std::shared_ptr<VideoProperties> properties;
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = registry.find(owner);
    if (found == registry.end()) return std::nullopt;
    properties = found->second.lock();
  }
  if (!properties) return std::nullopt;
  std::lock_guard<std::mutex> lock(properties->mutex);
  return properties->values;
}

VideoStateObserver::VideoStateObserver(mpv_handle* owner) {
  client_ = mpv_create_client(owner, "anibaka-render-state");
  if (!client_) throw std::runtime_error("Cannot create video state client");
  const auto observe = [this](uint64_t id, const char* name, mpv_format format) {
    if (mpv_observe_property(client_, id, name, format) < 0) {
      mpv_destroy(client_);
      client_ = nullptr;
      throw std::runtime_error("Cannot observe video state");
    }
  };
  observe(Dimensions, "video-out-params", MPV_FORMAT_NODE);
  observe(Path, "path", MPV_FORMAT_STRING);
  observe(Fps, "container-fps", MPV_FORMAT_DOUBLE);
  observe(EstimatedFps, "estimated-vf-fps", MPV_FORMAT_DOUBLE);
  observe(Position, "playback-time", MPV_FORMAT_DOUBLE);
  observe(Seeking, "seeking", MPV_FORMAT_FLAG);
  observe(Gamma, "video-params/gamma", MPV_FORMAT_STRING);
  for (uint64_t i = 0; i < std::size(kPropertyNames); ++i) {
    observe(kCachedProperty + i, kPropertyNames[i], MPV_FORMAT_STRING);
  }
  owner_ = reinterpret_cast<int64_t>(owner);
  properties_ = std::make_shared<VideoProperties>();
  std::lock_guard<std::mutex> lock(registry_mutex);
  registry[owner_] = properties_;
}

void VideoStateObserver::Start(std::function<void()> changed) {
  dimensions_changed_ = std::move(changed);
  events_ = std::thread([this] { Run(); });
}

VideoStateObserver::~VideoStateObserver() {
  Stop();
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = registry.find(owner_);
    if (found != registry.end() && found->second.lock() == properties_) registry.erase(found);
  }
  if (client_) mpv_destroy(client_);
}

void VideoStateObserver::Stop() {
  stopping_ = true;
  if (client_) mpv_wakeup(client_);
  if (events_.joinable()) events_.join();
}

VideoState VideoStateObserver::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void VideoStateObserver::SetTimingOffset(double value) {
  // Use our own client so async replies never collide with media-kit's IDs.
  mpv_set_property_async(client_, 0, "video-timing-offset", MPV_FORMAT_DOUBLE, &value);
}

void VideoStateObserver::Run() {
  while (!stopping_) {
    const auto event = mpv_wait_event(client_, -1);
    if (stopping_ || event->event_id == MPV_EVENT_SHUTDOWN) return;
    if (event->event_id != MPV_EVENT_PROPERTY_CHANGE) continue;
    const auto property = static_cast<mpv_event_property*>(event->data);
    if (event->reply_userdata >= kCachedProperty &&
        event->reply_userdata < kCachedProperty + std::size(kPropertyNames)) {
      const auto value = property->format == MPV_FORMAT_STRING && property->data
          ? *static_cast<char**>(property->data) : nullptr;
      std::lock_guard<std::mutex> lock(properties_->mutex);
      properties_->values[kPropertyNames[event->reply_userdata - kCachedProperty]] = value ? value : "";
      continue;
    }
    bool resized = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto string = [&]() {
        const auto value = property->format == MPV_FORMAT_STRING && property->data
            ? *static_cast<char**>(property->data) : nullptr;
        return value ? value : "";
      };
      const auto number = [&]() {
        return property->format == MPV_FORMAT_DOUBLE && property->data
            ? *static_cast<double*>(property->data) : 0.0;
      };
      switch (event->reply_userdata) {
        case Dimensions: {
          int64_t w = 0, h = 0, rotate = 0;
          if (property->format == MPV_FORMAT_NODE && property->data) {
            const auto node = static_cast<mpv_node*>(property->data);
            if (node->format == MPV_FORMAT_NODE_MAP) {
              for (int i = 0; i < node->u.list->num; ++i) {
                const auto& value = node->u.list->values[i];
                if (value.format != MPV_FORMAT_INT64) continue;
                const auto key = node->u.list->keys[i];
                if (std::strcmp(key, "dw") == 0) w = value.u.int64;
                if (std::strcmp(key, "dh") == 0) h = value.u.int64;
                if (std::strcmp(key, "rotate") == 0) rotate = value.u.int64;
              }
            }
          }
          resized = w != state_.width || h != state_.height || rotate != state_.rotation;
          state_.width = w; state_.height = h; state_.rotation = rotate;
          break;
        }
        case Path: state_.path = string(); break;
        case Fps: state_.fps = number(); break;
        case EstimatedFps: state_.estimated_fps = number(); break;
        case Position: state_.position = number(); break;
        case Seeking:
          state_.seeking = property->format == MPV_FORMAT_FLAG && property->data &&
              *static_cast<int*>(property->data) != 0;
          break;
        case Gamma: {
          const auto gamma = string();
          state_.hdr = std::strcmp(gamma, "pq") == 0 ||
              std::strcmp(gamma, "hlg") == 0 || std::strcmp(gamma, "st2084") == 0;
          break;
        }
      }
    }
    if (resized && dimensions_changed_) dimensions_changed_();
  }
}
