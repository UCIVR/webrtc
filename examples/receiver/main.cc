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

std::mutex log_lock{};
std::ofstream log_file{"relay.log"};

template <typename... types>
void log_to(std::ostream& stream, level severity, types&&... args) {
  stream << "[relay:" << name(severity) << "]";
  ((stream << ' ' << std::forward<types>(args)), ...);
  stream << std::endl;
}

template <typename... types>
void log(level severity, types&&... args) {
  std::lock_guard guard{log_lock};
  log_to(std::cerr, severity, std::forward<types>(args)...);
  log_to(log_file, severity, std::forward<types>(args)...);
}

struct webrtc_factory {
  const static std::unique_ptr<rtc::Thread> signal_thread;

  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory{};

  webrtc_factory() : factory{} {
    factory = webrtc::CreatePeerConnectionFactory(
        nullptr, nullptr, signal_thread.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

    if (!factory)
      throw std::runtime_error{"Failed to create PeerConnectionFactory"};
  }

  auto operator->() { return factory.operator->(); }
};

const std::unique_ptr<rtc::Thread> webrtc_factory::signal_thread{[] {
  auto thread = rtc::Thread::CreateWithSocketServer();
  thread->Start();
  return thread;
}()};

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
    // self().close_all();
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

using track_callback =
    std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)>;

// TODO: stop the relayed stream before destruction completes, to avoid a racy segfault
// TODO: implement renegotiation so the source can be switched out?
class webrtc_observer : public webrtc::PeerConnectionObserver,
                        public webrtc::CreateSessionDescriptionObserver,
                        public webrtc::SetSessionDescriptionObserver {
 public:
  webrtc_observer(server_type::connection_ptr signal_socket,
                  track_callback on_track,
                  rtc::scoped_refptr<webrtc::RtpTransceiverInterface> track)
      : factory{},
        peer{},
        signal_socket{signal_socket},
        on_track{on_track},
        senders{} {
    peer = create_peer(this, track);
    signal_socket->set_message_handler(
        [this](websocketpp::connection_hdl hdl,
               server_type::message_ptr message) { on_message(hdl, message); });
  }

  ~webrtc_observer() { close(); }

  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<webrtc_observer>(std::forward<types>(args)...);
  }

 private:
  webrtc_factory factory;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
  server_type::connection_ptr signal_socket;
  track_callback on_track;
  std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>> senders{};

  void close() {
    peer->Close();
    log(level::info, reinterpret_cast<std::uintptr_t>(this), "closing peer");
  }

  static rtc::scoped_refptr<webrtc::PeerConnectionInterface> create_peer(
      webrtc_observer* host,
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> track) {
    webrtc::PeerConnectionInterface::RTCConfiguration config{};
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer turner{};
    turner.uri = "turn:54.200.166.206:3478?transport=tcp";
    turner.username = "user";
    turner.password = "root";
    config.servers.emplace_back(std::move(turner));

    const auto maybe_pc = host->factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies{host});

    if (!maybe_pc.ok()) {
      throw std::runtime_error{"failed to create PeerConnection"};
    }

    log(level::info, "created PeerConnection\n");

    auto peer = maybe_pc.value();
    if (track) {
      const auto real_track = track->receiver()->track();
      const auto sender = peer->AddTrack(real_track, {"mirrored_stream"});
      if (!sender.ok()) {
        log(level::error, "failed to add track:", sender.error().message());
      } else {
        log(level::info, "added track to peer");
        host->senders.emplace_back(sender.value());
      }
    }

    return std::move(peer);
  }

  void on_message(websocketpp::connection_hdl hdl,
                  server_type::message_ptr message) {
    // TODO: we really should check that it's the expected hdl here
    const auto opcode = message->get_opcode();
    if (opcode != 1) {
      log(level::warning, reinterpret_cast<std::uintptr_t>(this),
          "I don't know how to use this frame", opcode);

      return;
    }

    auto payload = boost::json::parse(message->get_payload()).as_object();
    if (payload.contains("offer")) {
      auto offer = payload["offer"].as_object();
      peer->SetRemoteDescription(
          this, webrtc::CreateSessionDescription(
                    webrtc::SdpTypeFromString(offer["type"].as_string().c_str())
                        .value(),
                    offer["sdp"].as_string().c_str())
                    .release());

      peer->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
    } else if (payload.contains("new-ice-candidate")) {
      auto blob = payload["new-ice-candidate"].as_object();
      webrtc::SdpParseError error{};
      std::unique_ptr<webrtc::IceCandidateInterface> candidate{
          webrtc::CreateIceCandidate(blob["sdpMid"].as_string().c_str(),
                                     blob["sdpMLineIndex"].as_int64(),
                                     blob["candidate"].as_string().c_str(),
                                     &error)};

      if (!candidate) {
        log(level::error, reinterpret_cast<std::uintptr_t>(this),
            "failed to parse ICE candidate: ", error.description);
        return;
      }

      peer->AddIceCandidate(
          std::move(candidate), [this](webrtc::RTCError error) {
            if (!error.ok())
              log(level::error, reinterpret_cast<std::uintptr_t>(this),
                  "failed to set ICE candidate with error:", error.message());
          });
    }
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {
    const auto name = [new_state] {
      switch (new_state) {
        case decltype(new_state)::kStable:
          return "kStable";

        case decltype(new_state)::kHaveLocalOffer:
          return "kHaveLocalOffer";

        case decltype(new_state)::kHaveLocalPrAnswer:
          return "kHaveLocalPrAnswer";

        case decltype(new_state)::kHaveRemoteOffer:
          return "kHaveRemoteOffer";

        case decltype(new_state)::kHaveRemotePrAnswer:
          return "kHaveRemotePrAnswer";

        case decltype(new_state)::kClosed:
          return "kClosed";
      }
    }();

    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "Signaling state change:", name);
  }

  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "Added data channel to peer");

    // TODO: remove me
    webrtc::DataChannelInit channel{};
    peer->CreateDataChannel("I Am a Teapot", &channel);
  }

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "ICE gathering state change:", [state] {
          switch (state) {
            case decltype(state)::kIceGatheringComplete:
              return "Complete";

            case decltype(state)::kIceGatheringGathering:
              return "Gathering";

            case decltype(state)::kIceGatheringNew:
              return "New";
          }
        }());
  }

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string blob{};
    if (!candidate->ToString(&blob)) {
      log(level::error, reinterpret_cast<std::uintptr_t>(this),
          "failed to serialize ICE candidate");
      return;
    }

    boost::json::object data{};
    boost::json::object inner_blob{};
    inner_blob["candidate"] = blob;
    inner_blob["sdpMid"] = candidate->sdp_mid();
    inner_blob["sdpMLineIndex"] = candidate->sdp_mline_index();
    data["iceCandidate"] = inner_blob;
    signal_socket->send(boost::json::serialize(data),
                        websocketpp::frame::opcode::text);
  }

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "created local session description");
    peer->SetLocalDescription(this, desc);
    boost::json::object data{};
    data["type"] = webrtc::SdpTypeToString(desc->GetType());
    std::string sdp{};
    if (!desc->ToString(&sdp)) {
      log(level::error, reinterpret_cast<std::uintptr_t>(this),
          "failed to serialize SDP");

      return;
    }

    data["sdp"] = sdp;
    boost::json::object msg{};
    msg["answer"] = data;
    signal_socket->send(boost::json::serialize(msg),
                        websocketpp::frame::opcode::text);
  }

  void OnSuccess() override {
    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "SetSessionDescription succeeded");
  }

  void OnFailure(webrtc::RTCError error) override {
    log(level::error, reinterpret_cast<std::uintptr_t>(this),
        "SetSessionDescription/CreateSessionDescription failed:",
        error.message());
  }

  void OnTrack(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    on_track(transceiver);
  }
};

using peer_ptr =
    rtc::scoped_refptr<rtc::FinalRefCountedObject<webrtc_observer>>;

class sink_server : public socket_server<sink_server> {
 public:
  template <typename... types>
  void on_open(websocketpp::connection_hdl hdl, types&&...) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    const auto maybe = connections.find(new_connection);
    if (maybe == connections.end()) {
      const auto peer = webrtc_observer::make(
          new_connection, [](auto&&...) {}, transceiver);
      connections[new_connection] = peer;
    }
  }

  void on_close(websocketpp::connection_hdl hdl) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    auto maybe = connections.find(new_connection);
    if (maybe != connections.end()) {
      log(level::warning, "source disconnected");
      auto& [connection, peer] = *maybe;
      peer = nullptr;
      connections.erase(maybe);
    }
  }

  template <typename... types>
  void on_message(types&&...) {}

  void close_all() {
    log(level::info, "closing source connections");
    for (auto& [connection, peer] : connections) {
      connection->close(websocketpp::close::status::going_away,
                        "Server shutting down");

      peer = nullptr;
    }

    connections.clear();
  }

  void switch_source(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    log(level::info, "switching sources");
    this->transceiver = transceiver;
    // TODO: OnNegotiationNeeded?
    // for (const auto& [conn, peer] : connections)
    //   peer->add_track(transceiver);
  }

 private:
  std::map<decltype(server)::connection_ptr, peer_ptr> connections{};

  std::mutex track_lock;
  rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver;
};

class source_server : public socket_server<source_server> {
 public:
  source_server(sink_server& sink)
      : socket_server<source_server>{}, sink{sink}, connection{}, peer{} {}

  template <typename... types>
  void on_open(websocketpp::connection_hdl hdl, types&&...) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connection) {
      log(level::warning, "rejecting sink connection; one already exists");
      return;
    }

    connection = new_connection;
    peer = webrtc_observer::make(
        connection,
        [this](rtc::scoped_refptr<webrtc::RtpTransceiverInterface> track) {
          on_track(track);
        },
        nullptr);
  }

  void on_close(websocketpp::connection_hdl hdl) {
    const auto new_connection = server.get_con_from_hdl(hdl);
    if (connection == new_connection) {
      log(level::warning, "source disconnected");
      connection = nullptr;
      peer = nullptr;
    }
  }

  template <typename... types>
  void on_message(types&&...) {}

  void on_track(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> track) {
    log(level::info, "track added",
        reinterpret_cast<std::uintptr_t>(track.get()));

    if (track->receiver()->track()->enabled())
      log(level::info, "track enabled",
          reinterpret_cast<std::uintptr_t>(track.get()));

    sink.switch_source(track);
  }

  void close_all() {
    if (connection) {
      log(level::info, "closing source connection");
      connection->close(websocketpp::close::status::going_away,
                        "Server shutting down");

      peer = nullptr;
    }
  }

 private:
  sink_server& sink;
  decltype(server)::connection_ptr connection;
  peer_ptr peer;
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
    sink_server sink{};
    source_server source{sink};
    scoped_session source_session{source, 9002};
    scoped_session sink_session{sink, 9003};

    std::string input{};
    while (std::cin >> input) {
      if (input == "exit")
        break;
    }
  } catch (const std::exception& error) {
    log(level::error, "fatal error: ", error.what());
    return -1;
  }
}
