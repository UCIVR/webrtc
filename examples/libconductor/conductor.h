#pragma once

#ifdef LIBCONDUCTOR_EXPORT
#define LIBCONDUCTOR_API __declspec(dllexport)
#else
#define LIBCONDUCTOR_API __declspec(dllimport)
#endif

#include <cstdint>

namespace conductor {
class observer;

using video_callback = bool(void*, std::uint8_t*, std::uint64_t);
using on_video = void(void*, std::uint64_t width, std::uint64_t height, video_callback* callback, void* data);

class LIBCONDUCTOR_API observer_handle {
 public:
  observer_handle(void* client, on_video* video_event);
  ~observer_handle();
  observer_handle(observer_handle&) = delete;
  observer_handle(observer_handle&&);
  void start(const char* url);

 private:
  observer* impl;
};
}  // namespace conductor
