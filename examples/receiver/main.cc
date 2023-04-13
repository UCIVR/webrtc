#include <api/create_peerconnection_factory.h>
#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include <iostream>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"

namespace receiver {
class Observer : public webrtc::PeerConnectionObserver {
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}

  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {}

  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}

  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
  }
};
}  // namespace receiver

int main() {
  const auto signaling_thread = rtc::Thread::CreateWithSocketServer();
  signaling_thread->Start();
  const auto pc_factory = webrtc::CreatePeerConnectionFactory(
      nullptr, nullptr, signaling_thread.get(), nullptr,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);

  if (!pc_factory) {
    std::cerr << "Failed to create PeerConnectionFactory\n";
    return -1;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config{};
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionInterface::IceServer server{};
  server.uri = "stun:localhost:3478";
  config.servers.push_back(server);

  receiver::Observer observer{};
  const auto maybe_pc = pc_factory->CreatePeerConnectionOrError(
      config, webrtc::PeerConnectionDependencies{&observer});
}
