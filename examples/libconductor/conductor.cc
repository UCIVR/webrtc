#define LIBCONDUCTOR_EXPORT
#define BOOST_ALL_NO_LIB

#include "conductor.h"

#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <rtc_base/thread.h>

#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <fstream>
#include <iostream>
#include <thread>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "websocketpp/client.hpp"
#include "websocketpp/config/asio_no_tls.hpp"
#include "websocketpp/server.hpp"

using server_type = websocketpp::server<websocketpp::config::asio>;
using client_type = websocketpp::client<websocketpp::config::asio>;
using message_ptr = server_type::message_ptr;
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace conductor {
using peercon_ptr = rtc::scoped_refptr<webrtc::PeerConnectionInterface>;

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
void log_to(std::ostream& stream, level severity, types&&... args) {
  stream << "[libconductor:" << name(severity) << "]";
  ((stream << ' ' << std::forward<types>(args)), ...);
  stream << std::endl;
}

template <typename... types>
void log(level severity, types&&... args) {
  std::stringstream stream{};
  log_to(stream, severity, std::forward<types>(args)...);
  OutputDebugStringA(stream.str().c_str());
}

struct webrtc_factory {
  std::unique_ptr<rtc::Thread> signal_thread;
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

  auto operator->() { return factory.operator->(); }
};

using track_callback =
    std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)>;

class local_desc_observer
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<local_desc_observer>(
        std::forward<types>(args)...);
  }

  local_desc_observer(peercon_ptr peer, server_type::connection_ptr socket)
      : peer{peer}, socket{socket} {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) {
      log(level::error, reinterpret_cast<std::uintptr_t>(this),
          "SetLocalDescription failed:", error.message());

      return;
    }

    log(level::info, reinterpret_cast<std::uintptr_t>(this),
        "SetLocalDescription succeeded");

    const auto desc = peer->local_description();
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
    msg["description"] = data;
    socket->send(boost::json::serialize(msg), websocketpp::frame::opcode::text);
  }

 private:
  peercon_ptr peer;
  server_type::connection_ptr socket;
};

class remote_desc_observer
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<remote_desc_observer>(
        std::forward<types>(args)...);
  }

  remote_desc_observer(
      peercon_ptr peer,
      rtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface> observer,
      bool is_offer)
      : peer{peer}, observer{observer}, is_offer{is_offer} {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (is_offer)
      peer->SetLocalDescription(observer);
  }

 private:
  peercon_ptr peer;
  rtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface> observer;
  bool is_offer;
};

class webrtc_observer : public webrtc::PeerConnectionObserver {
 public:
  webrtc_observer(webrtc_factory& factory,
                  server_type::connection_ptr signal_socket,
                  track_callback on_track)
      : factory{factory},
        peer{},
        signal_socket{signal_socket},
        on_track{on_track},
        current_sender{},
        ignore_offer{},
        making_offer{} {
    peer = create_peer(this);
    signal_socket->set_message_handler(
        [this](websocketpp::connection_hdl hdl,
               server_type::message_ptr message) { on_message(hdl, message); });
  }

  ~webrtc_observer() { close(); }

  template <typename... types>
  static auto make(types&&... args) {
    return rtc::make_ref_counted<webrtc_observer>(std::forward<types>(args)...);
  }

  void switch_track(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (current_sender) {
      log(level::info, "removing existing track sender");
      const auto maybe_removed = peer->RemoveTrackOrError(current_sender);
      if (!maybe_removed.ok()) {
        log(level::error, "failed to remove existing track from peer:",
            maybe_removed.message());

        return;
      }
    }

    const auto real_track = transceiver->receiver()->track();
    const auto sender = peer->AddTrack(real_track, {"mirrored_stream"});
    if (!sender.ok())
      log(level::error, "failed to add track:", sender.error().message());
    else
      log(level::info, "added track to peer");
  }

 private:
  static constexpr auto polite = true;

  webrtc_factory& factory;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
  server_type::connection_ptr signal_socket;
  track_callback on_track;
  rtc::scoped_refptr<webrtc::RtpSenderInterface> current_sender;

  bool ignore_offer;
  bool making_offer;

  void close() {
    peer->Close();
    log(level::info, reinterpret_cast<std::uintptr_t>(this), "closing peer");
  }

  static rtc::scoped_refptr<webrtc::PeerConnectionInterface> create_peer(
      webrtc_observer* host) {
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

    return maybe_pc.value();
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
    if (payload.contains("description")) {
      auto offer = payload["description"].as_object();
      auto type = offer["type"].as_string();

      const auto offer_collision =
          type == "offer" &&
          (making_offer ||
           peer->signaling_state() != webrtc::PeerConnectionInterface::kStable);

      ignore_offer = !polite && offer_collision;
      if (ignore_offer)
        return;

      auto description = webrtc::CreateSessionDescription(
          webrtc::SdpTypeFromString(type.c_str()).value(),
          offer["sdp"].as_string().c_str());

      const auto is_offer = type == "offer";
      peer->SetRemoteDescription(
          std::move(description),
          remote_desc_observer::make(
              peer, local_desc_observer::make(peer, signal_socket), is_offer));

    } else if (payload.contains("candidate")) {
      auto blob = payload["candidate"].as_object();
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

  void OnNegotiationNeededEvent(uint32_t id) override {
    factory.signal_thread->PostTask([this, id] {
      if (peer->ShouldFireNegotiationNeededEvent(id)) {
        making_offer = true;
        peer->SetLocalDescription(
            local_desc_observer::make(peer, signal_socket));

        making_offer = false;
      }
    });
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
    data["candidate"] = inner_blob;
    signal_socket->send(boost::json::serialize(data),
                        websocketpp::frame::opcode::text);
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    on_track(transceiver);
  }
};

using peer_ptr =
    rtc::scoped_refptr<rtc::FinalRefCountedObject<webrtc_observer>>;

class observer : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  observer(void* client, on_video* on_video_impl)
      : socket_client{},
        client{client},
        on_video_impl{on_video_impl},
        frames{},
        peer{},
        track{},
        socket_thread{} {
    this->client = client;
    this->on_video_impl = on_video_impl;
  }
  76
  void start(const char* url) {
    socket_client.init_asio();
    std::error_code ec;
    const auto connection = socket_client.get_connection(url, ec);
    if (ec) {
      log(level::error, "Connection did not succeed:", ec.message());
      return;
    }

    signal_socket = connection;

    connection->set_open_handler(
        [](auto&&...) { log(level::info, "connected"); });

    connection->set_close_handler(
        [](auto&&...) { log(level::info, "closed"); });

    connection->set_termination_handler(
        [](auto&&...) { log(level::info, "terminated"); });

    socket_client.connect(connection);
    peer = webrtc_observer::make(factory, connection,
                                 [this](auto track) { on_track(track); });

    socket_thread = std::thread{[this] { socket_client.run(); }};
  }

  ~observer() {
    signal_socket->close(websocketpp::close::status::going_away,
                         "client exiting");

    socket_client.stop();
    socket_thread.join();
    log(level::info, "Destructor completed");
  }

 private:
  client_type socket_client{};
  client_type::connection_ptr signal_socket{};
  webrtc_factory factory{};
  void* client{};
  on_video* on_video_impl{};
  std::uint64_t frames{};
  peer_ptr peer{};
  rtc::scoped_refptr<webrtc::VideoTrackInterface> track{};
  std::thread socket_thread{};

  void on_track(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (track) {
      log(level::error, "Expected only one track!");
    }

    log(level::info, "Added track of type: ",
        cricket::MediaTypeToString(transceiver->media_type()).c_str(), "\n");
    auto maybe_track = transceiver->receiver()->track();
    if (maybe_track->enabled())
      log(level::info, "Track is enabled\n");

    if (maybe_track->kind() != track->kVideoKind) {
      log(level::error, "Not the expected video track!");
    }

    track = static_cast<webrtc::VideoTrackInterface*>(maybe_track.get());
    track->AddOrUpdateSink(this, {});
  }

  void OnFrame(const webrtc::VideoFrame& frame) override {
    // std::lock_guard guard{big_lock};
    ++frames;
    if (frames % 600 == 0)
      log(level::info, "Another 600 frames were received...");

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
};

observer_handle::observer_handle(void* client, on_video* on_video_impl)
    : impl{new observer{client, on_video_impl}} {}

observer_handle::observer_handle(observer_handle&& other) : impl{} {
  using namespace std;
  swap(impl, other.impl);
}

void observer_handle::start(const char* url) {
  impl->start(url);
}

observer_handle::~observer_handle() {
  if (impl) {
    delete impl;
  }
}

}  // namespace conductor
