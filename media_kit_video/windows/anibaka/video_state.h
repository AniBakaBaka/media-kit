// Project-owned libmpv observation client. No synchronous property reads are
// allowed in the render loop: inference can hold mpv's core for a long time.
#pragma once

#include <client.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

struct VideoState {
  int64_t width = 0, height = 0, rotation = 0;
  double fps = 0, estimated_fps = 0, position = 0;
  bool seeking = false, hdr = false;
  std::string path;
};

struct VideoProperties {
  std::mutex mutex;
  std::map<std::string, std::string> values;
};
std::optional<std::map<std::string, std::string>> GetVideoProperties(int64_t owner);

class VideoStateObserver {
 public:
  explicit VideoStateObserver(mpv_handle* owner);
  ~VideoStateObserver();
  VideoStateObserver(const VideoStateObserver&) = delete;
  VideoStateObserver& operator=(const VideoStateObserver&) = delete;
  void Start(std::function<void()> dimensions_changed);
  void Stop();
  VideoState Snapshot() const;
  void SetTimingOffset(double value);

 private:
  void Run();
  mpv_handle* client_ = nullptr;
  int64_t owner_ = 0;
  std::shared_ptr<VideoProperties> properties_;
  mutable std::mutex mutex_;
  VideoState state_;
  std::atomic<bool> stopping_{false};
  std::function<void()> dimensions_changed_;
  std::thread events_;
};
