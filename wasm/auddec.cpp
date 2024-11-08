#include "moonlight_wasm.hpp"

#include <chrono>

#include "samsung/wasm/elementary_media_packet.h"

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;
using TimeStamp = samsung::wasm::Seconds;

static constexpr TimeStamp kAudioBufferMargin = 100ms;

static std::vector<opus_int16> s_DecodeBuffer;

static TimeStamp s_frameDuration;
static TimeStamp s_pktPts;
static TimeStamp s_estimatedAudioEnd;

static size_t s_samplesPerFrame;
static size_t s_channelCount;

static std::chrono::time_point<std::chrono::steady_clock> s_firstAppend;
static bool s_hasFirstFrame = false;

static bool s_AudioSyncEnabled;

static inline TimeStamp FrameDuration(double samplesPerFrame,
                                      double sampleRate) {
  return TimeStamp(samplesPerFrame / sampleRate);
}

static void DecodeAndAppendPacket(samsung::wasm::ElementaryMediaTrack* track,
                                  samsung::wasm::SessionId session_id,
                                  OpusMSDecoder* decoder,
                                  const unsigned char* sampleData,
                                  int sampleLength) {
  int samplesDecoded = opus_multistream_decode(
      decoder,
      sampleData, sampleLength,
      s_DecodeBuffer.data(), 
      s_samplesPerFrame,
      0);

  if (samplesDecoded <= 0) {
    s_DecodeBuffer.assign(s_DecodeBuffer.size(), 0); // Clear buffer on decode error
    return;
  }

  size_t desiredSize = sizeof(opus_int16) * samplesDecoded * s_channelCount;

  samsung::wasm::ElementaryMediaPacket pkt{
     s_pktPts,
     s_pktPts,
     s_frameDuration,
     true,
     desiredSize,
     s_DecodeBuffer.data(),
     0,
     0,
     0,
     0,
     session_id
  };

  if (track->AppendPacket(pkt)) {
    s_pktPts += s_frameDuration;
  } else {
    MoonlightInstance::ClLogMessage("Append audio packet failed\n");
  }

  if (desiredSize > s_DecodeBuffer.size()) {
    s_DecodeBuffer.resize(desiredSize);
  }
}

int MoonlightInstance::AudDecInit(int audioConfiguration,
                                  POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                                  void* context, int flags) {
  int rc;
  ClLogMessage("MoonlightInstance::AudDecSetup\n");
  s_pktPts = 0s;
  s_channelCount = opusConfig->channelCount; // 2 6 8
  s_samplesPerFrame = opusConfig->samplesPerFrame; // 240 = 48 * 5
  size_t pcmBufferSize = sizeof(opus_int16) * s_samplesPerFrame * s_channelCount ; // 960 2880 3840
  s_DecodeBuffer.resize(pcmBufferSize);
  s_frameDuration = FrameDuration(opusConfig->samplesPerFrame, // 240
                                  opusConfig->sampleRate); // 48000

  g_Instance->m_OpusDecoder =
      opus_multistream_decoder_create(opusConfig->sampleRate,
                                      opusConfig->channelCount,
                                      opusConfig->streams,
                                      opusConfig->coupledStreams,
                                      opusConfig->mapping,
                                      &rc);
  s_estimatedAudioEnd = 0s;
  s_hasFirstFrame = false;

  s_AudioSyncEnabled = g_Instance->m_AudioSyncEnabled;
  return 0;
}

void MoonlightInstance::AudDecCleanup(void) {
  s_DecodeBuffer.clear();
  s_DecodeBuffer.shrink_to_fit();
}

void MoonlightInstance::AudDecDecodeAndPlaySample(char* sampleData,
                                                  int sampleLength) {
  if (!g_Instance->m_AudioStarted)
    return;

  if (!s_hasFirstFrame) {
    s_firstAppend = std::chrono::steady_clock::now();
    s_hasFirstFrame = true;
  }

  auto now = std::chrono::steady_clock::now();
  TimeStamp ntp = now - s_firstAppend;
  if (s_AudioSyncEnabled && ntp + kAudioBufferMargin < s_estimatedAudioEnd) {
    ClLogMessage("Dropping audio packet to avoid overflow: "
                 "PTS: %.03f NTP: %.03f\n",
                 s_pktPts.count(), ntp.count());
    return;
  }

  DecodeAndAppendPacket(&g_Instance->m_AudioTrack,
                        g_Instance->m_AudioSessionId.load(),
                        g_Instance->m_OpusDecoder,
                        reinterpret_cast<unsigned char*>(sampleData),
                        sampleLength);
  s_estimatedAudioEnd =
      std::max(s_estimatedAudioEnd, ntp) + s_frameDuration;
}

AUDIO_RENDERER_CALLBACKS MoonlightInstance::s_ArCallbacks = {
    .init = MoonlightInstance::AudDecInit,
    .cleanup = MoonlightInstance::AudDecCleanup,
    .decodeAndPlaySample = MoonlightInstance::AudDecDecodeAndPlaySample,
    .capabilities = CAPABILITY_DIRECT_SUBMIT
};
