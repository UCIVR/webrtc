#pragma once

#ifdef LIBCONDUCTOR_EXPORT
#define LIBCONDUCTOR_API __declspec(dllexport)
#else
#define LIBCONDUCTOR_API __declspec(dllimport)
#endif

namespace conductor {
class observer;

using log_function = void(bool is_error, const wchar_t* string);

class LIBCONDUCTOR_API observer_handle {
 public:
  observer_handle(log_function* logger_impl);
  ~observer_handle();
  observer_handle(observer_handle&) = delete;
  observer_handle(observer_handle&&);
  void start();

 private:
  observer* impl;
};
}  // namespace conductor
