#include "moonlight_wasm.hpp"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pairing.h>
#include <iostream>

#include <emscripten.h>
#include <emscripten/html5.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// Requests the NaCl module to connection to the server specified after the:
#define MSG_START_REQUEST "startRequest"
// Requests the NaCl module stop streaming
#define MSG_STOP_REQUEST "stopRequest"
// Sent by the NaCl module when the stream has stopped whether user-requested or not
#define MSG_STREAM_TERMINATED "streamTerminated: "

#define MSG_OPENURL "openUrl"

MoonlightInstance* g_Instance;

MoonlightInstance::MoonlightInstance()
    : m_OpusDecoder(NULL),
      m_MouseLocked(false),
      m_MouseLastPosX(-1),
      m_MouseLastPosY(-1),
      m_WaitingForAllModifiersUp(false),
      m_AccumulatedTicks(0),
      m_MouseDeltaX(0),
      m_MouseDeltaY(0),
      m_HttpThreadPoolSequence(0),
      m_Dispatcher("Curl"),
      m_Mutex(),
      m_EmssStateChanged(),
      m_EmssAudioStateChanged(),
      m_EmssVideoStateChanged(),
      m_EmssReadyState(EmssReadyState::kDetached),
      m_AudioStarted(false),
      m_VideoStarted(false),
      m_AudioSessionId(0),
      m_VideoSessionId(0),
      m_MediaElement("nacl_module"),
      m_Source(
        samsung::wasm::ElementaryMediaStreamSource::LatencyMode::kLow, //TODO add option in menu
        samsung::wasm::ElementaryMediaStreamSource::RenderingMode::kMediaElement),
      m_SourceListener(this),
      m_AudioTrackListener(this),
      m_VideoTrackListener(this),
      m_VideoTrack(),
      m_AudioTrack() {
  m_Dispatcher.start();
  m_Source.SetListener(&m_SourceListener);
}

MoonlightInstance::~MoonlightInstance() { m_Dispatcher.stop(); }

void MoonlightInstance::OnConnectionStarted(uint32_t unused) {
  // Tell the front end
  PostToJs(std::string("Connection Established"));
}

void MoonlightInstance::OnConnectionStopped(uint32_t error) {
  // Not running anymore
  m_Running = false;

  // Unlock the mouse
  UnlockMouse();

  // Notify the JS code that the stream has ended
  PostToJs(std::string(MSG_STREAM_TERMINATED + std::to_string((int)error)));
}

void MoonlightInstance::StopConnection() {
  pthread_t t;

  // Stopping needs to happen in a separate thread to avoid a potential deadlock
  // caused by us getting a callback to the main thread while inside
  // LiStopConnection.
  pthread_create(&t, NULL, MoonlightInstance::StopThreadFunc, NULL);

  // We'll need to call the listener ourselves since our connection terminated
  // callback won't be invoked for a manually requested termination.
  OnConnectionStopped(0);
}

void* MoonlightInstance::StopThreadFunc(void* context) {
  // We must join the connection thread first, because LiStopConnection must
  // not be invoked during LiStartConnection.
  pthread_join(g_Instance->m_ConnectionThread, NULL);

  // Force raise all modifier keys to avoid leaving them down after
  // disconnecting
  LiSendKeyboardEvent(0xA0, KEY_ACTION_UP, 0);
  LiSendKeyboardEvent(0xA1, KEY_ACTION_UP, 0);
  LiSendKeyboardEvent(0xA2, KEY_ACTION_UP, 0);
  LiSendKeyboardEvent(0xA3, KEY_ACTION_UP, 0);
  LiSendKeyboardEvent(0xA4, KEY_ACTION_UP, 0);
  LiSendKeyboardEvent(0xA5, KEY_ACTION_UP, 0);

  // Not running anymore
  g_Instance->m_Running = false;

  // We also need to stop this thread after the connection thread, because it
  // depends on being initialized there.
  pthread_join(g_Instance->m_InputThread, NULL);

  // Stop the connection
  LiStopConnection();
  return NULL;
}

void* MoonlightInstance::InputThreadFunc(void* context) {
  MoonlightInstance* me = (MoonlightInstance*)context;

  while (me->m_Running) {
    me->PollGamepads();
    me->ReportMouseMovement();

    // Poll every 5 ms
    usleep(5 * 1000);
  }

  return NULL;
}

void* MoonlightInstance::ConnectionThreadFunc(void* context) {
  MoonlightInstance* me = (MoonlightInstance*)context;
  int err;
  SERVER_INFORMATION serverInfo;

  // Post a status update before we begin
  PostToJs(std::string("Starting connection to ") + me->m_Host);

  LiInitializeServerInformation(&serverInfo);
  serverInfo.address = me->m_Host.c_str();
  serverInfo.serverInfoAppVersion = me->m_AppVersion.c_str();
  serverInfo.serverInfoGfeVersion = me->m_GfeVersion.c_str();
  serverInfo.rtspSessionUrl = me->m_RtspUrl.c_str();
  serverInfo.serverCodecModeSupport = me->m_ServerCodecModeSupport; 

  err = LiStartConnection(&serverInfo, &me->m_StreamConfig,
  &MoonlightInstance::s_ClCallbacks, &MoonlightInstance::s_DrCallbacks,
  &MoonlightInstance::s_ArCallbacks, NULL, 0, NULL, 0);
  if (err != 0) {
    // Notify the JS code that the stream has ended
    // NB: We pass error code 0 here to avoid triggering a "Connection
    // terminated" warning message.
    PostToJs(MSG_STREAM_TERMINATED + std::to_string(0));
    return NULL;
  }

  // Set running state before starting connection-specific threads
  me->m_Running = true;

  pthread_create(&me->m_InputThread, NULL, MoonlightInstance::InputThreadFunc, me);

  return NULL;
}

static void HexStringToBytes(const char* str, char* output) {
  for (size_t i = 0; i < strlen(str); i += 2) {
    sscanf(&str[i], "%2hhx", &output[i / 2]);
  }
}

MessageResult MoonlightInstance::StartStream(
std::string host, std::string width, std::string height, std::string fps,
std::string bitrate, std::string rikey, std::string rikeyid,
std::string appversion, std::string gfeversion, std::string rtspurl, bool framePacing,
bool audioSync, bool hdrEnabled, std::string codecVideo, std::string serverCodecSupportMode, std::string audioConfig, bool statsEnabled) {
  PostToJs("Setting stream width to: " + width);
  PostToJs("Setting stream height to: " + height);
  PostToJs("Setting stream fps to: " + fps);
  PostToJs("Setting stream host to: " + host);
  PostToJs("Setting stream bitrate to: " + bitrate);
  PostToJs("Setting rikey to: " + rikey);
  PostToJs("Setting rikeyid to: " + rikeyid);
  PostToJs("Setting appversion to: " + appversion);
  PostToJs("Setting gfeversion to: " + gfeversion);
  PostToJs("Setting RTSP url to: " + rtspurl);
  PostToJs("Setting frame pacing to: " + std::to_string(framePacing));
  PostToJs("Setting audio syncing to: " + std::to_string(audioSync));
  PostToJs("Setting HDR to:" + std::to_string(hdrEnabled));
  PostToJs("Setting videoCodec: " + codecVideo);
  PostToJs("Setting serverCodecSupportMode: " + serverCodecSupportMode);
  PostToJs("Setting audioConfig: " + audioConfig);
  PostToJs("Setting stats to: " + std::to_string(statsEnabled));

  // Populate the stream configuration
  LiInitializeStreamConfiguration(&m_StreamConfig);
  m_StreamConfig.width = stoi(width);
  m_StreamConfig.height = stoi(height);
  m_StreamConfig.fps = stoi(fps);
  m_StreamConfig.bitrate = stoi(bitrate);  // kilobits per second
  
  if (audioConfig == "51") {
    m_StreamConfig.audioConfiguration = MAKE_AUDIO_CONFIGURATION(6, 0x60F); //011000001111
  } else if (audioConfig == "71") {
    m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND; //011000111111
  } else {
    m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  }

  m_StreamConfig.streamingRemotely = STREAM_CFG_AUTO;
  m_StreamConfig.packetSize = 1392;
  m_StreamConfig.encryptionFlags = ENCFLG_NONE;

  // H.264 is always supported
  int derivedVideoFormats = VIDEO_FORMAT_H264;
  // sad switch options from index.html
  switch (stoi(codecVideo)) {
    case 264:
        break;
    case 265:
        derivedVideoFormats |= VIDEO_FORMAT_H265;
        if (hdrEnabled) {
            derivedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
        }
        break;
    case 1:
        derivedVideoFormats |= VIDEO_FORMAT_AV1_MAIN8;
        derivedVideoFormats |= SCM_AV1_MAIN8;

        if (hdrEnabled) {
            derivedVideoFormats |= VIDEO_FORMAT_AV1_MAIN10;
            derivedVideoFormats |= SCM_AV1_MAIN10;
        }

        // We'll try to fall back to HEVC first if AV1 fails. We'd rather not fall back
        // straight to H.264 if the user asked for AV1 and the host doesn't support it.
        if (derivedVideoFormats & VIDEO_FORMAT_AV1_MAIN8) {
            derivedVideoFormats |= VIDEO_FORMAT_H265;
        }
        if (derivedVideoFormats & VIDEO_FORMAT_AV1_MAIN10) {
            derivedVideoFormats |= VIDEO_FORMAT_H265_MAIN10;
        }

        break;
  }
  m_StreamConfig.supportedVideoFormats = derivedVideoFormats;

  // Load the rikey and rikeyid into the stream configuration
  HexStringToBytes(rikey.c_str(), m_StreamConfig.remoteInputAesKey);
  int rikeyiv = htonl(stoi(rikeyid));
  memcpy(m_StreamConfig.remoteInputAesIv, &rikeyiv, sizeof(rikeyiv));

  // Store the parameters from the start message
  m_Host = host;
  m_AppVersion = appversion;
  m_GfeVersion = gfeversion;
  m_RtspUrl = rtspurl;
  m_FramePacingEnabled = framePacing;
  m_AudioSyncEnabled = audioSync;
  m_HdrEnabled = hdrEnabled;

  m_ServerCodecModeSupport = derivedVideoFormats; // FIXME value should come from the server
  m_AudioConfig = m_StreamConfig.audioConfiguration;
  m_StatsEnabled = statsEnabled;



  // Initialize the rendering surface before starting the connection
  if (InitializeRenderingSurface(m_StreamConfig.width, m_StreamConfig.height)) {
    // Start the worker thread to establish the connection
    pthread_create(&m_ConnectionThread, NULL, MoonlightInstance::ConnectionThreadFunc, this);
  } else {
    // Failed to initialize renderer
    OnConnectionStopped(0);
  }

  return MessageResult::Resolve();
}

MessageResult MoonlightInstance::StopStream() {
  // Begin connection teardown
  StopConnection();

  return MessageResult::Resolve();
}

void MoonlightInstance::ToggleStats() {
  m_StatsEnabled = !m_StatsEnabled;
  PostToJsAsync(std::string("StatMsg:  "));
}

void MoonlightInstance::STUN_private(int callbackId) {
  unsigned int wanAddr;
  char addrStr[128] = {};

  if (LiFindExternalAddressIP4("stun.moonlight-stream.org", 3478, &wanAddr) ==
      0) {
    inet_ntop(AF_INET, &wanAddr, addrStr, sizeof(addrStr));
    PostPromiseMessage(callbackId, "resolve", std::string(addrStr, strlen(addrStr)));
  } else {
    PostPromiseMessage(callbackId, "resolve", "");
  }
}

void MoonlightInstance::STUN(int callbackId) {
  m_Dispatcher.post_job(
      std::bind(&MoonlightInstance::STUN_private, this, callbackId), false);
}

void MoonlightInstance::Pair_private(int callbackId, std::string serverMajorVersion,
std::string address, std::string randomNumber) {
  char* ppkstr;
  int err = gs_pair(atoi(serverMajorVersion.c_str()), address.c_str(),
                    randomNumber.c_str(), &ppkstr);

  printf("pair address: %s result: %d\n", address.c_str(), err);
  if (err == 0) {
    free(ppkstr);
    PostPromiseMessage(callbackId, "resolve", ppkstr);
  } else {
    PostPromiseMessage(callbackId, "reject", std::to_string(err));
  }
}

void MoonlightInstance::Pair(int callbackId, std::string serverMajorVersion,
std::string address, std::string randomNumber) {
  printf("%s address: %s\n", __func__, address.c_str());
  m_Dispatcher.post_job(
      std::bind(&MoonlightInstance::Pair_private, this, callbackId,
                serverMajorVersion, address, randomNumber),
      false);
}

bool MoonlightInstance::Init(uint32_t argc, const char* argn[], const char* argv[]) {
  g_Instance = this;
  return true;
}

int main(int argc, char** argv) {
  g_Instance = new MoonlightInstance();

  emscripten_set_keyup_callback(kCanvasName, NULL, EM_TRUE, handleKeyUp);
  emscripten_set_keydown_callback(kCanvasName, NULL, EM_TRUE, handleKeyDown);
  emscripten_set_mousedown_callback(kCanvasName, NULL, EM_TRUE,
                                    handleMouseDown);
  emscripten_set_mouseup_callback(kCanvasName, NULL, EM_TRUE, handleMouseUp);
  emscripten_set_mousemove_callback(kCanvasName, NULL, EM_TRUE,
                                    handleMouseMove);
  emscripten_set_wheel_callback(kCanvasName, NULL, EM_TRUE, handleWheel);

  // As we want to setup callbacks on DOM document and
  // emscripten_set_pointerlock... calls use document.querySelector
  // method, passing first argument to it, I've workaround for it
  // When passing address 0x1, js glue code replace it with document object
  static const char* kDocument = reinterpret_cast<const char*>(0x1);
  emscripten_set_pointerlockchange_callback(kDocument, NULL, EM_TRUE, handlePointerLockChange);
  emscripten_set_pointerlockerror_callback(kDocument, NULL, EM_TRUE, handlePointerLockError);
  EM_ASM(Module['noExitRuntime'] = true);
  unsigned char buffer[128];
  int rc = RAND_bytes(buffer, sizeof(buffer));

  if (rc != 1) {
    std::cout << "RAND_bytes failed\n";
  }
  RAND_seed(buffer, 128);
}
MessageResult startStream(std::string host, std::string width,
std::string height, std::string fps, std::string bitrate, std::string rikey,
std::string rikeyid, std::string appversion, std::string gfeversion, std::string rtspurl, bool framePacing,
bool audioSync, bool hdrEnabled, std::string codecVideo, std::string serverCodecSupportMode, std::string audioConfig, bool statsEnabled) {
  printf("%s host: %s w: %s h: %s\n", __func__, host.c_str(), width.c_str(), height.c_str());
  return g_Instance->StartStream(host, width, height, fps, bitrate, rikey,
  rikeyid, appversion, gfeversion, rtspurl, framePacing, audioSync, hdrEnabled, codecVideo, serverCodecSupportMode, audioConfig, statsEnabled);
}

MessageResult stopStream() { return g_Instance->StopStream(); }
void toggleStats() {
  g_Instance->ToggleStats();
}
void stun(int callbackId) { g_Instance->STUN(callbackId); }

void pair(int callbackId, std::string serverMajorVersion, std::string address, std::string randomNumber) {
  g_Instance->Pair(callbackId, serverMajorVersion, address, randomNumber);
}

void PostToJs(std::string msg) {
  MAIN_THREAD_EM_ASM(
      {
        const msg = UTF8ToString($0);
        handleMessage(msg);
      },
      msg.c_str());
}

void PostToJsAsync(std::string msg) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const msg = UTF8ToString($0);
        handleMessage(msg);
      },
      msg.c_str());
}

void PostPromiseMessage(int callbackId, const std::string& type, const std::string& response) {
  MAIN_THREAD_EM_ASM(
      {
        const type = UTF8ToString($1);
        const response = UTF8ToString($2);

        handlePromiseMessage($0, type, response);
      },
      callbackId, type.c_str(), response.c_str());
}
void PostPromiseMessage(int callbackId, const std::string& type, const std::vector<uint8_t>& response) {
  MAIN_THREAD_EM_ASM(
      {
        const type = UTF8ToString($1);
        const response = HEAPU8.slice($2, $2 + $3);

        handlePromiseMessage($0, type, response);
      },
      callbackId, type.c_str(), response.data(), response.size());
}

void wakeOnLan(int callbackId, std::string macAddress) {
    unsigned char magicPacket[102];
    unsigned char mac[6];

    if (sscanf(macAddress.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        printf("Invalid MAC address format\n");
        return;
    }

    for (int i = 0; i < 6; i++) {
        magicPacket[i] = 0xFF;
    }
    for (int i = 1; i <= 16; i++) {
        memcpy(&magicPacket[i * 6], &mac, 6 * sizeof(unsigned char));
    }

    int udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == -1) {
        printf("Failed to create socket\n");
        return;
    }

    int broadcast = 1;
    if (setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1) {
        printf("Failed to enable broadcast\n");
        close(udpSocket);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_BROADCAST;
    addr.sin_port = htons(9); // Wake-on-LAN typically uses port 9

    if (sendto(udpSocket, magicPacket, sizeof(magicPacket), 0, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        printf("Failed to send magic packet\n");
    }

    close(udpSocket);
}

EMSCRIPTEN_BINDINGS(handle_message) {
  emscripten::value_object<MessageResult>("MessageResult")
    .field("type", &MessageResult::type)
    .field("ret", &MessageResult::ret);

  emscripten::function("startStream", &startStream);
  emscripten::function("stopStream", &stopStream);
  emscripten::function("toggleStats", &toggleStats);
  emscripten::function("stun", &stun);
  emscripten::function("pair", &pair);
  emscripten::function("wakeOnLan", &wakeOnLan);
}
