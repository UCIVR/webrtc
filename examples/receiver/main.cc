#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

#define BOOST_ALL_NO_LIB
#include <boost/json.hpp>
#include <boost/json/src.hpp>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "websocketpp/config/asio_no_tls.hpp"
#include "websocketpp/server.hpp"

using server_type = websocketpp::server<websocketpp::config::asio>;
using message_ptr = server_type::message_ptr;

namespace receiver {
enum class level { error, warning, info };

constexpr auto name(level l) {
  switch (l) {
    case level::error:
      return "error";

    case level::warning:
      return "warning";

    case level::info:
      return "info";
  }
}

template <typename... types>
void log(level severity, types&&... args) {
  std::cout << "[relay:" << name(severity) << "]";
  ((std::cout << ' ' << std::forward<types>(args)), ...);
  std::cout << std::endl;
}

struct webrtc_factory {
  std::unique_ptr<rtc::Thread> signal_thread{};
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory{};

  webrtc_factory()
      : signal_thread{rtc::Thread::CreateWithSocketServer()}, factory{} {
    signal_thread->Start();
    factory = webrtc::CreatePeerConnectionFactory(
        nullptr, nullptr, signal_thread.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

    if (!factory)
      throw std::runtime_error{"Failed to create PeerConnectionFactory"};
  }
};

template <typename derived>
class socket_server {
 public:
  void start(unsigned port) {
    server.init_asio();
    server.set_reuse_addr(true);
    server.set_message_handler([this](auto&&... args) {
      log(level::info, "message received");
      self().on_message(std::forward<decltype(args)>(args)...);
    });

    server.set_open_handler([this](auto&&... args) {
      log(level::info, "socket opened");
      self().on_open(std::forward<decltype(args)>(args)...);
    });

    server.set_close_handler([this](auto&&... args) {
      log(level::info, "socket closed");
      self().on_close(std::forward<decltype(args)>(args)...);
    });

    server.listen(port);
    server.start_accept();
    server_thread = std::thread{[this] { server.run(); }};
  }

  void shut_down() {
    server.stop_listening();
    self().close_all();
    server.stop();
    server_thread.join();
  }

 protected:
  server_type server{};

 private:
  std::thread server_thread{};

  auto& self() { return *reinterpret_cast<derived*>(this); }

  template <typename... types>
  void on_open(types&&...) {}

  template <typename... types>
  void on_message(types&&...) {}

  template <typename... types>
  void on_close(types&&...) {}

  void close_all() {}
};

class source_server : public socket_server<source_server> {
 public:
  template <typename... types>
  void on_open(websocketpp::connection_hdl hdl, types&&...) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connection) {
      log(level::warning, "rejecting sink connection; one already exists");
      return;
    }

    connection = new_connection;
  }

  void on_close(websocketpp::connection_hdl hdl) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connection == new_connection) {
      log(level::warning, "source disconnected");
      connection = nullptr;
    }
  }

  template <typename... types>
  void on_message(types&&...) {}

  void close_all() {
    if (connection) {
      log(level::info, "closing source connection");
      connection->close(websocketpp::close::status::going_away,
                        "Server shutting down");
    }
  }

 private:
  decltype(server)::connection_ptr connection{};
};

class sink_server : public socket_server<sink_server> {
 public:
  template <typename... types>
  void on_open(websocketpp::connection_hdl hdl, types&&...) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    connections.insert(new_connection);
  }

  void on_close(websocketpp::connection_hdl hdl) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connections.find(new_connection) != connections.end()) {
      log(level::warning, "source disconnected");
      connections.erase(new_connection);
    }
  }

  template <typename... types>
  void on_message(types&&...) {}

  void close_all() {
    log(level::info, "closing source connections");
    for (const auto connection : connections) {
      connection->close(websocketpp::close::status::going_away,
                        "Server shutting down");
    }

    connections.clear();
  }

 private:
  std::set<decltype(server)::connection_ptr> connections{};
};

template <typename type>
class scoped_session {
 public:
  scoped_session(type& server, unsigned port) : server{server} {
    server.start(port);
  }

  ~scoped_session() { server.shut_down(); }

 private:
  type& server;
};
}  // namespace receiver

int main() {
  using namespace receiver;

  try {
    rtc::LogMessage::LogToDebug(rtc::LS_ERROR);
    source_server source{};
    sink_server sink{};
    scoped_session source_session{source, 9002};
    scoped_session sink_session{sink, 9003};
    webrtc_factory factory{};
    webrtc_factory factory1{};

    std::string input{};
    while (std::cin >> input) {
      if (input == "exit")
        break;
    }
  } catch (const std::exception& error) {
    log(level::error, error.what());
    return -1;
  }
}
