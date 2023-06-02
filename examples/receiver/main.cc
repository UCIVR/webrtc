#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include <iostream>
#include <mutex>
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
using websocketpp::lib::bind;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

namespace receiver {
template <typename consumer_type>
class webrtc_observer : public webrtc::PeerConnectionObserver,
                        public webrtc::SetSessionDescriptionObserver,
                        public webrtc::CreateSessionDescriptionObserver {
 public:
  webrtc_observer(consumer_type& consumer)
      : connection{},
        signaling_thread{},
        pc_factory{},
        peer_connection{},
        track{},
        consumer{consumer} {
    init_webrtc();
  }

  void start(server_type& server) { this->server = &server; }

  void add_track(
      const rtc::scoped_refptr<webrtc::MediaStreamTrackInterface>& track) {
    this->track = track;
  }

  void close() {
    std::cout << "[INFO] Closing Peer\n";
    track = nullptr;
    if (peer_connection) {
      std::cout << "[INFO] Closing PeerConnection\n";
      peer_connection->Close();
      peer_connection = nullptr;
    }

    if (!connection.expired()) {
      std::cout << "[INFO] Explicitly closing websocket\n";
      server->close(connection, websocketpp::close::status::going_away,
                    "Exited from console");

      connection = {};
    }
  }

  void on_message(websocketpp::connection_hdl hdl, message_ptr message) {
    if (hdl.lock() != connection.lock()) {
      std::cout << "Wrong socket!\n";
      return;
    }

    if (message->get_opcode() != 1) {
      std::cout << "I don't know how to use this frame\n";
      return;
    }

    auto payload = boost::json::parse(message->get_payload()).as_object();
    if (payload.contains("offer")) {
      auto offer = payload["offer"].as_object();

      create_peer();
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
        std::cout << "Failed to parse ICE candidate: " << error.description
                  << "\n";
        return;
      }

      peer_connection->AddIceCandidate(
          std::move(candidate), [](webrtc::RTCError error) {
            if (!error.ok()) {
              std::cout << "[ERROR] Failed to set ICE candidate with error: "
                        << error.message() << "\n";
            }
          });
    }
  }

  void on_open(websocketpp::connection_hdl hdl) {
    if (connection.expired()) {
      std::cout << "[INFO] Connection opened\n";
      connection = hdl;
    } else {
      std::cout << "[WARNING] Rejecting connection\n";
      server->close(hdl, websocketpp::close::status::subprotocol_error,
                    "Rejected connection; other client already present");
    }
  }

 private:
  websocketpp::connection_hdl connection{};
  std::unique_ptr<rtc::Thread> signaling_thread{};
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory{};
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection{};
  rtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track{};
  consumer_type& consumer{};
  server_type* server{};

  void init_webrtc() {
    signaling_thread = rtc::Thread::CreateWithSocketServer();
    signaling_thread->Start();

    rtc::LogMessage::LogToDebug(rtc::LS_WARNING);
    pc_factory = webrtc::CreatePeerConnectionFactory(
        nullptr, nullptr, signaling_thread.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

    if (!pc_factory) {
      std::cout << "Failed to create PeerConnectionFactory\n";
      std::exit(-1);
    }
  }

  void create_peer() {
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
      std::cout << "Failed to create PeerConnection\n";
      std::exit(-1);
    }

    std::cout << "[INFO] Created Peer\n";
    peer_connection = std::move(maybe_pc.value());

    if (track) {
      const std::vector<std::string> stream_ids{};
      const auto result = peer_connection->AddTrack(track, stream_ids);
      if (!result.ok()) {
        std::cout << "[ERROR] Could not add track to audience: "
                  << result.error().message() << "\n";
      }
    }
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}

  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {}

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    std::cout << "[WARNING] ICE gathering state change: " << [state] {
      switch (state) {
        case decltype(state)::kIceGatheringComplete:
          return "Complete";

        case decltype(state)::kIceGatheringGathering:
          return "Gathering";

        case decltype(state)::kIceGatheringNew:
          return "New";
      }
    }() << "\n";
  }

  void OnStandardizedIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override {
    std::cout << "[INFO] ICE connection state change: " << [this, state] {
      switch (state) {
        case decltype(state)::kIceConnectionChecking:
          return "Checking";

        case decltype(state)::kIceConnectionClosed:
          close();
          return "Closed";

        case decltype(state)::kIceConnectionCompleted:
          return "Completed";

        case decltype(state)::kIceConnectionConnected:
          return "Connected";

        case decltype(state)::kIceConnectionDisconnected:
          close();
          return "Disconnected";

        case decltype(state)::kIceConnectionFailed:
          close();
          return "Failed";

        case decltype(state)::kIceConnectionMax:
          return "Max";

        case decltype(state)::kIceConnectionNew:
          return "New";
      }
    }() << "\n";
  }

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    std::string blob{};
    if (!candidate->ToString(&blob)) {
      std::cout << "[ERROR] Failed to serialize ICE candidate\n";
      return;
    }

    boost::json::object data{};
    boost::json::object inner_blob{};
    inner_blob["candidate"] = blob;
    inner_blob["sdpMid"] = candidate->sdp_mid();
    inner_blob["sdpMLineIndex"] = candidate->sdp_mline_index();
    data["iceCandidate"] = inner_blob;
    server->send(connection, boost::json::serialize(data),
                 websocketpp::frame::opcode::text);
  }

  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) {
    std::cout << "[INFO] Connection state change: " << [state] {
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
    }() << "\n";
  }

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::cout << "[INFO] Created local session description\n";
    peer_connection->SetLocalDescription(this, desc);
    boost::json::object data{};
    data["type"] = webrtc::SdpTypeToString(desc->GetType());
    std::string sdp{};
    if (!desc->ToString(&sdp)) {
      std::cout << "[ERROR] Failed to serialize SDP\n";
      return;
    }

    data["sdp"] = sdp;
    boost::json::object msg{};
    msg["answer"] = data;
    server->send(connection, boost::json::serialize(msg),
                 websocketpp::frame::opcode::text);
  }

  void OnSuccess() override { std::cout << "[INFO] Succeeded\n"; }

  void OnFailure(webrtc::RTCError error) override {
    std::cout << "[ERROR] Failed: " << error.message() << "\n";
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    std::cout << "[INFO] Added track of type: "
              << cricket::MediaTypeToString(transceiver->media_type()) << "\n";
    track = transceiver->receiver()->track();
    if (track->enabled())
      std::cout << "[INFO] Track is enabled\n";

    consumer.on_track(transceiver);
  }
};

template <typename implementer_type>
class socket_server {
 public:
  socket_server(implementer_type& implementer, std::mutex& exit_lock)
      : implementer{implementer},
        server{},
        connection{},
        waiter_thread{[&exit_lock, this] {
          exit_lock.lock();
          std::cout << "1\n";
          server.stop_listening();
          std::cout << "2\n";
          this->implementer.close();
          std::cout << "3\n";
          server.stop();
          std::cout << "4\n";
          exit_lock.unlock();
        }} {}

  void start(unsigned port) {
    try {
      server.init_asio();
      server.set_message_handler(
          websocketpp::lib::bind(&socket_server::on_message, this, ::_1, ::_2));

      server.set_open_handler(
          websocketpp::lib::bind(&socket_server::on_open, this, ::_1));

      server.listen(port);
      server.start_accept();
      implementer.start(server);
      server.run();
      stop();
    } catch (const websocketpp::exception& error) {
      std::cout << "[ERROR] Websocket: " << error.what() << "\n";
      throw;
    }
  }

 private:
  implementer_type& implementer;
  server_type server{};
  websocketpp::connection_hdl connection{};
  std::thread waiter_thread{};

  void stop() {
    implementer.close();
    waiter_thread.join();
  }

  void on_message(websocketpp::connection_hdl hdl, message_ptr message) {
    implementer.on_message(hdl, message);
  }

  void on_open(websocketpp::connection_hdl hdl) { implementer.on_open(hdl); }
};

struct null_consumer {
  void on_track(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    std::cout << "[INFO] Null consumer saw new track\n";
  }
};

struct presenter_consumer {
  null_consumer consumer;

  using audience_type = webrtc_observer<null_consumer>;
  decltype(rtc::make_ref_counted<audience_type>(consumer)) audience;

  socket_server<audience_type> server;
  std::thread server_thread;

  presenter_consumer(std::mutex& exit_lock, unsigned port)
      : consumer{},
        audience{rtc::make_ref_counted<audience_type>(consumer)},
        server{*audience, exit_lock},
        server_thread{[this, port] { server.start(port); }} {}

  ~presenter_consumer() { server_thread.join(); }

  void on_track(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    std::cout << "[INFO] Presenter consumer saw new track\n";
    audience->add_track(transceiver->receiver()->track());
  }
};

}  // namespace receiver

// TODO: THE AUDIENCE MUST ADD AN EMPTY DATA CHANNEL OR THE HANDSHAKE WILL FAIL

int main() {
  using namespace receiver;

  std::mutex exit_lock{};
  std::thread input_thread{[&exit_lock] {
    exit_lock.lock();
    std::string input{};
    while (input != "exit" && !std::cin.eof())
      std::cin >> input;

    exit_lock.unlock();
  }};

  // Janky, but the idea is to wait for the lock to be held by input_thread;
  while (exit_lock.try_lock())
    exit_lock.unlock();

  presenter_consumer consumer{exit_lock, 9003};
  using presenter = webrtc_observer<presenter_consumer>;
  const auto presenter_stream = rtc::make_ref_counted<presenter>(consumer);
  socket_server presenter_server{*presenter_stream, exit_lock};
  presenter_server.start(9002);
  input_thread.join();
}
