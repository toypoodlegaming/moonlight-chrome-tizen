#include "moonlight_wasm.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>

#include <h264_stream.h>

#include <pthread.h>

#include "samsung/wasm/elementary_audio_track_config.h"
#include "samsung/wasm/elementary_media_packet.h"
#include "samsung/wasm/elementary_video_track_config.h"
#include "samsung/html/html_media_element_listener.h"
#include "samsung/wasm/operation_result.h"

#define INITIAL_DECODE_BUFFER_LEN 1024 * 1024
#define MAX_SPS_EXTRA_SIZE 16

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;
using EmssReadyState = samsung::wasm::ElementaryMediaStreamSource::ReadyState;
using EmssOperationResult = samsung::wasm::OperationResult;
using EmssAsyncResult = samsung::wasm::OperationResult;
using HTMLAsyncResult = samsung::wasm::OperationResult;
using TimeStamp = samsung::wasm::Seconds;

static constexpr TimeStamp kFrameTimeMargin = 0.5ms;
static constexpr TimeStamp kTimeWindow = 1s;
static constexpr uint32_t kSampleRate = 48000;

static bool s_FramePacingEnabled = false;

static uint32_t s_Width = 0;
static uint32_t s_Height = 0;
static uint32_t s_Framerate = 0;

static std::vector<unsigned char> s_DecodeBuffer;

static TimeStamp s_frameDuration;
static TimeStamp s_pktPts;

static TimeStamp s_ptsDiff;
static TimeStamp s_lastSec;

static std::chrono::time_point<std::chrono::steady_clock> s_firstAppend;
static std::chrono::time_point<std::chrono::steady_clock> s_lastTime;
static bool s_hasFirstFrame = false;

static int s_VideoFormat;
static std::string s_StatString = "";

static uint32_t total_bytes = 0;
static int m_LastFrameNumber = 0;
static VIDEO_STATS m_ActiveWndVideoStats;
static VIDEO_STATS m_LastWndVideoStats;
static VIDEO_STATS m_GlobalVideoStats;

MoonlightInstance::SourceListener::SourceListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::SourceListener::OnSourceClosed() {
  ClLogMessage("EMSS::OnClosed\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_EmssReadyState = EmssReadyState::kClosed;
  m_Instance->m_EmssStateChanged.notify_all();
}

void MoonlightInstance::SourceListener::OnSourceOpenPending() {
  ClLogMessage("EMSS::OnOpenPending\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_EmssReadyState = EmssReadyState::kOpenPending;
  m_Instance->m_EmssStateChanged.notify_all();
}

void MoonlightInstance::SourceListener::OnSourceOpen() {
  ClLogMessage("EMSS::OnOpen\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_EmssReadyState = EmssReadyState::kOpen;
  m_Instance->m_EmssStateChanged.notify_all();
}

MoonlightInstance::AudioTrackListener::AudioTrackListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::AudioTrackListener::OnTrackOpen() {
  ClLogMessage("AUDIO ElementaryMediaTrack::OnTrackOpen\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_AudioStarted = true;
  m_Instance->m_EmssAudioStateChanged.notify_all();
}

void MoonlightInstance::AudioTrackListener::OnTrackClosed(
    samsung::wasm::ElementaryMediaTrack::CloseReason) {
  ClLogMessage("AUDIO ElementaryMediaTrack::OnTrackClosed\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_AudioStarted = false;
}

void MoonlightInstance::AudioTrackListener::OnSessionIdChanged(
    samsung::wasm::SessionId new_session_id) {
  ClLogMessage("AUDIO ElementaryMediaTrack::OnSessionIdChanged\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_AudioSessionId.store(new_session_id);
}

MoonlightInstance::VideoTrackListener::VideoTrackListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::VideoTrackListener::OnTrackOpen() {
  ClLogMessage("VIDEO ElementaryMediaTrack::OnTrackOpen\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_VideoStarted = true;
  m_Instance->m_EmssVideoStateChanged.notify_all();
  LiRequestIdrFrame();
}

void MoonlightInstance::VideoTrackListener::OnTrackClosed(
    samsung::wasm::ElementaryMediaTrack::CloseReason) {
  ClLogMessage("VIDEO ElementaryMediaTrack::OnTrackClosed\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_VideoStarted = false;
}

void MoonlightInstance::VideoTrackListener::OnSessionIdChanged(
    samsung::wasm::SessionId new_session_id) {
  ClLogMessage("VIDEO ElementaryMediaTrack::OnSessionIdChanged\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_VideoSessionId.store(new_session_id);
}

void MoonlightInstance::DidChangeFocus(bool got_focus) {
  // Request an IDR frame to dump the frame queue that may have
  // built up from the GL pipeline being stalled.
  if (got_focus) {
    LiRequestIdrFrame();
  }
}

bool MoonlightInstance::InitializeRenderingSurface(int width, int height) {
  return true;
}

int MoonlightInstance::StartupVidDecSetup(int videoFormat, int width,
int height, int redrawRate, void* context, int drFlags) {
  g_Instance->m_MediaElement.SetSrc(&g_Instance->m_Source);
  ClLogMessage("Waiting for closed\n");
  g_Instance->WaitFor(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kClosed;
  });
  ClLogMessage("closed done\n");

  {
    samsung::wasm::ChannelLayout selectedLayout;
    switch (CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(g_Instance->m_AudioConfig))
    {
    case 6:
      selectedLayout = samsung::wasm::ChannelLayout::k5_1Back; 
      break;
    case 8:
      selectedLayout = samsung::wasm::ChannelLayout::k7_1; 
      break;
    default:
      selectedLayout = samsung::wasm::ChannelLayout::kStereo;
      break;
    }

    auto add_track_result = g_Instance->m_Source.AddTrack(
      samsung::wasm::ElementaryAudioTrackConfig {
        "audio/webm; codecs=\"pcm\"",  // mimeType
        // "audio/mp4; codecs=\"pcm\"",  // mimeType, works
        {},  // extradata (empty?)
        samsung::wasm::DecodingMode::kHardware,
        samsung::wasm::SampleFormat::kS16, //test kS16 is ok, kPlanarS16 does not work at all
        selectedLayout,
        kSampleRate
      });
    if (add_track_result) {
      g_Instance->m_AudioTrack = std::move(*add_track_result);
      g_Instance->m_AudioTrack.SetListener(&g_Instance->m_AudioTrackListener);
    }
  }

  {
    s_VideoFormat = videoFormat;
    const char *mimetype = "video/mp4";
    if(videoFormat & VIDEO_FORMAT_H265_MAIN10) {
      mimetype = "video/mp4; codecs=\"hev1.2.4.L153.B0\"";  // h265 main10 mimeType	: hev1.2.4.L153.B0 can be updated to hev1.2.6.L153.B0 depending on TV capabilities
    } else if(videoFormat & VIDEO_FORMAT_H265) {
      mimetype = "video/mp4; codecs=\"hev1.1.6.L93.B0\"";  // h265 main mimeType
    } else if(videoFormat & VIDEO_FORMAT_H264) {
      mimetype = "video/mp4; codecs=\"avc1.64002A\"";  // h264 High Profile 4.2 mimeType
    } else if (videoFormat & VIDEO_FORMAT_AV1_MAIN8) {
      mimetype = "video/mp4; codecs=\"av01.0.13M.08\"";  // AV1 Main Profile, level 5.1, Main tier, 8 bits
    } else if (videoFormat & VIDEO_FORMAT_AV1_MAIN10) {
      mimetype = "video/mp4; codecs=\"av01.0.13M.10\"";  // AV1 Main Profile, level 5.1, Main tier, 10 bits
    }
    else {
      ClLogMessage("Cannot select mime type for videoFormat=0x%x\n", videoFormat);
      return -1;
    }

    ClLogMessage("Using mimeType %s\n", mimetype);
    auto add_track_result = g_Instance->m_Source.AddTrack(
      samsung::wasm::ElementaryVideoTrackConfig{
        mimetype,
        {},                                   // extradata (empty?)
        samsung::wasm::DecodingMode::kHardware,
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(redrawRate),  // framerateNum
        1,                                  // framerateDen
      });
    if (add_track_result) {
      g_Instance->m_VideoTrack = std::move(*add_track_result);
      g_Instance->m_VideoTrack.SetListener(&g_Instance->m_VideoTrackListener);
    }
  }

  ClLogMessage("Inb4 source open\n");
  g_Instance->m_Source.Open([](EmssOperationResult){});
  g_Instance->WaitFor(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kOpenPending;
  });
  ClLogMessage("Source ready to open\n");
  g_Instance->m_MediaElement.Play([](EmssOperationResult err) {
    if (err != EmssOperationResult::kSuccess) {
      ClLogMessage("Play error\n");
    } else {
      ClLogMessage("Play success\n");
    }
  });

  ClLogMessage("Waiting for start\n");
  g_Instance->WaitFor(&g_Instance->m_EmssAudioStateChanged,
                      [] { return g_Instance->m_AudioStarted.load(); });

  g_Instance->WaitFor(&g_Instance->m_EmssVideoStateChanged,
                      [] { return g_Instance->m_VideoStarted.load(); });
  ClLogMessage("started\n");
  return 0;
}

int MoonlightInstance::VidDecSetup(int videoFormat, int width, int height,
int redrawRate, void* context, int drFlags) {
  ClLogMessage("MoonlightInstance::VidDecSetup\n");
  s_DecodeBuffer.resize(INITIAL_DECODE_BUFFER_LEN);

  s_Width = width;
  s_Height = height;
  s_Framerate = redrawRate;

  s_frameDuration = TimeStamp(1.0 / (float)redrawRate);
  s_pktPts = 0s;
  s_hasFirstFrame = false;
  s_lastSec = 0s;
  s_ptsDiff = 0s;

  s_FramePacingEnabled = g_Instance->m_FramePacingEnabled;

  s_StatString.resize(1000);
  memset(&m_ActiveWndVideoStats, 0, sizeof(m_ActiveWndVideoStats));
  memset(&m_LastWndVideoStats, 0, sizeof(m_LastWndVideoStats));
  memset(&m_GlobalVideoStats, 0, sizeof(m_GlobalVideoStats));

  static std::once_flag once_flag;
  std::call_once(once_flag, &MoonlightInstance::StartupVidDecSetup,
  videoFormat, width, height, redrawRate, context, drFlags);
  return DR_OK;
}

void MoonlightInstance::VidDecCleanup(void) {
  s_DecodeBuffer.clear();
  s_DecodeBuffer.shrink_to_fit();
}

int MoonlightInstance::VidDecSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
  // ClLogMessage("MoonlightInstance::VidDecSubmitDecodeUnit\n");

  if (!g_Instance->m_VideoStarted)
    return DR_OK;

  PLENTRY entry;
  unsigned int offset;
  unsigned int totalLength;
  // ClLogMessage("Video packet append at: %f\n", s_pktPts);

  totalLength = decodeUnit->fullLength;
  if (decodeUnit->frameType == FRAME_TYPE_IDR) {
    // Add some extra space in case we need to do an SPS fixup
    totalLength += MAX_SPS_EXTRA_SIZE;
  }
  // Resize the decode buffer if needed
  if (totalLength > s_DecodeBuffer.size()) {
    s_DecodeBuffer.resize(totalLength + AV_INPUT_BUFFER_PADDING_SIZE);
  }

  entry = decodeUnit->bufferList;
  offset = 0;
  while (entry != NULL) {
    memcpy(&s_DecodeBuffer[offset], entry->data, entry->length);
    offset += entry->length;
    entry = entry->next;
  }

  auto now = std::chrono::steady_clock::now();
  if (!s_hasFirstFrame) {
    s_firstAppend = std::chrono::steady_clock::now();
    s_hasFirstFrame = true;
  } else if (s_FramePacingEnabled) {
    TimeStamp fromStart = now - s_firstAppend;

    while (s_pktPts > fromStart - s_ptsDiff + kFrameTimeMargin) {
      now = std::chrono::steady_clock::now();
      fromStart = now - s_firstAppend;
    }

    if (fromStart > s_lastSec + kTimeWindow) {
      s_lastSec += kTimeWindow;
      s_ptsDiff = fromStart - s_pktPts;
    }
  }
  s_lastTime = now;

  total_bytes += decodeUnit->fullLength;
  if (!m_LastFrameNumber) {
      m_ActiveWndVideoStats.measurementStartTimestamp = LiGetMillis();
      m_LastFrameNumber = decodeUnit->frameNumber;
  } else {
      // Any frame number greater than m_LastFrameNumber + 1 represents a dropped frame
      m_ActiveWndVideoStats.networkDroppedFrames += decodeUnit->frameNumber - (m_LastFrameNumber + 1);
      m_ActiveWndVideoStats.totalFrames += decodeUnit->frameNumber - (m_LastFrameNumber + 1);
      m_LastFrameNumber = decodeUnit->frameNumber;
  }

  // Flip stats windows roughly every second
  if (m_ActiveWndVideoStats.measurementStartTimestamp + 1000 < LiGetMillis()) {
      // Update overlay stats if it's enabled
      if (g_Instance->m_StatsEnabled) { 
          float bitrate_bps = (total_bytes * 8.0);
          float bitrate_mbps = bitrate_bps / 1024.0 / 1024.0;  

          VIDEO_STATS lastTwoWndStats = {};
          lastTwoWndStats.bitrate_mbps = bitrate_mbps; 
          addVideoStats(m_LastWndVideoStats, lastTwoWndStats);
          addVideoStats(m_ActiveWndVideoStats, lastTwoWndStats);
          
          stringifyVideoStats(lastTwoWndStats, s_StatString.data(), s_StatString.length());

          PostToJsAsync(std::string("StatMsg: " + s_StatString));
          std::fill(s_StatString.begin(), s_StatString.end(), ' ');
          total_bytes = 0;
      }

      // Accumulate these values into the global stats
      addVideoStats(m_ActiveWndVideoStats, m_GlobalVideoStats);

      // Move this window into the last window slot and clear it for next window
      memcpy(&m_LastWndVideoStats, &m_ActiveWndVideoStats, sizeof(m_ActiveWndVideoStats));
      memset(&m_ActiveWndVideoStats, 0, sizeof(m_ActiveWndVideoStats));
      m_ActiveWndVideoStats.measurementStartTimestamp = LiGetMillis();
  }

  if (decodeUnit->frameHostProcessingLatency != 0) {
      if (m_ActiveWndVideoStats.minHostProcessingLatency != 0) {
          m_ActiveWndVideoStats.minHostProcessingLatency = MIN(m_ActiveWndVideoStats.minHostProcessingLatency, decodeUnit->frameHostProcessingLatency);
      }
      else {
          m_ActiveWndVideoStats.minHostProcessingLatency = decodeUnit->frameHostProcessingLatency;
      }
      m_ActiveWndVideoStats.framesWithHostProcessingLatency += 1;
  }
  m_ActiveWndVideoStats.maxHostProcessingLatency = MAX(m_ActiveWndVideoStats.maxHostProcessingLatency, decodeUnit->frameHostProcessingLatency);
  m_ActiveWndVideoStats.totalHostProcessingLatency += decodeUnit->frameHostProcessingLatency;

  m_ActiveWndVideoStats.receivedFrames++;
  m_ActiveWndVideoStats.totalFrames++;

  // Start the decoding
  samsung::wasm::ElementaryMediaPacket pkt{
    s_pktPts,
    s_pktPts,
    s_frameDuration,
    decodeUnit->frameType == FRAME_TYPE_IDR,
    offset,
    s_DecodeBuffer.data(),
    0,
    0,
    0,
    0,
    g_Instance->m_VideoSessionId.load()
  };

  m_ActiveWndVideoStats.totalReassemblyTime += decodeUnit->enqueueTimeMs - decodeUnit->receiveTimeMs;
  m_ActiveWndVideoStats.totalDecodeTime += LiGetMillis() - decodeUnit->enqueueTimeMs;
  m_ActiveWndVideoStats.decodedFrames++;

  uint64_t beforeRender = LiGetMillis();
  if (g_Instance->m_VideoTrack.AppendPacket(pkt)) {
    uint64_t afterRender = LiGetMillis();
    s_pktPts += s_frameDuration;
    m_ActiveWndVideoStats.totalRenderTime += afterRender - beforeRender;
    m_ActiveWndVideoStats.renderedFrames++;

    return DR_OK;
  } else {
    ClLogMessage("Append video packet failed\n");
    return DR_NEED_IDR;
  }
}

void MoonlightInstance::addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst) {
    dst.receivedFrames += src.receivedFrames;
    dst.decodedFrames += src.decodedFrames;
    dst.renderedFrames += src.renderedFrames;
    dst.totalFrames += src.totalFrames;
    dst.networkDroppedFrames += src.networkDroppedFrames;
    dst.pacerDroppedFrames += src.pacerDroppedFrames;
    dst.totalReassemblyTime += src.totalReassemblyTime;
    dst.totalDecodeTime += src.totalDecodeTime;
    dst.totalPacerTime += src.totalPacerTime;
    dst.totalRenderTime += src.totalRenderTime;

    if (dst.minHostProcessingLatency == 0) {
        dst.minHostProcessingLatency = src.minHostProcessingLatency;
    }
    else if (src.minHostProcessingLatency != 0) {
        dst.minHostProcessingLatency = MIN(dst.minHostProcessingLatency, src.minHostProcessingLatency);
    }
    dst.maxHostProcessingLatency = MAX(dst.maxHostProcessingLatency, src.maxHostProcessingLatency);
    dst.totalHostProcessingLatency += src.totalHostProcessingLatency;
    dst.framesWithHostProcessingLatency += src.framesWithHostProcessingLatency;

    if (!LiGetEstimatedRttInfo(&dst.lastRtt, &dst.lastRttVariance)) {
        dst.lastRtt = 0;
        dst.lastRttVariance = 0;
    }
    else {
        // Our logic to determine if RTT is valid depends on us never
        // getting an RTT of 0. ENet currently ensures RTTs are >= 1.
        if (dst.lastRtt > 0) {
          //return;
        }
    }

    auto now = LiGetMillis();

    // Initialize the measurement start point if this is the first video stat window
    if (!dst.measurementStartTimestamp) {
        dst.measurementStartTimestamp = src.measurementStartTimestamp;
    }

    // The following code assumes the global measure was already started first
    if (dst.measurementStartTimestamp <= src.measurementStartTimestamp) {
      dst.totalFps = (float)dst.totalFrames / ((float)(now - dst.measurementStartTimestamp) / 1000);
      dst.receivedFps = (float)dst.receivedFrames / ((float)(now - dst.measurementStartTimestamp) / 1000);
      dst.decodedFps = (float)dst.decodedFrames / ((float)(now - dst.measurementStartTimestamp) / 1000);
      dst.renderedFps = (float)dst.renderedFrames / ((float)(now - dst.measurementStartTimestamp) / 1000);
    }
}

void MoonlightInstance::stringifyVideoStats(VIDEO_STATS& stats, char* output, int length)
{
    int offset = 0;
    const char* codecString;
    int ret;

    // Start with an empty string
    output[offset] = 0;

    switch (s_VideoFormat)
    {
    case VIDEO_FORMAT_H264:
        codecString = "H.264";
        break;

    case VIDEO_FORMAT_H265:
        codecString = "HEVC";
        break;

    case VIDEO_FORMAT_H265_MAIN10:
        if (LiGetCurrentHostDisplayHdrMode()) {
            codecString = "HEVC Main 10 HDR";
        }
        else {
            codecString = "HEVC Main 10 SDR";
        }
        break;

    case VIDEO_FORMAT_AV1_MAIN8:
        codecString = "AV1";
        break;

    case VIDEO_FORMAT_AV1_MAIN10:
        if (LiGetCurrentHostDisplayHdrMode()) {
            codecString = "AV1 10-bit HDR";
        }
        else {
            codecString = "AV1 10-bit SDR";
        }
        break;

    default:
        //SDL_assert(false);
        codecString = "UNKNOWN";
        break;
    }

    if (stats.receivedFps > 0) {
        if (codecString != nullptr) {
            ret = snprintf(&output[offset],
                           length - offset,
                           "Video stream: %dx%d %.2f FPS (Codec: %s)\n",
                           s_Width,
                           s_Height,
                           stats.totalFps,
                           codecString);
            if (ret < 0 || ret >= length - offset) {
                //SDL_assert(false);
                return;
            }

            offset += ret;
        }

        ret = snprintf(&output[offset],
                       length - offset,
                       "Incoming frame rate from network: %.2f FPS\n"
                       "Decoding frame rate: %.2f FPS\n"
                       "Rendering frame rate: %.2f FPS\n",
                       stats.receivedFps,
                       stats.decodedFps,
                       stats.renderedFps);
        if (ret < 0 || ret >= length - offset) {
            //SDL_assert(false);
            return;
        }

        offset += ret;
    }

    if (stats.framesWithHostProcessingLatency > 0) {
        ret = snprintf(&output[offset],
                       length - offset,
                       "Host processing latency min/max/average: %.1f/%.1f/%.1f ms\n",
                       (float)stats.minHostProcessingLatency / 10,
                       (float)stats.maxHostProcessingLatency / 10,
                       (float)stats.totalHostProcessingLatency / 10 / stats.framesWithHostProcessingLatency);
        if (ret < 0 || ret >= length - offset) {
            //SDL_assert(false);
            return;
        }

        offset += ret;
    }

    if (stats.renderedFrames != 0) {
        char rttString[32];

        if (stats.lastRtt != 0) {
            snprintf(rttString, sizeof(rttString), "%u ms (variance: %u ms)", stats.lastRtt, stats.lastRttVariance);
        }
        else {
            snprintf(rttString, sizeof(rttString), "N/A");
        }

        ret = snprintf(&output[offset],
                       length - offset,
                       "Frames dropped by your network connection: %.2f%%\n"
                       "Frames dropped due to network jitter: %.2f%%\n"
                       "Average network latency: %s\n"
                       "Average decoding time: %.2f ms\n"
                       "Average frame queue delay: %.2f ms\n"
                       "Average rendering time (including monitor V-sync latency): %.2f ms\n"
                       "Video Bitrate: %.2f mbps\n",
                       (float)stats.networkDroppedFrames / stats.totalFrames * 100,
                       (float)stats.pacerDroppedFrames / stats.decodedFrames * 100,
                       rttString,
                       (float)stats.totalDecodeTime / stats.decodedFrames,
                       (float)stats.totalPacerTime / stats.renderedFrames,
                       (float)stats.totalRenderTime / stats.renderedFrames,
                       stats.bitrate_mbps);
        if (ret < 0 || ret >= length - offset) {
            //SDL_assert(false);
            return;
        }

        offset += ret;
    }
}

void MoonlightInstance::WaitFor(std::condition_variable* variable,
std::function<bool()> condition) {
  std::unique_lock<std::mutex> lock(m_Mutex);
  variable->wait(lock, condition);
}

DECODER_RENDERER_CALLBACKS MoonlightInstance::s_DrCallbacks = {
  .setup = MoonlightInstance::VidDecSetup,
  .cleanup = MoonlightInstance::VidDecCleanup,
  .submitDecodeUnit = MoonlightInstance::VidDecSubmitDecodeUnit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(4)
};
