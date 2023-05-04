#define LIBCONDUCTOR_EXPORT
#define BOOST_ALL_NO_LIB

#include "conductor.h"

#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <iostream>
#include <thread>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "websocketpp/config/asio_no_tls.hpp"
#include "websocketpp/server.hpp"

using server_type = websocketpp::server<websocketpp::config::asio>;
using message_ptr = server_type::message_ptr;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace conductor {
class observer : public webrtc::PeerConnectionObserver,
                 public webrtc::SetSessionDescriptionObserver,
                 public webrtc::CreateSessionDescriptionObserver {
 public:
  template <typename... types>
  void log_wrapper(bool is_error, types&&... arguments) {
    std::wstringstream stream{};
    ((stream << arguments), ...);
    logger_impl(is_error, stream.str().c_str());
  }

  template <typename... types>
  void log(types&&... arguments) {
    log_wrapper(false, arguments...);
  }

  template <typename... types>
  void log_error(types&&... arguments) {
    log_wrapper(true, arguments...);
  }

  observer(log_function* logger_impl) : observer{} {
    this->logger_impl = logger_impl;
  }

  void start_signal_server() {
    signal_thread = rtc::Thread::CreateWithSocketServer();
    signal_thread->Start();
    const auto pc_factory = webrtc::CreatePeerConnectionFactory(
        nullptr, nullptr, signal_thread.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

    if (!pc_factory) {
      log_error("Failed to create PeerConnectionFactory\n");
      std::exit(-1);
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config{};
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer turner{};
    turner.uri = "turn:54.200.166.206:3478?transport=tcp";
    turner.username = "user";
    turner.password = "root";
    config.servers.emplace_back(std::move(turner));

    const auto maybe_pc = pc_factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies{this});

    if (!maybe_pc.ok()) {
      log_error(L"Failed to create PeerConnection\n");
      std::exit(-1);
    }

    peer_connection = std::move(maybe_pc.value());
    server_thread = std::thread{[this] {
      server.init_asio();
      server.set_message_handler(
          websocketpp::lib::bind(&observer::on_message, this, ::_1, ::_2));

      server.set_open_handler(
          websocketpp::lib::bind(&observer::on_open, this, ::_1));

      server.listen(9002);
      server.start_accept();
      log("Starting websocket server");
      server.run();
      log("Stopping websocket server");
    }};
  }

  ~observer() {
    server.stop_listening();
    if (connection) {
      connection->close(websocketpp::close::status::normal, "Server exiting");
      log("Shutting down->");
    }

    server_thread.join();
    if (peer_connection) {
      signal_thread->BlockingCall([this] {peer_connection->Close();});
    }

    log("Destructor completed");
  }

 private:
  std::unique_ptr<rtc::Thread> signal_thread{};
  log_function* logger_impl{};
  std::thread server_thread{};
  server_type server{};
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection{};
  rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track{};
  server_type::connection_ptr connection{};

  void on_message(websocketpp::connection_hdl hdl, message_ptr message) {
    if (hdl.lock() != connection) {
      log_error("Wrong socket!\n");
      return;
    }

    if (message->get_opcode() != 1) {
      log_error("I don't know how to use this frame\n");
      return;
    }

    log("Received websocket message");
    auto payload = boost::json::parse(message->get_payload()).as_object();
    if (payload.contains("offer")) {
      auto offer = payload["offer"].as_object();

      peer_connection->SetRemoteDescription(
          this, webrtc::CreateSessionDescription(
                    webrtc::SdpTypeFromString(offer["type"].as_string().c_str())
                        .value(),
                    offer["sdp"].as_string().c_str())
                    .release());

      peer_connection->CreateAnswer(
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
        log_error("Failed to parse ICE candidate: ", error.description.c_str(),
                  "\n");
        return;
      }

      peer_connection->AddIceCandidate(
          std::move(candidate), [this](webrtc::RTCError error) {
            if (!error.ok()) {
              log("Failed to set ICE candidate with error: ", error.message(),
                  "\n");
            }
          });
    }
  }

  void on_open(websocketpp::connection_hdl hdl) {
    log("Connection opened\n");
    if (connection) {
      connection->close(websocketpp::close::status::going_away,
                        "Ruh-roh raggy...");
      log_error("Force-closed connection");
    }

    connection = server.get_con_from_hdl(hdl);
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}

  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {}

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    log(
        "ICE gathering state change: ",
        [state] {
          switch (state) {
            case decltype(state)::kIceGatheringComplete:
              return "Complete";

            case decltype(state)::kIceGatheringGathering:
              return "Gathering";

            case decltype(state)::kIceGatheringNew:
              return "New";
          }
        }(),
        "\n");
  }

  void OnStandardizedIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    log(
        "ICE connection state change: ",
        [state] {
          switch (state) {
            case decltype(state)::kIceConnectionChecking:
              return "Checking";

            case decltype(state)::kIceConnectionClosed:
              return "Closed";

            case decltype(state)::kIceConnectionCompleted:
              return "Completed";

            case decltype(state)::kIceConnectionConnected:
              return "Connected";

            case decltype(state)::kIceConnectionDisconnected:
              return "Disconnected";

            case decltype(state)::kIceConnectionFailed:
              return "Failed";

            case decltype(state)::kIceConnectionMax:
              return "Max";

            case decltype(state)::kIceConnectionNew:
              return "New";
          }
        }(),
        "\n");
  }

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string blob{};
    if (!candidate->ToString(&blob)) {
      log_error("Failed to serialize ICE candidate\n");
      return;
    }

    boost::json::object data{};
    boost::json::object inner_blob{};
    inner_blob["candidate"] = blob;
    inner_blob["sdpMid"] = candidate->sdp_mid();
    inner_blob["sdpMLineIndex"] = candidate->sdp_mline_index();
    data["iceCandidate"] = inner_blob;
    server.send(connection, boost::json::serialize(data),
                websocketpp::frame::opcode::text);
  }

  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    log(
        "Connection state change: ",
        [state] {
          switch (state) {
            case decltype(state)::kNew:
              return "New";

            case decltype(state)::kFailed:
              return "Failed";

            case decltype(state)::kDisconnected:
              return "Disconnected";

            case decltype(state)::kConnecting:
              return "Connecting";

            case decltype(state)::kConnected:
              return "Connected";

            case decltype(state)::kClosed:
              return "Closed";
          }
        }(),
        "\n");
  }

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    log("Created local session description\n");
    peer_connection->SetLocalDescription(this, desc);
    boost::json::object data{};
    data["type"] = webrtc::SdpTypeToString(desc->GetType());
    std::string sdp{};
    if (!desc->ToString(&sdp)) {
      log_error("Failed to serialize SDP\n");
      return;
    }

    data["sdp"] = sdp;
    boost::json::object msg{};
    msg["answer"] = data;
    server.send(connection, boost::json::serialize(msg),
                websocketpp::frame::opcode::text);
  }

  void OnSuccess() override { log_error("Succeeded!\n"); }

  void OnFailure(webrtc::RTCError error) override {
    log_error("Failed: ", error.message(), "\n");
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    log("Added track of type: ",
        cricket::MediaTypeToString(transceiver->media_type()).c_str(), "\n");
    track = transceiver->receiver()->track();
    if (track->enabled())
      log("Track is enabled\n");
  }

  observer() = default;
};

observer_handle::observer_handle(log_function* logger_impl)
    : impl{rtc::make_ref_counted<observer>(logger_impl).release()} {}

observer_handle::observer_handle(observer_handle&& other) : impl{} {
  using namespace std;
  swap(impl, other.impl);
}

void observer_handle::start() {
  impl->start_signal_server();
}

observer_handle::~observer_handle() {
  if (impl) {
    static_cast<webrtc::CreateSessionDescriptionObserver*>(impl)->Release();
  }
}

}  // namespace conductor
