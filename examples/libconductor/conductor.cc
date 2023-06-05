#define LIBCONDUCTOR_EXPORT
#define BOOST_ALL_NO_LIB

#include "conductor.h"

#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
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
#include "websocketpp/client.hpp"

using server_type = websocketpp::server<websocketpp::config::asio>;
using client_type = websocketpp::client<websocketpp::config::asio>;
using message_ptr = server_type::message_ptr;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace conductor {
class observer : public webrtc::PeerConnectionObserver,
                 public webrtc::SetSessionDescriptionObserver,
                 public webrtc::CreateSessionDescriptionObserver,
                 public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  template <typename... types>
  void log_wrapper(bool is_error, types&&... arguments) {
    std::wstringstream stream{};
    ((stream << arguments), ...);
    logger_impl(client, is_error, stream.str().c_str());
  }

  template <typename... types>
  void log(types&&... arguments) {
    log_wrapper(false, arguments...);
  }

  template <typename... types>
  void log_error(types&&... arguments) {
    log_wrapper(true, arguments...);
  }

  observer(void* client, log_function* logger_impl, on_video* on_video_impl)
      : observer{} {
    this->client = client;
    this->logger_impl = logger_impl;
    this->on_video_impl = on_video_impl;
  }

  void start_signal_server() {
    server_thread = std::thread{[this] {
      signal_thread = rtc::Thread::CreateWithSocketServer();
      worker_thread = rtc::Thread::CreateWithSocketServer();
      signal_thread->Start();
      worker_thread->Start();
      const auto pc_factory = webrtc::CreatePeerConnectionFactory(
          nullptr, worker_thread.get(), signal_thread.get(), nullptr,
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

      // server.init_asio();
      // server.set_message_handler(
      //     websocketpp::lib::bind(&observer::on_message, this, ::_1, ::_2));

      // server.set_open_handler(
      //     websocketpp::lib::bind(&observer::on_open, this, ::_1));

      // server.listen(9002);
      // server.start_accept();
      // log("Starting websocket server");
      // server.run();
      // log("Stopping websocket server");
      
      socket.init_asio();
      socket.set_message_handler(websocketpp::lib::bind(&observer::on_message, this, ::_1, ::_2));
      socket.set_open_handler(websocketpp::lib::bind(&observer::on_open, this, ::_1));
      
      websocketpp::lib::error_code ec;
      const auto c = socket.get_connection("ws://18.236.77.108:9003", ec);
      socket.connect(c);
      socket.run();
    }};
  }

  ~observer() {
    //server.stop_listening();
    if (connection) {
      connection->close(websocketpp::close::status::normal, "Server exiting");
      log("Shutting down->");
    }

    server_thread.join();
    if (peer_connection) {
      signal_thread->BlockingCall([this] { peer_connection->Close(); });
    }

    log("Destructor completed");
  }

 private:
  void* client{};
  std::unique_ptr<rtc::Thread> signal_thread{};
  std::unique_ptr<rtc::Thread> worker_thread{};
  log_function* logger_impl{};
  on_video* on_video_impl{};
  std::thread server_thread{};
  client_type socket{};
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection{};
  rtc::scoped_refptr<webrtc::VideoTrackInterface> track{};
  server_type::connection_ptr connection{};
  std::uint64_t frames{};
  std::mutex big_lock{};

  void on_message(websocketpp::connection_hdl hdl, message_ptr message) {
    // std::lock_guard guard{big_lock};

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
    // std::lock_guard guard{big_lock};
    log("Connection opened\n");
    if (connection) {
      connection->close(websocketpp::close::status::going_away,
                        "Ruh-roh raggy...");
      log_error("Force-closed connection");
    }

    connection = socket.get_con_from_hdl(hdl);
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
    socket.send(connection, boost::json::serialize(data),
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
    // std::lock_guard guard{big_lock};
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
    socket.send(connection, boost::json::serialize(msg),
                websocketpp::frame::opcode::text);
  }

  void OnSuccess() override { log_error("Succeeded!\n"); }

  void OnFailure(webrtc::RTCError error) override {
    log_error("Failed: ", error.message(), "\n");
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    // std::lock_guard guard{big_lock};
    if (track) {
      log_error("Expected only one track!");
    }

    log("Added track of type: ",
        cricket::MediaTypeToString(transceiver->media_type()).c_str(), "\n");
    auto maybe_track = transceiver->receiver()->track();
    if (maybe_track->enabled())
      log("Track is enabled\n");

    if (maybe_track->kind() != track->kVideoKind) {
      log_error("Not the expected video track!");
    }

    track = static_cast<webrtc::VideoTrackInterface*>(maybe_track.get());
    track->AddOrUpdateSink(this, {});
  }

  void OnFrame(const webrtc::VideoFrame& frame) override {
    // std::lock_guard guard{big_lock};
    ++frames;
    if (frames % 600 == 0)
      log("Another 600 frames were received...");

    on_video_impl(
        client, frame.width(), frame.height(),
        [](void* raw_frame, std::uint8_t* buffer, std::uint64_t size) {
          const auto& frame =
              *reinterpret_cast<const webrtc::VideoFrame*>(raw_frame);

          const auto expected_size = webrtc::CalcBufferSize(
              webrtc::VideoType::kARGB, frame.width(), frame.height());

          if (expected_size > size)
            return false;

          webrtc::ConvertFromI420(frame, webrtc::VideoType::kARGB, 0, buffer);

          return true;
        },
        const_cast<void*>(static_cast<const void*>(&frame)));
  }

  observer() = default;
};

observer_handle::observer_handle(void* client,
                                 log_function* logger_impl,
                                 on_video* on_video_impl)
    : impl{rtc::make_ref_counted<observer>(client, logger_impl, on_video_impl)
               .release()} {}

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