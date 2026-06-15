#include <M5Unified.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_audio_enc.h"
#include "esp_camera.h"
#include "esp_opus_enc.h"
#include "esp_audio_types.h"
#include "driver/uart.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

void run_xiaozhi_ota_probe();
void run_stream_tts_demo();
void run_wifi_connect_app();
void run_camera_upload_app();
void run_tracking_user_demo();
static bool wifi_is_connected();
static void show_expression(const char* expression);
static bool execute_speak_command(const char* text);
static bool execute_speak_command_internal(const char* text, bool pause_voice_listener, const char* cache_name = nullptr);
static void request_speak_preempt(const char* reason);

extern const uint8_t calm_face_png_start[] asm("_binary_calm_face_png_start");
extern const uint8_t calm_face_png_end[] asm("_binary_calm_face_png_end");
extern const uint8_t speak1_face_png_start[] asm("_binary_speak1_face_png_start");
extern const uint8_t speak1_face_png_end[] asm("_binary_speak1_face_png_end");
extern const uint8_t speak2_face_png_start[] asm("_binary_speak2_face_png_start");
extern const uint8_t speak2_face_png_end[] asm("_binary_speak2_face_png_end");
extern const uint8_t shy_face_png_start[] asm("_binary_shy_face_png_start");
extern const uint8_t shy_face_png_end[] asm("_binary_shy_face_png_end");
extern const uint8_t thinking_face_png_start[] asm("_binary_thinking_face_png_start");
extern const uint8_t thinking_face_png_end[] asm("_binary_thinking_face_png_end");
extern const uint8_t blink_half_face_png_start[] asm("_binary_blink_half_face_png_start");
extern const uint8_t blink_half_face_png_end[] asm("_binary_blink_half_face_png_end");
extern const uint8_t blink_closed_face_png_start[] asm("_binary_blink_closed_face_png_start");
extern const uint8_t blink_closed_face_png_end[] asm("_binary_blink_closed_face_png_end");
extern const uint8_t wink_half_face_png_start[] asm("_binary_wink_half_face_png_start");
extern const uint8_t wink_half_face_png_end[] asm("_binary_wink_half_face_png_end");
extern const uint8_t wink_closed_face_png_start[] asm("_binary_wink_closed_face_png_start");
extern const uint8_t wink_closed_face_png_end[] asm("_binary_wink_closed_face_png_end");
extern const uint8_t heart_small_face_png_start[] asm("_binary_heart_small_face_png_start");
extern const uint8_t heart_small_face_png_end[] asm("_binary_heart_small_face_png_end");
extern const uint8_t heart_face_png_start[] asm("_binary_heart_face_png_start");
extern const uint8_t heart_face_png_end[] asm("_binary_heart_face_png_end");
extern const uint8_t nod_soft_face_png_start[] asm("_binary_nod_soft_face_png_start");
extern const uint8_t nod_soft_face_png_end[] asm("_binary_nod_soft_face_png_end");
extern const uint8_t nod_down_face_png_start[] asm("_binary_nod_down_face_png_start");
extern const uint8_t nod_down_face_png_end[] asm("_binary_nod_down_face_png_end");
extern const uint8_t happy_squint_face_png_start[] asm("_binary_happy_squint_face_png_start");
extern const uint8_t happy_squint_face_png_end[] asm("_binary_happy_squint_face_png_end");
extern const uint8_t happy_squint_soft_face_png_start[] asm("_binary_happy_squint_soft_face_png_start");
extern const uint8_t happy_squint_soft_face_png_end[] asm("_binary_happy_squint_soft_face_png_end");

namespace {

enum class AppId {
    Launcher,
    WifiConnect,
    VoiceDemo,
    StreamTtsDemo,
    CameraUpload,
    TrackingUser,
};

static constexpr const char* TAG = "Xiaopai";
static constexpr int kWifiConnectedBit = BIT0;
static constexpr int kWifiFailedBit = BIT1;
static constexpr int kHttpBufferSize = 4096;
static constexpr int kAudioUploadTimeoutMs = 60000;
static constexpr int kAudioSampleRate = 16000;
static constexpr int kOpusFrameDurationMs = 60;
static constexpr int kOpusFrameSamples = kAudioSampleRate * kOpusFrameDurationMs / 1000;
static constexpr int kRecordSampleRate = CONFIG_STACKCHAN_RECORD_SAMPLE_RATE;
static constexpr int kRecordChunkMs = CONFIG_STACKCHAN_RECORD_CHUNK_MS;
static constexpr int kVoiceProbeSamples = kRecordSampleRate * kRecordChunkMs / 1000;
static constexpr int kPreRollMs = CONFIG_STACKCHAN_PREROLL_MS;
static constexpr int kPreRollSamples = kRecordSampleRate * kPreRollMs / 1000;
static constexpr int kVoiceStartThreshold = CONFIG_STACKCHAN_VOICE_START_THRESHOLD;
static constexpr int kVoiceStopThreshold = CONFIG_STACKCHAN_VOICE_STOP_THRESHOLD;
static constexpr int kRecordMaxMs = CONFIG_STACKCHAN_RECORD_MAX_MS;
static constexpr int kSilenceStopMs = CONFIG_STACKCHAN_SILENCE_STOP_MS;
static constexpr int kVoiceListenerReleaseWaitMs = 1000;
static constexpr int kTtsStreamSampleRate = 16000;
static constexpr size_t kTtsStreamBufferSamples = 2048;
static constexpr int kLauncherAppCount = 5;
static constexpr bool kShowAppStatusScreens = false;
static constexpr uint32_t kWifiTaskStackBytes = 8 * 1024;
static constexpr uint32_t kApp1TaskStackBytes = 24 * 1024;
static constexpr uint32_t kApp2TaskStackBytes = 16 * 1024;
static constexpr uint32_t kCommandTaskStackBytes = 16 * 1024;
static constexpr uint32_t kCameraTaskStackBytes = 16 * 1024;
static constexpr uint32_t kTrackingTaskStackBytes = 20 * 1024;
static constexpr size_t kTtsMaxBytes = CONFIG_STACKCHAN_TTS_MAX_BYTES;
static constexpr int kCameraWidth = 320;
static constexpr int kCameraHeight = 240;
static constexpr int kCameraFreshDiscardFrames = 2;
static constexpr float kTrackingCx = 160.0f;
static constexpr float kTrackingCy = 120.0f;
static constexpr float kTrackingFx = 364.0f;
static constexpr float kTrackingFy = 364.0f;
static constexpr float kTrackingYawGain = 0.75f;
static constexpr float kTrackingPitchGain = 0.90f;
static constexpr float kTrackingYawDirection = 1.0f;
static constexpr float kTrackingPitchDirection = -1.0f;
static constexpr float kFindOwnerYawGain = 0.45f;
static constexpr float kFindOwnerPitchGain = 0.55f;
static constexpr float kFindOwnerYawDirection = 1.0f;
static constexpr float kFindOwnerPitchDirection = -1.0f;
static constexpr float kFindOwnerStopPixels = 32.0f;
static constexpr int kTrackingScanStepCount = 5;
static constexpr int kTrackingRefineRounds = 3;
static constexpr float kTrackingStopPixels = 16.0f;
static constexpr float kTrackingYawMinDeg = -180.0f;
static constexpr float kTrackingYawMaxDeg = 180.0f;
static constexpr float kTrackingPitchMinDeg = 0.0f;
static constexpr float kTrackingPitchMaxDeg = 90.0f;
static constexpr float kTrackingHomePitchDeg = 45.0f;
static constexpr float kTrackingScanYawDeg = 25.0f;
static constexpr float kTrackingScanPitchDeltaDeg = 20.0f;
static constexpr int kFindOwnerMaxRounds = 3;
static constexpr float kPi = 3.14159265358979323846f;
static constexpr uart_port_t kServoUart = UART_NUM_1;
static constexpr int kServoTxPin = 6;
static constexpr int kServoRxPin = 7;
static constexpr int kServoBaud = 1000000;
static constexpr int kServoPanId = 1;
static constexpr int kServoTiltId = 2;
static constexpr int kServoYawZeroRaw = 460;
static constexpr int kServoPitchZeroRaw = 620;
static constexpr float kServoStepsPerDegree = 3.2f;
static constexpr uint8_t kPy32Address = 0x6f;
static constexpr uint8_t kPy32ServoPowerPin = 0;
static constexpr uint32_t kPy32I2cFreq = 100000;
static constexpr uint8_t kSi12tAddress = 0x68;
static constexpr uint8_t kSi12tSensitivity1Reg = 0x02;
static constexpr uint8_t kSi12tCtrl1Reg = 0x08;
static constexpr uint8_t kSi12tCtrl2Reg = 0x09;
static constexpr uint8_t kSi12tRefRst1Reg = 0x0a;
static constexpr uint8_t kSi12tRefRst2Reg = 0x0b;
static constexpr uint8_t kSi12tChHold1Reg = 0x0c;
static constexpr uint8_t kSi12tChHold2Reg = 0x0d;
static constexpr uint8_t kSi12tCalHold1Reg = 0x0e;
static constexpr uint8_t kSi12tCalHold2Reg = 0x0f;
static constexpr uint8_t kSi12tOutput1Reg = 0x10;
static constexpr uint32_t kSi12tI2cFreq = 100000;
static constexpr uint32_t kHeadTouchPollMs = 50;
static constexpr uint32_t kHeadTouchClickMaxMs = 650;
static constexpr int16_t kHeadTouchSwipeThreshold = 40;
static constexpr uint32_t kHeadTouchTaskStackBytes = 6 * 1024;
static constexpr uint32_t kHeadTouchAudioTaskStackBytes = 16 * 1024;
static constexpr uint32_t kSpeakCommandTaskStackBytes = 16 * 1024;
static constexpr size_t kSpeakCommandMaxTextBytes = 512;
static constexpr size_t kSpeakCommandCacheNameBytes = 64;
static constexpr uint32_t kSpeakerDrainMs = 500;
static constexpr int kSpeakerVolumePercentMin = 10;
static constexpr int kSpeakerVolumePercentMax = 100;
static constexpr int kSpeakerVolumeDefaultStep = 10;

static void configure_speaker_for_tts()
{
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = kTtsStreamSampleRate;
    spk_cfg.dma_buf_len = 512;
    spk_cfg.dma_buf_count = 12;
    spk_cfg.task_priority = 4;
    spk_cfg.task_pinned_core = 0;
    M5.Speaker.config(spk_cfg);
}

AppId current_app = AppId::Launcher;
int selected_menu = 0;
TaskHandle_t wifi_task_handle = nullptr;
TaskHandle_t xiaozhi_task_handle = nullptr;
TaskHandle_t stream_tts_task_handle = nullptr;
TaskHandle_t camera_upload_task_handle = nullptr;
TaskHandle_t tracking_task_handle = nullptr;
TaskHandle_t command_task_handle = nullptr;
TaskHandle_t boot_task_handle = nullptr;
TaskHandle_t speak_animation_task_handle = nullptr;
TaskHandle_t expression_animation_task_handle = nullptr;
TaskHandle_t head_touch_task_handle = nullptr;
TaskHandle_t head_touch_audio_task_handle = nullptr;
TaskHandle_t speak_command_task_handle = nullptr;
EventGroupHandle_t wifi_event_group = nullptr;
SemaphoreHandle_t m5_mutex = nullptr;
SemaphoreHandle_t audio_mutex = nullptr;
QueueHandle_t head_touch_event_queue = nullptr;
QueueHandle_t speak_command_queue = nullptr;
int wifi_retry_count = 0;
bool wifi_started = false;
bool wifi_connect_requested = false;
volatile bool wifi_manual_switching = false;
volatile bool app1_stop_requested = false;
volatile bool app2_stop_requested = false;
volatile bool tracking_stop_requested = false;
volatile bool voice_listener_paused = false;
volatile bool voice_status_screen_suppressed = false;
volatile bool speech_playback_active = false;
volatile bool speech_expression_overridden = false;
volatile bool speak_animation_running = false;
volatile bool expression_animation_running = false;
volatile bool speak_command_active = false;
bool camera_initialized = false;
volatile bool camera_owns_internal_i2c = false;
bool servo_uart_initialized = false;
bool servo_power_ready = false;
float tracking_yaw_deg = 0.0f;
float tracking_pitch_deg = kTrackingHomePitchDeg;
int speaker_volume_percent =
    std::max(kSpeakerVolumePercentMin, std::min(kSpeakerVolumePercentMax, (CONFIG_STACKCHAN_TTS_VOLUME * 100 + 127) / 255));
char client_id[37] = {};
std::string active_wifi_ssid = CONFIG_STACKCHAN_WIFI_SSID;
std::string active_server_base = "http://192.168.21.15:8091";
bool active_server_selected = false;
int active_wifi_candidate_index = -1;
static constexpr int kWifiRetryLimit = 1;
static constexpr const char* kWifiNvsNamespace = "xiaopai";
static constexpr const char* kWifiNvsSsidKey = "wifi_ssid";
static constexpr const char* kWifiNvsPasswordKey = "wifi_password";
static constexpr const char* kServerNvsBaseKey = "server_base";
static constexpr const char* kProvisioningApPassword = "12345678";
static constexpr int kProvisioningMaxScanResults = 16;
bool wifi_sta_netif_created = false;
bool wifi_ap_netif_created = false;
bool provisioning_started = false;
httpd_handle_t provisioning_httpd = nullptr;

enum class HeadTouchEvent : uint8_t {
    Press,
    Click,
    SwipeForward,
    SwipeBackward,
};

struct SpeakCommandItem {
    char cmd_id[40];
    char text[kSpeakCommandMaxTextBytes];
    char cache_name[kSpeakCommandCacheNameBytes];
    bool pause_voice_listener;
};

struct WifiCandidate {
    const char* ssid;
    const char* password;
};

static constexpr WifiCandidate kWifiCandidates[] = {
    {CONFIG_STACKCHAN_WIFI_SSID, CONFIG_STACKCHAN_WIFI_PASSWORD},
    {"MYENDLESS", "88888888"},
};
static constexpr int kWifiCandidateCount = sizeof(kWifiCandidates) / sizeof(kWifiCandidates[0]);

static constexpr const char* kServerBaseCandidates[] = {
    "http://1.14.134.217:8091/",
    "http://192.168.21.15:8091",
    "http://172.24.77.83:8091",
    "http://192.168.137.1:8091",
};

struct ExpressionAsset {
    const char* name;
    const uint8_t* start;
    const uint8_t* end;
    int width;
    int height;
};

struct ExpressionAnimation {
    const char* name;
    const char* const* frames;
    size_t frame_count;
    int frame_ms;
};

static constexpr const char* kDefaultExpression = "calm";
static const ExpressionAsset kExpressionAssets[] = {
    {"calm", calm_face_png_start, calm_face_png_end, 320, 240},
    {"speak1", speak1_face_png_start, speak1_face_png_end, 320, 240},
    {"speak2", speak2_face_png_start, speak2_face_png_end, 320, 240},
    {"shy", shy_face_png_start, shy_face_png_end, 320, 240},
    {"thinking", thinking_face_png_start, thinking_face_png_end, 320, 240},
    {"blink_half", blink_half_face_png_start, blink_half_face_png_end, 320, 240},
    {"blink_closed", blink_closed_face_png_start, blink_closed_face_png_end, 320, 240},
    {"wink_half", wink_half_face_png_start, wink_half_face_png_end, 320, 240},
    {"wink_closed", wink_closed_face_png_start, wink_closed_face_png_end, 320, 240},
    {"heart_small", heart_small_face_png_start, heart_small_face_png_end, 320, 240},
    {"heart", heart_face_png_start, heart_face_png_end, 320, 240},
    {"nod_soft", nod_soft_face_png_start, nod_soft_face_png_end, 320, 240},
    {"nod_down", nod_down_face_png_start, nod_down_face_png_end, 320, 240},
    {"happy_squint", happy_squint_face_png_start, happy_squint_face_png_end, 320, 240},
    {"happy_squint_soft", happy_squint_soft_face_png_start, happy_squint_soft_face_png_end, 320, 240},
};
static constexpr const char* kBlinkFrames[] = {
    "calm",
    "blink_half",
    "blink_closed",
    "blink_half",
    "calm",
    "calm",
    "calm",
};
static constexpr const char* kWinkFrames[] = {
    "calm",
    "wink_half",
    "wink_closed",
    "wink_half",
    "calm",
    "wink_closed",
    "calm",
};
static constexpr const char* kHeartFrames[] = {
    "calm",
    "heart_small",
    "heart",
    "heart",
    "heart_small",
    "heart",
};
static constexpr const char* kNodFrames[] = {
    "calm",
    "nod_soft",
    "nod_down",
    "nod_soft",
    "calm",
};
static constexpr const char* kSpeakFrames[] = {
    "speak1",
    "speak2",
};
static constexpr const char* kHappyDynamicFrames[] = {
    "happy_squint_soft",
    "happy_squint",
    "happy_squint_soft",
    "happy_squint",
};
static constexpr ExpressionAnimation kExpressionAnimations[] = {
    {"blink", kBlinkFrames, sizeof(kBlinkFrames) / sizeof(kBlinkFrames[0]), 140},
    {"wink", kWinkFrames, sizeof(kWinkFrames) / sizeof(kWinkFrames[0]), 140},
    {"heart_action", kHeartFrames, sizeof(kHeartFrames) / sizeof(kHeartFrames[0]), 170},
    {"hearting", kHeartFrames, sizeof(kHeartFrames) / sizeof(kHeartFrames[0]), 170},
    {"nod", kNodFrames, sizeof(kNodFrames) / sizeof(kNodFrames[0]), 180},
    {"nodding", kNodFrames, sizeof(kNodFrames) / sizeof(kNodFrames[0]), 180},
    {"speak", kSpeakFrames, sizeof(kSpeakFrames) / sizeof(kSpeakFrames[0]), 160},
    {"speaking", kSpeakFrames, sizeof(kSpeakFrames) / sizeof(kSpeakFrames[0]), 160},
    {"happy_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
    {"happy_squint_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
};
bool expression_screen_visible = false;
const ExpressionAsset* current_expression_asset = nullptr;
const ExpressionAnimation* current_expression_animation = nullptr;

struct XiaozhiConfig {
    std::string websocket_url;
    std::string websocket_token;
    std::string session_id;
};

XiaozhiConfig xiaozhi_config;

struct App1Status {
    char stage[32] = "Idle";
    char line1[96] = "Ready";
    char line2[96] = "";
    char line3[96] = "";
    bool running = false;
    bool success = false;
};

App1Status app1_status;
App1Status wifi_status;
App1Status tts_status;
App1Status camera_status;
App1Status tracking_status;

class M5Lock {
public:
    M5Lock()
    {
        if (m5_mutex != nullptr) {
            xSemaphoreTake(m5_mutex, portMAX_DELAY);
            locked_ = true;
        }
    }
    ~M5Lock()
    {
        if (locked_) {
            xSemaphoreGive(m5_mutex);
        }
    }

private:
    bool locked_ = false;
};

void mark_expression_screen_dirty()
{
    expression_screen_visible = false;
    current_expression_asset = nullptr;
}

void draw_header(const char* title)
{
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.drawString(title, 12, 10);
    display.drawFastHLine(0, 42, display.width(), TFT_DARKGREY);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("BtnA / top-left: back", 12, display.height() - 18);
}

void draw_launcher()
{
    if (!kShowAppStatusScreens) {
        if (!expression_screen_visible) {
            show_expression(kDefaultExpression);
        }
        return;
    }
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);

    const int card_w = display.width() - 24;
    const int card_h = 36;
    const int first_y = 20;
    const char* titles[] = {
        "WiFi Connect",
        "Voice to Text",
        "Aliyun PCM TTS",
        "Camera Upload",
        "Tracking User",
    };
    const char* subtitles[] = {
        "Connect once before network apps",
        "Record voice and upload WAV by HTTP",
        "Stream text-to-speech as raw PCM",
        "Take one photo and upload it",
        "Face detect and turn toward user",
    };
    const bool wifi_connected = wifi_is_connected();

    for (int i = 0; i < kLauncherAppCount; ++i) {
        const int y = first_y + i * (card_h + 5);
        const uint16_t border = i == selected_menu ? TFT_CYAN : TFT_DARKGREY;
        display.drawRoundRect(12, y, card_w, card_h, 6, border);
        display.setTextColor(i == selected_menu ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
        display.setFont(&fonts::Font2);
        display.drawString(titles[i], 24, y + 4);
        display.setFont(&fonts::Font2);
        if (i == 0 && wifi_connected) {
            display.setTextColor(TFT_GREEN, TFT_BLACK);
            display.drawString("Connected", 24, y + 21);
            display.setTextColor(TFT_DARKGREY, TFT_BLACK);
            display.drawString(active_wifi_ssid.c_str(), 104, y + 21);
        } else {
            display.setTextColor(TFT_DARKGREY, TFT_BLACK);
            display.drawString(subtitles[i], 24, y + 21);
        }
    }

    display.setTextDatum(bottom_center);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.setFont(&fonts::Font2);
    display.drawString("Tap an app, or BtnA select / BtnB enter", display.width() / 2, display.height() - 8);
}

void draw_wifi_status()
{
    auto& display = M5.Display;
    draw_header("WiFi Connect");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(wifi_status.success ? TFT_GREEN : (wifi_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(wifi_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(wifi_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(wifi_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(wifi_status.line3, display.width() / 2, display.height() / 2 + 50);

    if (!wifi_status.running && !wifi_status.success) {
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("Tap screen or BtnB", display.width() / 2, display.height() / 2 + 80);
    }
}

void draw_wifi_setup_steps(const char* ap_ssid)
{
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);

    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("Setup", 18, 18);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("1. Connect to AP", 18, 58);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString(ap_ssid ? ap_ssid : "", 38, 84);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("Passwd", 38, 112);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString(kProvisioningApPassword, 104, 112);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("2. Open Browser", 18, 150);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString("192.168.4.1", 38, 176);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("3. Select WiFi & Server", 18, 214);
}

void draw_app1_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Voice to Text");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(app1_status.success ? TFT_GREEN : (app1_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(app1_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(app1_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(app1_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(app1_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void draw_tts_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Aliyun PCM TTS");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(tts_status.success ? TFT_GREEN : (tts_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(tts_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(tts_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(tts_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(tts_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void draw_camera_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Camera Upload");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(camera_status.success ? TFT_GREEN : (camera_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(camera_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(camera_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(camera_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(camera_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void draw_tracking_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Tracking User");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(tracking_status.success ? TFT_GREEN : (tracking_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(tracking_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(tracking_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(tracking_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(tracking_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void show_recognition_text(const char* title, const char* text)
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header(title);

    display.setTextDatum(top_left);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.drawString("Recognized:", 12, 58);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setCursor(12, 94);
    display.setTextWrap(true);
    display.print(text && text[0] ? text : "(empty)");

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("Listening will continue automatically", 12, display.height() - 42);
}

void set_app1_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                     bool running = true, bool success = false)
{
    snprintf(app1_status.stage, sizeof(app1_status.stage), "%s", stage);
    snprintf(app1_status.line1, sizeof(app1_status.line1), "%s", line1);
    snprintf(app1_status.line2, sizeof(app1_status.line2), "%s", line2);
    snprintf(app1_status.line3, sizeof(app1_status.line3), "%s", line3);
    app1_status.running = running;
    app1_status.success = success;
    ESP_LOGI(TAG, "APP1 status: %s | %s | %s | %s", app1_status.stage, app1_status.line1, app1_status.line2,
             app1_status.line3);
    if (voice_status_screen_suppressed || !kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_app1_status();
}

void set_wifi_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                     bool running = true, bool success = false)
{
    snprintf(wifi_status.stage, sizeof(wifi_status.stage), "%s", stage);
    snprintf(wifi_status.line1, sizeof(wifi_status.line1), "%s", line1);
    snprintf(wifi_status.line2, sizeof(wifi_status.line2), "%s", line2);
    snprintf(wifi_status.line3, sizeof(wifi_status.line3), "%s", line3);
    wifi_status.running = running;
    wifi_status.success = success;
    ESP_LOGI(TAG, "APP0 status: %s | %s | %s | %s", wifi_status.stage, wifi_status.line1, wifi_status.line2,
             wifi_status.line3);
    M5Lock lock;
    draw_wifi_status();
}

void set_tts_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                    bool running = true, bool success = false)
{
    snprintf(tts_status.stage, sizeof(tts_status.stage), "%s", stage);
    snprintf(tts_status.line1, sizeof(tts_status.line1), "%s", line1);
    snprintf(tts_status.line2, sizeof(tts_status.line2), "%s", line2);
    snprintf(tts_status.line3, sizeof(tts_status.line3), "%s", line3);
    tts_status.running = running;
    tts_status.success = success;
    ESP_LOGI(TAG, "TTS status: %s | %s | %s | %s", tts_status.stage, tts_status.line1, tts_status.line2,
             tts_status.line3);
    if (voice_status_screen_suppressed || !kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_tts_status();
}

void set_camera_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                       bool running = true, bool success = false)
{
    snprintf(camera_status.stage, sizeof(camera_status.stage), "%s", stage);
    snprintf(camera_status.line1, sizeof(camera_status.line1), "%s", line1);
    snprintf(camera_status.line2, sizeof(camera_status.line2), "%s", line2);
    snprintf(camera_status.line3, sizeof(camera_status.line3), "%s", line3);
    camera_status.running = running;
    camera_status.success = success;
    ESP_LOGI(TAG, "Camera status: %s | %s | %s | %s", camera_status.stage, camera_status.line1, camera_status.line2,
             camera_status.line3);
    if (!kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_camera_status();
}

void set_tracking_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                       bool running = true, bool success = false)
{
    snprintf(tracking_status.stage, sizeof(tracking_status.stage), "%s", stage);
    snprintf(tracking_status.line1, sizeof(tracking_status.line1), "%s", line1);
    snprintf(tracking_status.line2, sizeof(tracking_status.line2), "%s", line2);
    snprintf(tracking_status.line3, sizeof(tracking_status.line3), "%s", line3);
    tracking_status.running = running;
    tracking_status.success = success;
    ESP_LOGI(TAG, "Tracking status: %s | %s | %s | %s", tracking_status.stage, tracking_status.line1, tracking_status.line2,
             tracking_status.line3);
    if (!kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_tracking_status();
}

void set_current_network_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                                bool running = true, bool success = false)
{
    if (current_app == AppId::WifiConnect) {
        set_wifi_status(stage, line1, line2, line3, running, success);
    } else if (current_app == AppId::StreamTtsDemo) {
        set_tts_status(stage, line1, line2, line3, running, success);
    } else if (current_app == AppId::CameraUpload) {
        set_camera_status(stage, line1, line2, line3, running, success);
    } else if (current_app == AppId::TrackingUser) {
        set_tracking_status(stage, line1, line2, line3, running, success);
    } else {
        set_app1_status(stage, line1, line2, line3, running, success);
    }
}

void start_wifi_connect_task()
{
    if (wifi_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            ::run_wifi_connect_app();
            wifi_task_handle = nullptr;
            vTaskDelete(nullptr);
        }, "app0_wifi", kWifiTaskStackBytes, nullptr, 3, &wifi_task_handle, 1);
    } else {
        ESP_LOGI(TAG, "WiFi setup task is already running");
    }
}

const char* touch_state_name(const m5::Touch_Class::touch_detail_t& touch)
{
    if (touch.wasPressed()) {
        return "Pressed";
    }
    if (touch.wasClicked()) {
        return "Clicked / Released";
    }
    if (touch.wasHold()) {
        return "Hold Start";
    }
    if (touch.isHolding()) {
        return "Holding";
    }
    if (touch.wasFlickStart()) {
        return "Flick Start";
    }
    if (touch.isFlicking()) {
        return "Flicking";
    }
    if (touch.wasFlicked()) {
        return "Flick End";
    }
    if (touch.wasDragStart()) {
        return "Drag Start";
    }
    if (touch.isDragging()) {
        return "Dragging";
    }
    if (touch.wasDragged()) {
        return "Drag End";
    }
    if (touch.isPressed()) {
        return "Touching";
    }
    if (touch.wasReleased()) {
        return "Released";
    }
    return "Idle";
}

void draw_touch_status(const char* event_name, const m5::Touch_Class::touch_detail_t& touch)
{
    if (!kShowAppStatusScreens) {
        if (!expression_screen_visible) {
            show_expression(kDefaultExpression);
        }
        return;
    }
    auto& display = M5.Display;
    draw_header("APP2 Touch Events");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(strcmp(event_name, "Idle") == 0 ? TFT_DARKGREY : TFT_ORANGE, TFT_BLACK);
    display.drawString(event_name, display.width() / 2, display.height() / 2 - 24);

    char line[96];
    display.setFont(&fonts::Font4);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(line, sizeof(line), "x:%d y:%d", touch.x, touch.y);
    display.drawString(line, display.width() / 2, display.height() / 2 + 22);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    snprintf(line, sizeof(line), "dx:%d dy:%d clicks:%u", touch.deltaX(), touch.deltaY(), touch.getClickCount());
    display.drawString(line, display.width() / 2, display.height() / 2 + 48);
}

void enter_app(AppId app)
{
    if (current_app == AppId::VoiceDemo && app != AppId::VoiceDemo) {
        while (M5.Mic.isRecording()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        M5.Mic.end();
    }
    if (current_app == AppId::StreamTtsDemo && app != AppId::StreamTtsDemo) {
        app2_stop_requested = true;
        M5.Speaker.stop();
    }
    if (current_app == AppId::TrackingUser && app != AppId::TrackingUser) {
        tracking_stop_requested = true;
    }

    current_app = app;

    if (app == AppId::Launcher) {
        draw_launcher();
        return;
    }

    if (app == AppId::WifiConnect) {
        start_wifi_connect_task();
        return;
    }

    if (app == AppId::VoiceDemo) {
        app1_stop_requested = false;
        set_app1_status("Starting", "Local recording upload");
        if (xiaozhi_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_xiaozhi_ota_probe();
                xiaozhi_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app1_voice", kApp1TaskStackBytes, nullptr, 3, &xiaozhi_task_handle, 1);
        } else {
            set_app1_status("Running", "Probe task is already running");
        }
        return;
    }

    if (app == AppId::StreamTtsDemo) {
        app2_stop_requested = false;
        set_tts_status("Starting", "Aliyun PCM streaming TTS");
        if (stream_tts_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_stream_tts_demo();
                stream_tts_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app2_pcm_tts", kApp2TaskStackBytes, nullptr, 3, &stream_tts_task_handle, 1);
        } else {
            set_tts_status("Running", "Stream TTS task is already running");
        }
        return;
    }

    if (app == AppId::CameraUpload) {
        set_camera_status("Starting", "Taking one photo");
        if (camera_upload_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_camera_upload_app();
                camera_upload_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app3_camera", kCameraTaskStackBytes, nullptr, 3, &camera_upload_task_handle, 1);
        } else {
            set_camera_status("Running", "Camera upload already running");
        }
        return;
    }

    if (app == AppId::TrackingUser) {
        tracking_stop_requested = false;
        set_tracking_status("Starting", "Taking photo");
        if (tracking_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_tracking_user_demo();
                tracking_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app3_tracking", kTrackingTaskStackBytes, nullptr, 3, &tracking_task_handle, 1);
        } else {
            set_tracking_status("Running", "Camera task is already running");
        }
        return;
    }

    draw_touch_status("Idle", M5.Touch.getDetail());
}

bool back_requested()
{
    if (M5.BtnA.wasClicked()) {
        return true;
    }

    auto touch = M5.Touch.getDetail();
    return touch.wasClicked() && touch.x < 72 && touch.y < 52;
}

void update_launcher()
{
    if (M5.BtnA.wasClicked()) {
        selected_menu = (selected_menu + 1) % kLauncherAppCount;
        draw_launcher();
        return;
    }

    if (M5.BtnB.wasClicked()) {
        const AppId apps[] = {AppId::WifiConnect, AppId::VoiceDemo, AppId::StreamTtsDemo, AppId::CameraUpload,
                              AppId::TrackingUser};
        enter_app(apps[selected_menu]);
        return;
    }

    auto touch = M5.Touch.getDetail();
    if (!touch.wasClicked()) {
        return;
    }

    const int first_y = 20;
    const int card_h = 36;
    const int gap = 5;
    const AppId apps[] = {AppId::WifiConnect, AppId::VoiceDemo, AppId::StreamTtsDemo, AppId::CameraUpload,
                          AppId::TrackingUser};
    for (int i = 0; i < kLauncherAppCount; ++i) {
        const int y = first_y + i * (card_h + gap);
        if (touch.y >= y && touch.y < y + card_h) {
            selected_menu = i;
            enter_app(apps[i]);
            return;
        }
    }
}

void update_wifi_connect()
{
    if (back_requested()) {
        enter_app(AppId::Launcher);
        return;
    }

    if (wifi_task_handle != nullptr || wifi_status.running || wifi_status.success) {
        return;
    }

    auto touch = M5.Touch.getDetail();
    if (M5.BtnB.wasClicked() || touch.wasClicked()) {
        start_wifi_connect_task();
    }
}

void update_voice_demo()
{
    if (back_requested()) {
        app1_stop_requested = true;
        enter_app(AppId::Launcher);
        return;
    }
}

void update_stream_tts_demo()
{
    if (back_requested()) {
        app2_stop_requested = true;
        M5.Speaker.stop();
        enter_app(AppId::Launcher);
        return;
    }
}

void update_camera_upload()
{
    if (back_requested()) {
        enter_app(AppId::Launcher);
        return;
    }
}

void update_tracking_user()
{
    if (back_requested()) {
        tracking_stop_requested = true;
        enter_app(AppId::Launcher);
        return;
    }
}

}  // namespace

static esp_err_t init_nvs_once()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void force_core_s3_display_board()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("M5GFX", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open M5GFX failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u32(nvs_handle, "AUTODETECT", static_cast<uint32_t>(m5gfx::board_t::board_M5StackCoreS3));
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void ensure_client_id()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("xiaopai", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(client_id, sizeof(client_id), "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
        return;
    }

    size_t length = sizeof(client_id);
    err = nvs_get_str(nvs_handle, "client_id", client_id, &length);
    if (err == ESP_OK && strlen(client_id) > 0) {
        nvs_close(nvs_handle);
        return;
    }

    uint32_t r0 = esp_random();
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    snprintf(client_id, sizeof(client_id), "%08lx-%04lx-4%03lx-%04lx-%012llx", static_cast<unsigned long>(r0),
             static_cast<unsigned long>(r1 & 0xffff), static_cast<unsigned long>(r2 & 0x0fff),
             static_cast<unsigned long>((r2 & 0x3fff) | 0x8000),
             static_cast<unsigned long long>((static_cast<uint64_t>(r3) << 16) | (r0 & 0xffff)));
    nvs_set_str(nvs_handle, "client_id", client_id);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static std::string mac_address()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return std::string(mac_str);
}

struct ParsedUrl {
    std::string host;
    std::string path = "/";
    int port = 443;
    bool tls = true;
};

static bool parse_websocket_url(const std::string& url, ParsedUrl& parsed)
{
    const char* wss = "wss://";
    const char* ws = "ws://";
    size_t offset = 0;
    if (url.rfind(wss, 0) == 0) {
        parsed.tls = true;
        parsed.port = 443;
        offset = strlen(wss);
    } else if (url.rfind(ws, 0) == 0) {
        parsed.tls = false;
        parsed.port = 80;
        offset = strlen(ws);
    } else {
        return false;
    }

    size_t slash = url.find('/', offset);
    std::string host_port = slash == std::string::npos ? url.substr(offset) : url.substr(offset, slash - offset);
    parsed.path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = host_port.substr(0, colon);
        parsed.port = atoi(host_port.substr(colon + 1).c_str());
    } else {
        parsed.host = host_port;
    }
    return !parsed.host.empty() && parsed.port > 0;
}

static int32_t average_abs_level(const int16_t* samples, size_t count)
{
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += abs(samples[i]);
    }
    return static_cast<int32_t>(sum / std::max<size_t>(count, 1));
}

static bool mic_record_blocking(int16_t* samples, size_t count, uint32_t sample_rate)
{
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!M5.Mic.record(samples, count, sample_rate)) {
        return false;
    }

    const uint32_t record_ms = static_cast<uint32_t>(count * 1000 / sample_rate) + 2;
    vTaskDelay(pdMS_TO_TICKS(record_ms));
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

static std::string json_string_value(const cJSON* root, const char* key)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

static std::string make_ws_hello_message()
{
    char message[256];
    snprintf(message, sizeof(message),
             "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":false},"
             "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
             "\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d}}",
             kAudioSampleRate, kOpusFrameDurationMs);
    return std::string(message);
}

static std::string make_listen_message(const char* state)
{
    return std::string("{\"session_id\":\"") + xiaozhi_config.session_id +
           "\",\"type\":\"listen\",\"state\":\"" + state + "\",\"mode\":\"manual\"}";
}

static bool handle_ws_text_message(const char* data, size_t len, bool& got_hello, std::string& stt_text)
{
    ESP_LOGI(TAG, "WS text (%u bytes): %.*s", static_cast<unsigned>(len), static_cast<int>(len), data);

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "WS text is not JSON");
        return false;
    }

    std::string type = json_string_value(root, "type");
    if (type == "hello") {
        std::string transport = json_string_value(root, "transport");
        xiaozhi_config.session_id = json_string_value(root, "session_id");
        got_hello = transport == "websocket";
        ESP_LOGI(TAG, "WS hello transport=%s session_id=%s", transport.c_str(), xiaozhi_config.session_id.c_str());
    } else if (type == "stt") {
        stt_text = json_string_value(root, "text");
        ESP_LOGI(TAG, "STT result: %s", stt_text.c_str());
        show_recognition_text("Speech Text", stt_text.c_str());
    } else if (type == "tts") {
        std::string state = json_string_value(root, "state");
        std::string text = json_string_value(root, "text");
        ESP_LOGI(TAG, "TTS state=%s text=%s", state.c_str(), text.c_str());
        if (!text.empty()) {
            set_app1_status("TTS Text", text.c_str(), "Speech recognition already worked", "", false, true);
        }
    } else if (type == "llm") {
        std::string text = json_string_value(root, "text");
        ESP_LOGI(TAG, "LLM text=%s", text.c_str());
    } else if (!type.empty()) {
        ESP_LOGI(TAG, "Unhandled WS message type=%s", type.c_str());
    }

    cJSON_Delete(root);
    return true;
}

static bool receive_ws_once(esp_transport_handle_t ws, int timeout_ms, bool& got_hello, std::string& stt_text)
{
    int polled = esp_transport_poll_read(ws, timeout_ms);
    if (polled == 0) {
        return true;
    }
    if (polled < 0) {
        ESP_LOGE(TAG, "WS poll failed");
        return false;
    }

    char buffer[4096];
    int read_len = esp_transport_read(ws, buffer, sizeof(buffer) - 1, timeout_ms);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "WS read failed: %d", read_len);
        return false;
    }

    ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(ws);
    if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
        buffer[read_len] = '\0';
        return handle_ws_text_message(buffer, read_len, got_hello, stt_text);
    }
    if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
        ESP_LOGI(TAG, "WS binary audio from server ignored (%d bytes)", read_len);
        return true;
    }
    if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
        ESP_LOGW(TAG, "WS close frame received");
        return false;
    }

    ESP_LOGI(TAG, "WS opcode=%d len=%d", static_cast<int>(opcode), read_len);
    return true;
}

static void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA start");
        if (wifi_connect_requested) {
            ESP_LOGI(TAG, "Connecting to SSID '%s'", active_wifi_ssid.c_str());
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event ? event->reason : -1);
        if (wifi_manual_switching) {
            return;
        }
        if (wifi_retry_count++ < kWifiRetryLimit) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", wifi_retry_count, kWifiRetryLimit);
        } else {
            xEventGroupSetBits(wifi_event_group, kWifiFailedBit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        wifi_connect_requested = false;
        xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
    }
}

static std::string get_saved_wifi_password(const std::string& ssid);
static bool http_health_ok(const std::string& base_url);

static bool ensure_wifi_stack_started()
{
    if (wifi_event_group == nullptr) {
        wifi_event_group = xEventGroupCreate();
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!wifi_sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        wifi_sta_netif_created = true;
    }
    if (!wifi_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        wifi_ap_netif_created = true;
    }

    if (!wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                                            nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                                            nullptr));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    return true;
}

static bool save_wifi_credentials(const std::string& ssid, const std::string& password)
{
    if (ssid.empty()) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open wifi failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_str(nvs_handle, kWifiNvsSsidKey, ssid.c_str());
    nvs_set_str(nvs_handle, kWifiNvsPasswordKey, password.c_str());
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err == ESP_OK;
}

static bool load_saved_wifi_credentials(std::string& ssid, std::string& password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    char ssid_buf[33] = {};
    char password_buf[65] = {};
    size_t ssid_len = sizeof(ssid_buf);
    size_t password_len = sizeof(password_buf);
    err = nvs_get_str(nvs_handle, kWifiNvsSsidKey, ssid_buf, &ssid_len);
    if (err == ESP_OK) {
        esp_err_t pass_err = nvs_get_str(nvs_handle, kWifiNvsPasswordKey, password_buf, &password_len);
        if (pass_err != ESP_OK) {
            password_buf[0] = '\0';
        }
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK || ssid_buf[0] == '\0') {
        return false;
    }

    ssid = ssid_buf;
    password = password_buf;
    return true;
}

static std::string get_saved_wifi_password(const std::string& ssid)
{
    std::string saved_ssid;
    std::string saved_password;
    if (load_saved_wifi_credentials(saved_ssid, saved_password) && saved_ssid == ssid) {
        return saved_password;
    }

    for (const auto& candidate : kWifiCandidates) {
        if (candidate.ssid != nullptr && ssid == candidate.ssid) {
            return candidate.password != nullptr ? std::string(candidate.password) : std::string();
        }
    }
    return std::string();
}

static std::string normalize_server_base(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(),
                value.end());
    if (value.empty()) {
        return value;
    }
    if (value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0) {
        value = "http://" + value;
    }
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

static bool save_server_base(const std::string& base)
{
    if (base.empty()) {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open server failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_str(nvs_handle, kServerNvsBaseKey, base.c_str());
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return err == ESP_OK;
}

static bool load_saved_server_base(std::string& base)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(kWifiNvsNamespace, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    char base_buf[96] = {};
    size_t base_len = sizeof(base_buf);
    err = nvs_get_str(nvs_handle, kServerNvsBaseKey, base_buf, &base_len);
    nvs_close(nvs_handle);
    if (err != ESP_OK || base_buf[0] == '\0') {
        return false;
    }

    base = normalize_server_base(base_buf);
    return !base.empty();
}

static bool select_server_base(const std::string& requested_base, bool persist)
{
    std::string base = normalize_server_base(requested_base);
    if (!base.empty()) {
        ESP_LOGI(TAG, "Testing server base: %s", base.c_str());
        if (http_health_ok(base)) {
            active_server_base = base;
            active_server_selected = true;
            if (persist) {
                save_server_base(base);
            }
            ESP_LOGI(TAG, "Server selected: %s", active_server_base.c_str());
            show_expression("calm");
            return true;
        }

        active_server_selected = false;
        ESP_LOGW(TAG, "Server health failed: %s", base.c_str());
        return false;
    }

    return false;
}

static bool is_known_wifi_ssid(const std::string& ssid)
{
    std::string saved_ssid;
    std::string saved_password;
    if (load_saved_wifi_credentials(saved_ssid, saved_password) && saved_ssid == ssid) {
        return true;
    }
    for (const auto& candidate : kWifiCandidates) {
        if (candidate.ssid != nullptr && ssid == candidate.ssid) {
            return true;
        }
    }
    return false;
}

static bool configure_wifi_credentials(const std::string& ssid, const std::string& password)
{
    if (ssid.empty()) {
        return false;
    }
    active_wifi_ssid = ssid;

    wifi_config_t wifi_config = {};
    snprintf(reinterpret_cast<char*>(wifi_config.sta.ssid), sizeof(wifi_config.sta.ssid), "%s", ssid.c_str());
    snprintf(reinterpret_cast<char*>(wifi_config.sta.password), sizeof(wifi_config.sta.password), "%s",
             password.c_str());
    wifi_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed for %s: %s", ssid.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool connect_wifi_credentials(const std::string& ssid, const std::string& password, bool persist)
{
    if (!ensure_wifi_stack_started()) {
        return false;
    }

    ESP_LOGI(TAG, "Connecting WiFi credentials: %s", ssid.c_str());
    xEventGroupClearBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit);
    wifi_retry_count = 0;
    wifi_connect_requested = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(provisioning_started ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    if (!configure_wifi_credentials(ssid, password)) {
        wifi_connect_requested = false;
        return false;
    }

    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    } else {
        wifi_manual_switching = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        wifi_manual_switching = false;
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(18000));
    if (bits & kWifiConnectedBit) {
        active_wifi_ssid = ssid;
        active_server_selected = false;
        if (persist) {
            save_wifi_credentials(ssid, password);
        }
        ESP_LOGI(TAG, "WiFi connected: %s", active_wifi_ssid.c_str());
        return true;
    }

    wifi_connect_requested = false;
    ESP_LOGW(TAG, "WiFi connect failed: %s", ssid.c_str());
    return false;
}

static std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

static std::string make_provisioning_ap_ssid()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "Xiaopai-%02X%02X", mac[4], mac[5]);
    return std::string(ssid);
}

static std::string make_server_options_json()
{
    std::string saved_base;
    bool has_saved = load_saved_server_base(saved_base);
    std::string json = "\"savedServer\":\"";
    json += json_escape(has_saved ? saved_base : active_server_base);
    json += "\",\"servers\":[";
    bool first = true;
    if (has_saved) {
        json += "\"";
        json += json_escape(saved_base);
        json += "\"";
        first = false;
    }
    for (const char* base : kServerBaseCandidates) {
        if (base == nullptr || strlen(base) == 0) {
            continue;
        }
        std::string normalized = normalize_server_base(base);
        if (has_saved && normalized == saved_base) {
            continue;
        }
        if (!first) {
            json += ',';
        }
        json += "\"";
        json += json_escape(normalized);
        json += "\"";
        first = false;
    }
    json += "]";
    return json;
}

static const char* provisioning_page_html()
{
    return R"rawliteral(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小派同学 WiFi 连接界面</title>
<style>
:root{color-scheme:dark}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#111820;color:#eef3f7}
main{max-width:560px;margin:0 auto;padding:24px 16px 36px}
main.done{min-height:100vh;display:grid;place-items:center;padding:0 20px;text-align:center;font-size:24px;font-weight:750;color:#36c98f}
h1{font-size:25px;margin:6px 0 4px}
.sub{color:#a8b6c2;font-size:14px;margin:0 0 18px}
.panel{border:1px solid #2f3f4b;border-radius:8px;background:#17212b;padding:14px;margin:12px 0}
.status{min-height:22px;color:#8bd3ff;margin:10px 0 0;line-height:1.45}
.net,.server{display:flex;align-items:center;justify-content:space-between;gap:12px;border:1px solid #32424f;border-radius:8px;padding:12px;margin:8px 0;background:#101821}
.selected{border-color:#36c98f;background:#14241f}
.ssid,.server-name{font-size:16px;font-weight:650;overflow:hidden;text-overflow:ellipsis}
.meta{color:#a5b1bc;font-size:13px;margin-top:3px}
button{border:0;border-radius:8px;padding:10px 14px;background:#36c98f;color:#06110c;font-weight:750;font-size:15px}
button.secondary{background:#2b3540;color:#eef3f7}
button.full{width:100%;margin-top:12px}
label{display:block;color:#b9c5ce;font-size:13px;margin:10px 0 5px}
input{box-sizing:border-box;width:100%;border:1px solid #3d4b57;border-radius:8px;background:#0b1117;color:#eef3f7;padding:12px;font-size:16px}
dialog{border:1px solid #42515d;border-radius:8px;background:#151c24;color:#eef3f7;width:min(92vw,420px)}
dialog::backdrop{background:rgba(0,0,0,.55)}
.row{display:flex;gap:10px;justify-content:flex-end;margin-top:14px}
.hint{font-size:13px;color:#96a6b3;line-height:1.45}
</style>
</head>
<body>
<main>
<h1>小派同学 WiFi 连接界面</h1>
<p class="sub">先选择 WiFi，再选择本地服务地址。连接成功后小派同学会自动继续启动。</p>
<section class="panel">
  <strong>1. 选择 WiFi</strong>
  <div class="status" id="status">正在扫描附近 WiFi...</div>
  <button class="secondary" onclick="scan()">重新扫描</button>
  <div id="list"></div>
</section>
<section class="panel">
  <strong>2. 选择服务器</strong>
  <p class="hint">请选择电脑上的小派同学服务地址，或输入新的 IP + 端口，例如 192.168.1.23:8091。</p>
  <div id="servers"></div>
  <label for="customServer">自定义服务器</label>
  <input id="customServer" placeholder="192.168.1.23:8091">
</section>
<button class="full" onclick="connectSelected()">连接 WiFi 并检测服务器</button>
</main>
<dialog id="dlg">
<form method="dialog" onsubmit="event.preventDefault(); saveWifiDialog();">
<h2 id="dlgTitle">连接 WiFi</h2>
<input id="ssid" autocomplete="off" placeholder="SSID">
<input id="password" autocomplete="current-password" type="password" placeholder="密码">
<div class="row">
<button class="secondary" type="button" onclick="dlg.close()">取消</button>
<button type="submit">连接</button>
</div>
</form>
</dialog>
<script>
const list=document.getElementById('list'),serversEl=document.getElementById('servers'),statusEl=document.getElementById('status'),dlg=document.getElementById('dlg'),customServer=document.getElementById('customServer');
let selectedWifi=null,selectedServer='',savedServers=[];
function esc(s){return String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function normalizeServer(s){s=String(s||'').trim();if(!s)return'';if(!/^https?:\/\//i.test(s))s='http://'+s;return s.replace(/\/+$/,'');}
function showReady(){document.body.innerHTML='<main class="done">可以与小派同学互动啦~</main>';}
async function scan(){
  statusEl.textContent='正在扫描附近 WiFi...';
  const r=await fetch('/scan');
  const data=await r.json();
  if(data.connected&&data.serverOk){showReady();return;}
  savedServers=data.servers||[];
  selectedServer=data.savedServer||savedServers[0]||'';
  customServer.value=selectedServer;
  list.innerHTML='';
  for(const ap of data.aps){
    const row=document.createElement('div');
    row.className='net';
    row.innerHTML='<div><div class="ssid">'+esc(ap.ssid)+'</div><div class="meta">'+ap.rssi+' dBm '+(ap.known?'已保存，可直接连接':'需要输入密码')+'</div></div>';
    const b=document.createElement('button');
    b.textContent='选择';
    b.onclick=()=>selectWifi(ap.ssid,'',ap.known,row);
    row.appendChild(b);
    list.appendChild(row);
  }
  if(!data.aps.length) list.innerHTML='<p>没有扫描到 WiFi。</p>';
  const manual=document.createElement('div');
  manual.className='net';
  manual.innerHTML='<div><div class="ssid">手动输入隐藏网络</div><div class="meta">输入 SSID 和密码</div></div>';
  const mb=document.createElement('button');
  mb.textContent='输入';
  mb.onclick=()=>openDialog('');
  manual.appendChild(mb);
  list.appendChild(manual);
  renderServers();
  statusEl.textContent=data.connected?'当前已连接 '+data.connected+'，也可以重新选择网络':'请选择要连接的 WiFi';
}
function renderServers(){
  serversEl.innerHTML='';
  for(const base of savedServers){
    const row=document.createElement('div');
    row.className='server'+(normalizeServer(base)===normalizeServer(selectedServer)?' selected':'');
    row.innerHTML='<div><div class="server-name">'+esc(base)+'</div><div class="meta">点击使用这个服务地址</div></div>';
    const b=document.createElement('button');
    b.textContent='选择';
    b.onclick=()=>{selectedServer=normalizeServer(base);customServer.value=selectedServer;renderServers();};
    row.appendChild(b);
    serversEl.appendChild(row);
  }
}
function selectWifi(s,p,known,row){
  selectedWifi={ssid:s,password:p,known};
  document.querySelectorAll('.net').forEach(x=>x.classList.remove('selected'));
  if(row)row.classList.add('selected');
  statusEl.textContent='已选择 WiFi：'+s;
  if(!known)openDialog(s);
}
function openDialog(s){ssid.value=s||'';password.value='';dlg.showModal();setTimeout(()=>s?password.focus():ssid.focus(),50);}
function saveWifiDialog(){selectedWifi={ssid:ssid.value,password:password.value,known:false};dlg.close();statusEl.textContent='已选择 WiFi：'+ssid.value;}
async function connectSelected(){
  if(!selectedWifi){alert('请先选择 WiFi。');return;}
  const serverBase=normalizeServer(customServer.value||selectedServer);
  if(!serverBase){alert('请输入服务器 IP 和端口，例如 192.168.1.23:8091。');customServer.focus();return;}
  statusEl.textContent='正在连接 WiFi 并检测服务器...';
  const r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...selectedWifi,serverBase})});
  const data=await r.json();
  if(!data.ok){alert('WiFi 连接失败，请重新输入 WiFi 密码。');openDialog(selectedWifi.ssid);statusEl.textContent='WiFi 连接失败';return;}
  if(!data.serverOk){alert('服务器连接失败，请重新输入 IP + 端口。');customServer.focus();statusEl.textContent='WiFi 已连接，但服务器不可用';return;}
  showReady();
}
scan();
</script>
</body>
</html>
)rawliteral";
}

static esp_err_t provisioning_index_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, provisioning_page_html());
}

static esp_err_t provisioning_scan_handler(httpd_req_t* req)
{
    if (!ensure_wifi_stack_started()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi init failed");
        return ESP_FAIL;
    }

    wifi_scan_config_t scan_config = {};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
    }

    uint16_t ap_count = kProvisioningMaxScanResults;
    wifi_ap_record_t records[kProvisioningMaxScanResults] = {};
    esp_wifi_scan_get_ap_records(&ap_count, records);

    std::string json = "{\"connected\":\"";
    json += wifi_is_connected() ? json_escape(active_wifi_ssid) : "";
    json += "\",\"serverOk\":";
    json += (wifi_is_connected() && active_server_selected) ? "true" : "false";
    json += ",\"aps\":[";
    for (uint16_t i = 0; i < ap_count; ++i) {
        std::string ssid(reinterpret_cast<char*>(records[i].ssid));
        if (ssid.empty()) {
            continue;
        }
        if (json.back() != '[') {
            json += ',';
        }
        json += "{\"ssid\":\"";
        json += json_escape(ssid);
        json += "\",\"rssi\":";
        json += std::to_string(records[i].rssi);
        json += ",\"known\":";
        json += is_known_wifi_ssid(ssid) ? "true" : "false";
        json += "}";
    }
    json += "],";
    json += make_server_options_json();
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

static bool read_http_body(httpd_req_t* req, std::string& body)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        return false;
    }

    body.resize(req->content_len);
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, &body[received], req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += ret;
    }
    return true;
}

static esp_err_t provisioning_connect_handler(httpd_req_t* req)
{
    std::string body;
    if (!read_http_body(req, body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON* password_item = cJSON_GetObjectItem(root, "password");
    cJSON* known_item = cJSON_GetObjectItem(root, "known");
    cJSON* server_base_item = cJSON_GetObjectItem(root, "serverBase");
    std::string ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : "";
    std::string password = cJSON_IsString(password_item) ? password_item->valuestring : "";
    std::string server_base = cJSON_IsString(server_base_item) ? server_base_item->valuestring : "";
    bool known = cJSON_IsTrue(known_item);
    cJSON_Delete(root);

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    if (known && password.empty()) {
        password = get_saved_wifi_password(ssid);
    }

    bool ok = connect_wifi_credentials(ssid, password, true);
    bool server_ok = false;
    std::string normalized_server = normalize_server_base(server_base);
    if (ok) {
        server_ok = select_server_base(normalized_server, true);
    }
    std::string response = "{\"ok\":";
    response += ok ? "true" : "false";
    response += ",\"serverOk\":";
    response += server_ok ? "true" : "false";
    response += ",\"ssid\":\"";
    response += json_escape(ssid);
    response += "\",\"serverBase\":\"";
    response += json_escape(server_ok ? active_server_base : normalized_server);
    response += "\"}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response.c_str(), response.size());
}

static bool start_provisioning_portal()
{
    if (provisioning_started) {
        return true;
    }
    if (!ensure_wifi_stack_started()) {
        return false;
    }

    std::string ap_ssid = make_provisioning_ap_ssid();
    wifi_config_t ap_config = {};
    snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ap_ssid.c_str());
    snprintf(reinterpret_cast<char*>(ap_config.ap.password), sizeof(ap_config.ap.password), "%s",
             kProvisioningApPassword);
    ap_config.ap.ssid_len = ap_ssid.size();
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;
    if (strlen(kProvisioningApPassword) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }

    if (provisioning_httpd == nullptr) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.lru_purge_enable = true;
        config.stack_size = 6144;
        ESP_ERROR_CHECK(httpd_start(&provisioning_httpd, &config));

        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = provisioning_index_handler,
            .user_ctx = nullptr,
        };
        httpd_uri_t scan_uri = {
            .uri = "/scan",
            .method = HTTP_GET,
            .handler = provisioning_scan_handler,
            .user_ctx = nullptr,
        };
        httpd_uri_t connect_uri = {
            .uri = "/connect",
            .method = HTTP_POST,
            .handler = provisioning_connect_handler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(provisioning_httpd, &index_uri);
        httpd_register_uri_handler(provisioning_httpd, &scan_uri);
        httpd_register_uri_handler(provisioning_httpd, &connect_uri);
    }

    provisioning_started = true;
    {
        M5Lock lock;
        draw_wifi_setup_steps(ap_ssid.c_str());
    }
    return true;
}

static bool configure_wifi_candidate(const WifiCandidate& candidate)
{
    if (candidate.ssid == nullptr || strlen(candidate.ssid) == 0) {
        return false;
    }
    return configure_wifi_credentials(candidate.ssid, candidate.password ? candidate.password : "");
}

static bool wifi_is_connected()
{
    if (!wifi_started) {
        return false;
    }

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return wifi_event_group != nullptr && (xEventGroupGetBits(wifi_event_group) & kWifiConnectedBit) != 0;
    }

    if (wifi_event_group != nullptr) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        return (bits & kWifiConnectedBit) != 0;
    }
    return false;
}

static bool ensure_wifi_connected(bool allow_connect, bool force_candidate_scan = false, int start_candidate_index = 0)
{
    ensure_wifi_stack_started();

    if (!force_candidate_scan && wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi already connected: %s", active_wifi_ssid.c_str());
        return true;
    }

    if (!allow_connect) {
        ESP_LOGW(TAG, "WiFi is not connected; network app will not reconnect automatically");
        return false;
    }

    std::string saved_ssid;
    std::string saved_password;
    if (!force_candidate_scan && load_saved_wifi_credentials(saved_ssid, saved_password)) {
        return connect_wifi_credentials(saved_ssid, saved_password, false);
    }

    for (int offset = 0; offset < kWifiCandidateCount; ++offset) {
        const int candidate_index = (start_candidate_index + offset + kWifiCandidateCount) % kWifiCandidateCount;
        const WifiCandidate& candidate = kWifiCandidates[candidate_index];
        if (candidate.ssid == nullptr || strlen(candidate.ssid) == 0) {
            continue;
        }
        ESP_LOGI(TAG, "Trying WiFi candidate: %s", candidate.ssid);
        xEventGroupClearBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit);
        wifi_retry_count = 0;
        if (wifi_started) {
            wifi_manual_switching = true;
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(300));
            wifi_manual_switching = false;
        }
        if (!configure_wifi_candidate(candidate)) {
            continue;
        }

        wifi_connect_requested = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(provisioning_started ? WIFI_MODE_APSTA : WIFI_MODE_STA));
        if (!wifi_started) {
            ESP_ERROR_CHECK(esp_wifi_start());
            wifi_started = true;
        } else {
            esp_wifi_connect();
        }

        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(16000));
        if (bits & kWifiConnectedBit) {
            active_wifi_ssid = candidate.ssid;
            active_wifi_candidate_index = candidate_index;
            active_server_selected = false;
            ESP_LOGI(TAG, "WiFi candidate connected: %s", active_wifi_ssid.c_str());
            return true;
        }
        wifi_connect_requested = false;
        ESP_LOGW(TAG, "WiFi candidate failed: %s", candidate.ssid);
    }

    ESP_LOGE(TAG, "WiFi connection failed or timed out");
    active_wifi_candidate_index = -1;
    return false;
}

static bool ensure_wifi_connected()
{
    return ensure_wifi_connected(false);
}

void run_wifi_connect_app()
{
    ensure_client_id();
    if (start_provisioning_portal()) {
        if (wifi_is_connected()) {
            ESP_LOGI(TAG, "Provisioning portal remains open, WiFi already connected: %s", active_wifi_ssid.c_str());
        }
    } else {
        set_wifi_status("Setup", "Could not start portal", "", "", false, false);
    }
}

static std::string make_server_url(const char* path)
{
    std::string url = active_server_base;
    if (!url.empty() && url.back() == '/' && path != nullptr && path[0] == '/') {
        url.pop_back();
    }
    url += path ? path : "";
    return url;
}

static bool http_health_ok(const std::string& base_url)
{
    std::string url = base_url + "/health";
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 3500;
    config.buffer_size = 512;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Server health %s -> err=%s status=%d", url.c_str(), esp_err_to_name(err), status);
    return err == ESP_OK && status >= 200 && status < 300;
}

static bool ensure_server_selected()
{
    if (active_server_selected && http_health_ok(active_server_base)) {
        return true;
    }

    active_server_selected = false;
    ESP_LOGW(TAG, "Server is not selected or health check failed");
    return false;
}

static bool ensure_network_ready()
{
    if (!start_provisioning_portal()) {
        set_current_network_status("Setup", "Could not start portal", "", "", false, false);
        return false;
    }

    while (!wifi_is_connected() || !active_server_selected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    return true;
}

static std::string make_system_info_json()
{
    const esp_app_desc_t* app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":%u,"
             "\"minimum_free_heap_size\":%u,\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"chip_model_name\":\"%s\",\"chip_info\":{\"model\":%d,\"cores\":%d,\"revision\":%d,\"features\":%lu},"
             "\"application\":{\"name\":\"%s\",\"version\":\"%s\",\"idf_version\":\"%s\"},"
             "\"board\":{\"type\":\"xiaopai-m5unified-demo\",\"name\":\"Xiaopai Probe\"}}",
             0U, static_cast<unsigned>(esp_get_minimum_free_heap_size()), mac_address().c_str(), client_id,
             CONFIG_IDF_TARGET, chip.model, chip.cores, chip.revision, static_cast<unsigned long>(chip.features),
             app->project_name, app->version, app->idf_ver);
    return std::string(json);
}

static void summarize_ota_response(const std::string& response)
{
    ESP_LOGI(TAG, "OTA response body (%u bytes): %s", static_cast<unsigned>(response.size()), response.c_str());

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        set_app1_status("Parse Fail", "Response is not JSON", "", "", false, false);
        ESP_LOGE(TAG, "Failed to parse OTA response JSON");
        return;
    }

    cJSON* activation = cJSON_GetObjectItem(root, "activation");
    cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");

    if (cJSON_IsObject(websocket)) {
        cJSON* url = cJSON_GetObjectItem(websocket, "url");
        cJSON* token = cJSON_GetObjectItem(websocket, "token");
        xiaozhi_config.websocket_url = cJSON_IsString(url) ? url->valuestring : "";
        xiaozhi_config.websocket_token = cJSON_IsString(token) ? token->valuestring : "";
        ESP_LOGI(TAG, "WebSocket config found: url=%s token=%s", cJSON_IsString(url) ? url->valuestring : "(missing)",
                 cJSON_IsString(token) ? token->valuestring : "(missing)");
        set_app1_status("Got Token", cJSON_IsString(url) ? url->valuestring : "websocket config found",
                        cJSON_IsString(token) ? "WebSocket token present" : "WebSocket token missing",
                        "See USB serial log", false, cJSON_IsString(token));
    } else if (cJSON_IsObject(mqtt)) {
        cJSON* endpoint = cJSON_GetObjectItem(mqtt, "endpoint");
        ESP_LOGI(TAG, "MQTT config found: endpoint=%s", cJSON_IsString(endpoint) ? endpoint->valuestring : "(missing)");
        set_app1_status("Got MQTT", cJSON_IsString(endpoint) ? endpoint->valuestring : "mqtt config found",
                        "See USB serial log", "", false, true);
    } else if (cJSON_IsObject(activation)) {
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        ESP_LOGI(TAG, "Activation required: code=%s message=%s challenge=%s",
                 cJSON_IsString(code) ? code->valuestring : "(missing)",
                 cJSON_IsString(message) ? message->valuestring : "(missing)",
                 cJSON_IsString(challenge) ? challenge->valuestring : "(missing)");
        set_app1_status("Activation", cJSON_IsString(code) ? code->valuestring : "Activation required",
                        cJSON_IsString(message) ? message->valuestring : "Bind this device in console",
                        "See USB serial log", false, false);
    } else {
        set_app1_status("No Config", "No websocket/mqtt/activation", "See USB serial log", "", false, false);
        ESP_LOGW(TAG, "OTA response has no activation/websocket/mqtt object");
    }

    cJSON_Delete(root);
}

static bool request_xiaozhi_ota_config()
{
    set_app1_status("HTTP", "POST Xiaozhi OTA config");
    std::string body = make_system_info_json();
    ESP_LOGI(TAG, "OTA URL: %s", CONFIG_STACKCHAN_XIAOZHI_OTA_URL);
    ESP_LOGI(TAG, "Device-Id: %s", mac_address().c_str());
    ESP_LOGI(TAG, "Client-Id: %s", client_id);
    ESP_LOGI(TAG, "Request body: %s", body.c_str());

    esp_http_client_config_t config = {};
    config.url = CONFIG_STACKCHAN_XIAOZHI_OTA_URL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_app1_status("HTTP Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    std::string ua = std::string("xiaopai-m5unified/") + esp_app_get_description()->version;
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "Client-Id", client_id);
    esp_http_client_set_header(client, "User-Agent", ua.c_str());
    esp_http_client_set_header(client, "Accept-Language", "zh-CN");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, body.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA HTTP request failed: %s", esp_err_to_name(err));
        set_app1_status("HTTP Fail", esp_err_to_name(err), "See USB serial log", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    int written = esp_http_client_write(client, body.c_str(), body.size());
    if (written < 0 || static_cast<size_t>(written) != body.size()) {
        ESP_LOGE(TAG, "HTTP write failed: written=%d expected=%u", written, static_cast<unsigned>(body.size()));
        set_app1_status("Write Fail", "HTTP request body failed", "", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGW(TAG, "HTTP content length unknown: %d", content_length);
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "OTA HTTP status=%d content_length=%d", status, content_length);
    if (status != 200) {
        char line[64];
        snprintf(line, sizeof(line), "HTTP status %d", status);
        set_app1_status("HTTP Status", line, "Expected 200", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::string response;
    char buffer[512];
    while (true) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read failed");
            set_app1_status("Read Fail", "HTTP body read failed", "", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }
        buffer[read_len] = '\0';
        response.append(buffer, read_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    summarize_ota_response(response);
    return !xiaozhi_config.websocket_url.empty() && !xiaozhi_config.websocket_token.empty();
}

static void* create_opus_encoder(int& frame_size_samples, int& outbuf_size)
{
    esp_opus_enc_config_t cfg = {};
    cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    cfg.complexity = 0;
    cfg.enable_fec = false;
    cfg.enable_dtx = true;
    cfg.enable_vbr = true;

    void* encoder = nullptr;
    esp_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &encoder);
    if (ret != ESP_AUDIO_ERR_OK || encoder == nullptr) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed: %d", ret);
        return nullptr;
    }

    esp_opus_enc_get_frame_size(encoder, &frame_size_samples, &outbuf_size);
    frame_size_samples /= sizeof(int16_t);
    ESP_LOGI(TAG, "Opus encoder ready: frame_samples=%d outbuf=%d", frame_size_samples, outbuf_size);
    return encoder;
}

static esp_transport_handle_t open_xiaozhi_websocket(esp_transport_handle_t& parent)
{
    ParsedUrl parsed;
    if (!parse_websocket_url(xiaozhi_config.websocket_url, parsed)) {
        ESP_LOGE(TAG, "Invalid websocket URL: %s", xiaozhi_config.websocket_url.c_str());
        set_app1_status("WS Fail", "Invalid websocket URL", xiaozhi_config.websocket_url.c_str(), "", false, false);
        return nullptr;
    }

    parent = parsed.tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (parent == nullptr) {
        set_app1_status("WS Fail", "transport init failed", "", "", false, false);
        return nullptr;
    }
    if (parsed.tls) {
        esp_transport_ssl_crt_bundle_attach(parent, esp_crt_bundle_attach);
    }

    esp_transport_handle_t ws = esp_transport_ws_init(parent);
    if (ws == nullptr) {
        set_app1_status("WS Fail", "websocket init failed", "", "", false, false);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    std::string auth = "Bearer " + xiaozhi_config.websocket_token;
    std::string headers = std::string("Protocol-Version: 1\r\n") +
                          "Device-Id: " + mac_address() + "\r\n" +
                          "Client-Id: " + client_id + "\r\n";
    esp_transport_ws_set_path(ws, parsed.path.c_str());
    esp_transport_ws_set_auth(ws, auth.c_str());
    esp_transport_ws_set_headers(ws, headers.c_str());
    esp_transport_ws_set_user_agent(ws, "xiaopai-m5unified");

    set_app1_status("WS", "Connecting...", parsed.host.c_str(), parsed.path.c_str());
    ESP_LOGI(TAG, "Connecting WS host=%s port=%d path=%s tls=%d", parsed.host.c_str(), parsed.port, parsed.path.c_str(),
             parsed.tls);
    if (esp_transport_connect(ws, parsed.host.c_str(), parsed.port, 15000) != 0) {
        ESP_LOGE(TAG, "WS connect failed, status=%d", esp_transport_ws_get_upgrade_request_status(ws));
        set_app1_status("WS Fail", "connect/upgrade failed", "See USB serial log", "", false, false);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    bool got_hello = false;
    std::string stt_text;
    std::string hello = make_ws_hello_message();
    ESP_LOGI(TAG, "WS send hello: %s", hello.c_str());
    if (esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT, hello.c_str(), hello.size(), 5000) < 0) {
        set_app1_status("WS Fail", "hello send failed", "", "", false, false);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    int64_t deadline = esp_timer_get_time() + 10000000LL;
    while (!got_hello && esp_timer_get_time() < deadline && !app1_stop_requested) {
        if (!receive_ws_once(ws, 1000, got_hello, stt_text)) {
            break;
        }
    }

    if (!got_hello) {
        set_app1_status("WS Fail", "No server hello", "See USB serial log", "", false, false);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    set_app1_status("Ready", "Listening for voice", "Speak louder than threshold", "", false, true);
    return ws;
}

static bool send_opus_frame(esp_transport_handle_t ws, void* encoder, const int16_t* pcm, int samples, int outbuf_size)
{
    std::vector<uint8_t> outbuf(outbuf_size);
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
    in.len = samples * sizeof(int16_t);
    esp_audio_enc_out_frame_t out = {};
    out.buffer = outbuf.data();
    out.len = outbuf.size();
    out.encoded_bytes = 0;

    esp_err_t ret = esp_opus_enc_process(encoder, &in, &out);
    if (ret != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        ESP_LOGE(TAG, "Opus encode failed: %d encoded=%u", ret, static_cast<unsigned>(out.encoded_bytes));
        return false;
    }

    int written = esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_BINARY, reinterpret_cast<const char*>(out.buffer),
                                            out.encoded_bytes, 5000);
    if (written < 0) {
        ESP_LOGE(TAG, "WS audio send failed");
        return false;
    }
    ESP_LOGD(TAG, "Sent opus frame: pcm=%d encoded=%u", samples, static_cast<unsigned>(out.encoded_bytes));
    return true;
}

static bool run_one_speech_recognition(esp_transport_handle_t ws, void* encoder, int encoder_frame_samples,
                                       int encoder_outbuf_size, const int16_t* first_samples, int first_count)
{
    std::string start = make_listen_message("start");
    ESP_LOGI(TAG, "WS send listen start: %s", start.c_str());
    if (esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT, start.c_str(), start.size(), 5000) < 0) {
        set_app1_status("WS Fail", "listen start failed", "", "", false, false);
        return false;
    }

    set_app1_status("Recording", "Voice threshold hit", "Sending Opus to Xiaozhi", "");
    std::vector<int16_t> frame(encoder_frame_samples);
    int frame_pos = 0;
    int elapsed_ms = 0;
    int silence_ms = 0;
    bool got_hello = true;
    std::string stt_text;

    auto append_and_send = [&](const int16_t* samples, int count) -> bool {
        int offset = 0;
        while (offset < count) {
            int n = std::min(count - offset, encoder_frame_samples - frame_pos);
            memcpy(frame.data() + frame_pos, samples + offset, n * sizeof(int16_t));
            frame_pos += n;
            offset += n;
            if (frame_pos == encoder_frame_samples) {
                if (!send_opus_frame(ws, encoder, frame.data(), encoder_frame_samples, encoder_outbuf_size)) {
                    return false;
                }
                frame_pos = 0;
                elapsed_ms += kOpusFrameDurationMs;
                if (!receive_ws_once(ws, 0, got_hello, stt_text)) {
                    return false;
                }
            }
        }
        return true;
    };

    if (!append_and_send(first_samples, first_count)) {
        return false;
    }

    std::vector<int16_t> chunk(kVoiceProbeSamples);
    while (!app1_stop_requested && elapsed_ms < kRecordMaxMs) {
        if (!mic_record_blocking(chunk.data(), chunk.size(), kAudioSampleRate)) {
            ESP_LOGW(TAG, "Mic record returned false while recording");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(chunk.data(), chunk.size());
        silence_ms = level < kVoiceStopThreshold ? silence_ms + (kVoiceProbeSamples * 1000 / kAudioSampleRate) : 0;
        if (!append_and_send(chunk.data(), chunk.size())) {
            return false;
        }

        if (silence_ms >= kSilenceStopMs && elapsed_ms > 600) {
            ESP_LOGI(TAG, "Stop recording due to silence: %d ms", silence_ms);
            break;
        }
    }

    if (frame_pos > 0) {
        memset(frame.data() + frame_pos, 0, (encoder_frame_samples - frame_pos) * sizeof(int16_t));
        if (!send_opus_frame(ws, encoder, frame.data(), encoder_frame_samples, encoder_outbuf_size)) {
            return false;
        }
    }

    std::string stop = make_listen_message("stop");
    ESP_LOGI(TAG, "WS send listen stop: %s", stop.c_str());
    esp_transport_ws_send_raw(ws, WS_TRANSPORT_OPCODES_TEXT, stop.c_str(), stop.size(), 5000);
    set_app1_status("Recognizing", "Waiting for STT text", "See USB serial log", "");

    int64_t deadline = esp_timer_get_time() + 12000000LL;
    while (stt_text.empty() && esp_timer_get_time() < deadline && !app1_stop_requested) {
        if (!receive_ws_once(ws, 1000, got_hello, stt_text)) {
            return false;
        }
    }

    if (stt_text.empty()) {
        set_app1_status("No STT", "No text returned yet", "Try speaking louder/longer", "", false, false);
        ESP_LOGW(TAG, "Recognition completed without STT text");
    } else {
        set_app1_status("Recognized", stt_text.c_str(), "Listening again...", "", false, true);
    }
    return true;
}

static void append_le16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
}

static void append_le32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

static std::vector<uint8_t> make_wav_bytes(const std::vector<int16_t>& pcm)
{
    const uint32_t data_bytes = pcm.size() * sizeof(int16_t);
    std::vector<uint8_t> wav;
    wav.reserve(44 + data_bytes);

    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    append_le32(wav, 36 + data_bytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    append_le32(wav, 16);
    append_le16(wav, 1);
    append_le16(wav, 1);
    append_le32(wav, kRecordSampleRate);
    append_le32(wav, kRecordSampleRate * sizeof(int16_t));
    append_le16(wav, sizeof(int16_t));
    append_le16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    append_le32(wav, data_bytes);

    const uint8_t* pcm_bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), pcm_bytes, pcm_bytes + data_bytes);
    return wav;
}

static bool upload_wav_recording(const std::vector<int16_t>& pcm)
{
    std::vector<uint8_t> wav = make_wav_bytes(pcm);
    std::string upload_url = make_server_url("/upload-audio");
    ESP_LOGI(TAG, "Uploading WAV to %s, samples=%u bytes=%u", upload_url.c_str(),
             static_cast<unsigned>(pcm.size()), static_cast<unsigned>(wav.size()));
    ESP_LOGI(TAG, "APP1 stack high water=%u bytes, free heap=%u, min free heap=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    set_app1_status("Uploading", upload_url.c_str(), "Sending WAV over HTTP", "");

    esp_http_client_config_t config = {};
    config.url = upload_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kAudioUploadTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_app1_status("Upload Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "X-Client-Id", client_id);

    esp_err_t err = esp_http_client_open(client, wav.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Upload open failed: %s", esp_err_to_name(err));
        set_app1_status("Upload Fail", esp_err_to_name(err), "Check server URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t offset = 0;
    while (offset < wav.size()) {
        size_t chunk = std::min<size_t>(kHttpBufferSize, wav.size() - offset);
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(wav.data() + offset), chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "Upload write failed at offset=%u", static_cast<unsigned>(offset));
            set_app1_status("Upload Fail", "HTTP write failed", "See USB serial log", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        offset += written;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Upload HTTP status=%d content_length=%d", status, content_length);

    std::string response;
    char response_chunk[256];
    while (response.size() < 2048) {
        int read_len = esp_http_client_read(client, response_chunk, sizeof(response_chunk) - 1);
        if (read_len <= 0) {
            break;
        }
        response.append(response_chunk, read_len);
    }
    if (!response.empty()) {
        ESP_LOGI(TAG, "Upload response: %s", response.c_str());
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        std::string stt_text;
        if (!response.empty()) {
            cJSON* root = cJSON_Parse(response.c_str());
            if (root != nullptr) {
                std::string type = json_string_value(root, "type");
                if (type == "stt") {
                    stt_text = json_string_value(root, "text");
                } else if (type == "error") {
                    std::string message = json_string_value(root, "message");
                    set_app1_status("STT Error", message.empty() ? "server returned error" : message.c_str(),
                                    "See local server log", "", false, false);
                    cJSON_Delete(root);
                    return false;
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "Upload response is not JSON");
            }
        }

        if (!stt_text.empty()) {
            ESP_LOGI(TAG, "Background ASR text: %s", stt_text.c_str());
        } else {
            char line[96];
            snprintf(line, sizeof(line), "samples=%u wav=%u bytes", static_cast<unsigned>(pcm.size()),
                     static_cast<unsigned>(wav.size()));
            set_app1_status("No Speech", line, "Server returned empty text", "", false, true);
        }
        return true;
    }

    char line[48];
    snprintf(line, sizeof(line), "HTTP status %d", status);
    set_app1_status("Upload Fail", line, "See USB serial log", "", false, false);
    return false;
}

static void append_capped(std::vector<int16_t>& dst, const int16_t* samples, size_t count, size_t max_count)
{
    if (count >= max_count) {
        dst.assign(samples + count - max_count, samples + count);
        return;
    }
    if (dst.size() + count > max_count) {
        dst.erase(dst.begin(), dst.begin() + (dst.size() + count - max_count));
    }
    dst.insert(dst.end(), samples, samples + count);
}

static std::vector<int16_t> record_pcm_after_trigger(const std::vector<int16_t>& pre_roll, int32_t trigger_level)
{
    const size_t max_samples = static_cast<size_t>(kRecordSampleRate) * kRecordMaxMs / 1000;
    std::vector<int16_t> pcm;
    pcm.reserve(max_samples + pre_roll.size() + kVoiceProbeSamples);
    pcm.insert(pcm.end(), pre_roll.begin(), pre_roll.end());

    std::vector<int16_t> chunk(kVoiceProbeSamples);
    int elapsed_ms = pre_roll.size() * 1000 / kRecordSampleRate;
    int silence_ms = 0;
    uint32_t last_draw_ms = 0;
    int32_t smooth_level = trigger_level;

    set_app1_status("Recording", "Voice threshold hit", "Pre-roll captured", "");
    while (!app1_stop_requested && !voice_listener_paused && elapsed_ms < kRecordMaxMs && pcm.size() < max_samples) {
        if (!mic_record_blocking(chunk.data(), chunk.size(), kRecordSampleRate)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(chunk.data(), chunk.size());
        smooth_level = (smooth_level * 3 + level) / 4;
        silence_ms = smooth_level < kVoiceStopThreshold ? silence_ms + kRecordChunkMs : 0;
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        elapsed_ms += kRecordChunkMs;

        uint32_t now = M5.millis();
        if (now - last_draw_ms > 900) {
            char line1[64];
            char line2[64];
            snprintf(line1, sizeof(line1), "level=%ld smooth=%ld", static_cast<long>(level), static_cast<long>(smooth_level));
            snprintf(line2, sizeof(line2), "time=%dms silence=%dms", elapsed_ms, silence_ms);
            set_app1_status("Recording", line1, line2, "");
            last_draw_ms = now;
        }

        if (silence_ms >= kSilenceStopMs && elapsed_ms > kPreRollMs + 600) {
            ESP_LOGI(TAG, "Stop recording due to silence: elapsed=%d silence=%d", elapsed_ms, silence_ms);
            break;
        }
    }

    ESP_LOGI(TAG, "Recorded PCM samples=%u duration=%ums", static_cast<unsigned>(pcm.size()),
             static_cast<unsigned>(pcm.size() * 1000 / kRecordSampleRate));
    return pcm;
}

static void run_local_record_upload_loop()
{
    ensure_client_id();
    while (!wifi_is_connected() || !active_server_selected) {
        set_app1_status("Waiting", "Network is starting", "Listening starts soon", "", true, false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    M5.Speaker.end();
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = kRecordSampleRate;
    mic_cfg.magnification = CONFIG_STACKCHAN_MIC_MAGNIFICATION;
    mic_cfg.noise_filter_level = 0;
    mic_cfg.task_pinned_core = 1;
    M5.Mic.config(mic_cfg);
    if (!M5.Mic.isEnabled() && !M5.Mic.begin()) {
        set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
        ESP_LOGE(TAG, "M5.Mic.begin failed");
        return;
    }

    set_app1_status("Ready", "Listening", make_server_url("/upload-audio").c_str(), "", false, true);
    std::vector<int16_t> probe(kVoiceProbeSamples);
    std::vector<int16_t> pre_roll;
    pre_roll.reserve(kPreRollSamples + kVoiceProbeSamples);
    uint32_t last_status_ms = 0;
    int32_t smooth_level = 0;

    while (!app1_stop_requested) {
        if (voice_listener_paused) {
            if (M5.Mic.isEnabled()) {
                while (M5.Mic.isRecording()) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                M5.Mic.end();
            }
            set_app1_status("Paused", "Executing command", "Listening resumes soon", "", true, false);
            while (voice_listener_paused && !app1_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (app1_stop_requested) {
                break;
            }
            M5.Mic.config(mic_cfg);
            if (!M5.Mic.isEnabled() && !M5.Mic.begin()) {
                set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
                ESP_LOGE(TAG, "M5.Mic.begin failed after pause");
                break;
            }
            pre_roll.clear();
            smooth_level = 0;
            last_status_ms = 0;
            set_app1_status("Listening", "Resumed", "Speak to upload", "", true, false);
        }
        if (!mic_record_blocking(probe.data(), probe.size(), kRecordSampleRate)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(probe.data(), probe.size());
        smooth_level = smooth_level == 0 ? level : (smooth_level * 3 + level) / 4;
        append_capped(pre_roll, probe.data(), probe.size(), kPreRollSamples);
        uint32_t now = M5.millis();
        if (now - last_status_ms > 700) {
            char line[64];
            snprintf(line, sizeof(line), "level=%ld smooth=%ld start=%d", static_cast<long>(level),
                     static_cast<long>(smooth_level), kVoiceStartThreshold);
            set_app1_status("Listening", line, "Speak to record/upload", "", true, false);
            last_status_ms = now;
        }

        if (!voice_listener_paused && smooth_level >= kVoiceStartThreshold) {
            ESP_LOGI(TAG, "Voice threshold triggered: level=%ld smooth=%ld start=%d stop=%d pre_roll=%ums sample_rate=%d",
                     static_cast<long>(level), static_cast<long>(smooth_level), kVoiceStartThreshold,
                     kVoiceStopThreshold, kPreRollMs, kRecordSampleRate);
            request_speak_preempt("voice activity");
            auto pcm = record_pcm_after_trigger(pre_roll, smooth_level);
            if (!pcm.empty() && !app1_stop_requested && !voice_listener_paused) {
                upload_wav_recording(pcm);
            }
            pre_roll.clear();
            smooth_level = 0;
            last_status_ms = 0;
        }
    }

    set_app1_status("Stopped", "Voice stopped", "", "", false, false);
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();
}

static void pause_voice_listener_for_shared_peripherals(const char* reason)
{
    voice_listener_paused = true;
    ESP_LOGI(TAG, "Voice listener pause requested: %s", reason != nullptr ? reason : "shared peripheral use");

    const int max_wait_ms = kVoiceListenerReleaseWaitMs;
    int waited_ms = 0;
    while (xiaozhi_task_handle != nullptr && M5.Mic.isEnabled() && waited_ms < max_wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }

    if (M5.Mic.isEnabled()) {
        ESP_LOGW(TAG, "Voice listener did not release mic in %dms; forcing mic end", max_wait_ms);
        while (M5.Mic.isRecording()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        M5.Mic.end();
    }
}

static void resume_voice_listener_after_shared_peripherals()
{
    voice_listener_paused = false;
}

class VoiceListenerPauseGuard {
public:
    explicit VoiceListenerPauseGuard(const char* reason)
    {
        pause_voice_listener_for_shared_peripherals(reason);
    }

    ~VoiceListenerPauseGuard()
    {
        resume_voice_listener_after_shared_peripherals();
    }

    VoiceListenerPauseGuard(const VoiceListenerPauseGuard&) = delete;
    VoiceListenerPauseGuard& operator=(const VoiceListenerPauseGuard&) = delete;
};

static void run_xiaozhi_speech_loop()
{
    if (xiaozhi_config.websocket_url.empty() || xiaozhi_config.websocket_token.empty()) {
        set_app1_status("No WS", "OTA did not return websocket config", "Cannot recognize speech", "", false, false);
        return;
    }

    M5.Speaker.end();
    if (!M5.Mic.isEnabled() && !M5.Mic.begin()) {
        set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
        ESP_LOGE(TAG, "M5.Mic.begin failed");
        return;
    }

    int encoder_frame_samples = 0;
    int encoder_outbuf_size = 0;
    void* encoder = create_opus_encoder(encoder_frame_samples, encoder_outbuf_size);
    if (encoder == nullptr || encoder_frame_samples != kOpusFrameSamples) {
        set_app1_status("Opus Fail", "Could not start encoder", "See USB serial log", "", false, false);
        if (encoder != nullptr) {
            esp_opus_enc_close(encoder);
        }
        return;
    }

    esp_transport_handle_t parent = nullptr;
    esp_transport_handle_t ws = open_xiaozhi_websocket(parent);
    if (ws == nullptr) {
        esp_opus_enc_close(encoder);
        return;
    }

    std::vector<int16_t> probe(kVoiceProbeSamples);
    uint32_t last_status_ms = 0;
    bool got_hello = true;
    std::string stt_text;
    while (!app1_stop_requested) {
        if (!mic_record_blocking(probe.data(), probe.size(), kAudioSampleRate)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(probe.data(), probe.size());
        uint32_t now = M5.millis();
        if (now - last_status_ms > 700) {
            char level_line[64];
            snprintf(level_line, sizeof(level_line), "level=%ld threshold=%d", static_cast<long>(level), kVoiceStartThreshold);
            set_app1_status("Listening", level_line, "Speak to recognize", "", true, false);
            last_status_ms = now;
        }

        receive_ws_once(ws, 0, got_hello, stt_text);
        if (level >= kVoiceStartThreshold) {
            ESP_LOGI(TAG, "Voice threshold triggered: level=%ld threshold=%d", static_cast<long>(level), kVoiceStartThreshold);
            if (!run_one_speech_recognition(ws, encoder, encoder_frame_samples, encoder_outbuf_size, probe.data(),
                                            probe.size())) {
                ESP_LOGW(TAG, "Speech recognition round failed; reconnecting websocket");
                esp_transport_close(ws);
                esp_transport_destroy(ws);
                esp_transport_destroy(parent);
                parent = nullptr;
                ws = open_xiaozhi_websocket(parent);
                if (ws == nullptr) {
                    break;
                }
            }
            last_status_ms = 0;
        }
    }

    set_app1_status("Stopped", "Voice stopped", "", "", false, false);
    esp_transport_close(ws);
    esp_transport_destroy(ws);
    if (parent != nullptr) {
        esp_transport_destroy(parent);
    }
    esp_opus_enc_close(encoder);
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();
}

void run_xiaozhi_ota_probe()
{
    ESP_LOGI(TAG, "Starting local voice recording upload demo over USB serial debug");
    set_app1_status("Probe", "Preparing local recorder");
    run_local_record_upload_loop();
}

static std::string url_encode(const char* value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        unsigned char c = *p;
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

static std::string make_tts_url()
{
    std::string url = CONFIG_STACKCHAN_TTS_URL;
    url += (url.find('?') == std::string::npos) ? "?text=" : "&text=";
    url += url_encode(CONFIG_STACKCHAN_TTS_TEXT);
    return url;
}

static std::string make_stream_tts_url_for_text(const char* text)
{
    std::string url = make_server_url("/stream-speak");
    url += (url.find('?') == std::string::npos) ? "?text=" : "&text=";
    url += url_encode(text != nullptr ? text : "");
    return url;
}

static std::string make_event_audio_url(const char* name)
{
    std::string url = make_server_url("/event-audio/");
    url += url_encode(name != nullptr ? name : "");
    url += ".pcm";
    return url;
}

static std::string make_stream_tts_url()
{
    return make_stream_tts_url_for_text(CONFIG_STACKCHAN_TTS_TEXT);
}

static bool init_camera_once()
{
    if (camera_initialized) {
        return true;
    }

    camera_config_t config = {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = -1;
    config.pin_sccb_sda = 12;
    config.pin_sccb_scl = 11;
    config.pin_d7 = 47;
    config.pin_d6 = 48;
    config.pin_d5 = 16;
    config.pin_d4 = 15;
    config.pin_d3 = 42;
    config.pin_d2 = 41;
    config.pin_d1 = 40;
    config.pin_d0 = 39;
    config.pin_vsync = 46;
    config.pin_href = 38;
    config.pin_pclk = 45;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 0;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = -1;

    set_tracking_status("Camera", "Initializing GC0308", "QVGA RGB565", "");
    M5.In_I2C.release();
    camera_owns_internal_i2c = true;
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        esp_camera_deinit();
        M5.In_I2C.begin();
        camera_owns_internal_i2c = false;
        set_tracking_status("Cam Fail", esp_err_to_name(err), "Check CoreS3 camera", "", false, false);
        return false;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor != nullptr) {
        sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        sensor->set_pixformat(sensor, PIXFORMAT_RGB565);
    }

    camera_initialized = true;
    return true;
}

static void release_camera_driver()
{
    if (camera_initialized) {
        esp_camera_deinit();
        camera_initialized = false;
    }
    M5.In_I2C.begin();
    camera_owns_internal_i2c = false;
}

static camera_fb_t* get_fresh_camera_frame(const char* context)
{
    for (int i = 0; i < kCameraFreshDiscardFrames; ++i) {
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale == nullptr) {
            ESP_LOGW(TAG, "Camera stale-frame flush failed context=%s index=%d", context != nullptr ? context : "",
                     i + 1);
            break;
        }
        ESP_LOGI(TAG, "Camera stale-frame flushed context=%s index=%d len=%u", context != nullptr ? context : "",
                 i + 1, static_cast<unsigned>(stale->len));
        esp_camera_fb_return(stale);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    return esp_camera_fb_get();
}

struct FaceTarget {
    bool found = false;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float area = 0.0f;
};

struct ScanPose {
    const char* label;
    float yaw;
    float pitch;
};

struct ScanCandidate {
    bool found = false;
    FaceTarget face;
    float pose_yaw = 0.0f;
    float pose_pitch = kTrackingHomePitchDeg;
    float pixel_error = 0.0f;
};

static float clamp_float(float value, float min_value, float max_value)
{
    return std::max(min_value, std::min(max_value, value));
}

static int speaker_volume_raw_from_percent(int percent)
{
    int clamped = std::max(kSpeakerVolumePercentMin, std::min(kSpeakerVolumePercentMax, percent));
    return std::max(0, std::min(255, (clamped * 255 + 50) / 100));
}

static void apply_speaker_volume()
{
    M5.Speaker.setVolume(speaker_volume_raw_from_percent(speaker_volume_percent));
}

static uint8_t scs_checksum(const uint8_t* data, size_t length_without_checksum)
{
    uint32_t sum = 0;
    for (size_t i = 2; i < length_without_checksum; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(~(sum & 0xff));
}

static bool init_servo_uart()
{
    if (servo_uart_initialized) {
        return true;
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = kServoBaud;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(kServoUart, 1024, 0, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_param_config(kServoUart, &uart_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_pin(kServoUart, kServoTxPin, kServoRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush(kServoUart);
    servo_uart_initialized = true;
    ESP_LOGI(TAG, "Servo UART ready: uart=%d tx=%d rx=%d baud=%d", static_cast<int>(kServoUart), kServoTxPin,
             kServoRxPin, kServoBaud);
    return true;
}

static bool scs_write(uint8_t id, uint8_t address, const uint8_t* values, size_t value_count)
{
    if (!init_servo_uart()) {
        return false;
    }
    uint8_t packet[16] = {};
    const size_t length = value_count + 3;
    const size_t packet_len = value_count + 7;
    if (packet_len > sizeof(packet)) {
        return false;
    }
    packet[0] = 0xff;
    packet[1] = 0xff;
    packet[2] = id;
    packet[3] = static_cast<uint8_t>(length);
    packet[4] = 0x03;
    packet[5] = address;
    for (size_t i = 0; i < value_count; ++i) {
        packet[6 + i] = values[i];
    }
    packet[packet_len - 1] = scs_checksum(packet, packet_len - 1);
    int written = uart_write_bytes(kServoUart, packet, packet_len);
    uart_wait_tx_done(kServoUart, pdMS_TO_TICKS(40));
    return written == static_cast<int>(packet_len);
}

static bool set_servo_torque(uint8_t id, bool enabled)
{
    uint8_t value = enabled ? 1 : 0;
    return scs_write(id, 40, &value, 1);
}

static int angle_deg_to_raw(float angle_deg, int zero_raw)
{
    int raw = static_cast<int>(std::lround(zero_raw + angle_deg * kServoStepsPerDegree));
    return std::max(0, std::min(1000, raw));
}

static bool set_servo_raw_in_time(uint8_t id, int raw, uint16_t time_ms)
{
    uint8_t values[4] = {
        static_cast<uint8_t>((raw >> 8) & 0xff),
        static_cast<uint8_t>(raw & 0xff),
        static_cast<uint8_t>((time_ms >> 8) & 0xff),
        static_cast<uint8_t>(time_ms & 0xff),
    };
    return scs_write(id, 42, values, sizeof(values));
}

static bool py32_write_bit(uint8_t low_reg, uint8_t high_reg, uint8_t pin, bool enabled)
{
    uint8_t reg = pin < 8 ? low_reg : high_reg;
    uint8_t mask = 1 << (pin < 8 ? pin : pin - 8);
    uint8_t value = M5.In_I2C.readRegister8(kPy32Address, reg, kPy32I2cFreq);
    value = enabled ? (value | mask) : (value & ~mask);
    return M5.In_I2C.writeRegister8(kPy32Address, reg, value, kPy32I2cFreq);
}

static void enable_servo_power()
{
    if (servo_power_ready) {
        return;
    }
    if (camera_owns_internal_i2c) {
        ESP_LOGW(TAG, "Cannot enable servo power while camera owns internal I2C");
        return;
    }
    M5Lock lock;
    uint8_t version = M5.In_I2C.readRegister8(kPy32Address, 0x02, kPy32I2cFreq);
    if (version == 0 || version == 0xff) {
        ESP_LOGW(TAG, "PY32 IO expander not detected; servo power may already be on");
        servo_power_ready = true;
        return;
    }
    py32_write_bit(0x03, 0x04, kPy32ServoPowerPin, true);  // direction output
    py32_write_bit(0x0b, 0x0c, kPy32ServoPowerPin, false); // pull-down off
    py32_write_bit(0x09, 0x0a, kPy32ServoPowerPin, true);  // pull-up on
    py32_write_bit(0x05, 0x06, kPy32ServoPowerPin, true);  // power on
    servo_power_ready = true;
    ESP_LOGI(TAG, "Servo power enabled via PY32 version=0x%02x", version);
}

static bool move_head_to_tracking_angles(float yaw_deg, float pitch_deg, uint16_t time_ms)
{
    enable_servo_power();
    tracking_yaw_deg = clamp_float(yaw_deg, kTrackingYawMinDeg, kTrackingYawMaxDeg);
    tracking_pitch_deg = clamp_float(pitch_deg, kTrackingPitchMinDeg, kTrackingPitchMaxDeg);
    int yaw_raw = angle_deg_to_raw(tracking_yaw_deg, kServoYawZeroRaw);
    int pitch_raw = angle_deg_to_raw(tracking_pitch_deg, kServoPitchZeroRaw);
    set_servo_torque(kServoPanId, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    set_servo_torque(kServoTiltId, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    bool ok_yaw = set_servo_raw_in_time(kServoPanId, yaw_raw, time_ms);
    vTaskDelay(pdMS_TO_TICKS(90));
    bool ok_pitch = set_servo_raw_in_time(kServoTiltId, pitch_raw, time_ms);
    vTaskDelay(pdMS_TO_TICKS(90));
    ESP_LOGI(TAG, "Head move yaw=%.1f raw=%d pitch=%.1f raw=%d ok=%d/%d", tracking_yaw_deg, yaw_raw,
             tracking_pitch_deg, pitch_raw, ok_yaw, ok_pitch);
    return ok_yaw && ok_pitch;
}

static bool move_tilt_only_for_test(float pitch_deg, uint16_t time_ms)
{
    enable_servo_power();
    float clamped_pitch = clamp_float(pitch_deg, kTrackingPitchMinDeg, kTrackingPitchMaxDeg);
    int pitch_raw = angle_deg_to_raw(clamped_pitch, kServoPitchZeroRaw);
    set_servo_torque(kServoTiltId, true);
    bool ok_pitch = set_servo_raw_in_time(kServoTiltId, pitch_raw, time_ms);
    ESP_LOGI(TAG, "Tilt-only test pitch=%.1f raw=%d ok=%d", clamped_pitch, pitch_raw, ok_pitch);
    return ok_pitch;
}

static bool move_pan_only_for_test(float yaw_deg, uint16_t time_ms)
{
    enable_servo_power();
    float clamped_yaw = clamp_float(yaw_deg, kTrackingYawMinDeg, kTrackingYawMaxDeg);
    int yaw_raw = angle_deg_to_raw(clamped_yaw, kServoYawZeroRaw);
    set_servo_torque(kServoPanId, true);
    bool ok_yaw = set_servo_raw_in_time(kServoPanId, yaw_raw, time_ms);
    ESP_LOGI(TAG, "Pan-only test yaw=%.1f raw=%d ok=%d", clamped_yaw, yaw_raw, ok_yaw);
    return ok_yaw;
}

static bool move_center_for_test()
{
    bool ok_pan = move_pan_only_for_test(0.0f, 450);
    vTaskDelay(pdMS_TO_TICKS(220));
    bool ok_tilt = move_tilt_only_for_test(kTrackingHomePitchDeg, 550);
    if (ok_pan) {
        tracking_yaw_deg = 0.0f;
    }
    if (ok_tilt) {
        tracking_pitch_deg = kTrackingHomePitchDeg;
    }
    ESP_LOGI(TAG, "Center-only test home pitch=%.1f ok=%d/%d", kTrackingHomePitchDeg, ok_pan, ok_tilt);
    return ok_pan && ok_tilt;
}

static bool parse_face_target(const std::string& response, FaceTarget* target)
{
    if (target == nullptr) {
        return false;
    }
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        return false;
    }

    cJSON* face_detection = cJSON_GetObjectItemCaseSensitive(root, "face_detection");
    cJSON* face = cJSON_GetObjectItemCaseSensitive(face_detection, "best_face");
    if (!cJSON_IsObject(face)) {
        cJSON* faces = cJSON_GetObjectItemCaseSensitive(face_detection, "faces");
        if (cJSON_IsArray(faces) && cJSON_GetArraySize(faces) > 0) {
            face = cJSON_GetArrayItem(faces, 0);
        }
    }
    if (!cJSON_IsObject(face)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* center = cJSON_GetObjectItemCaseSensitive(face, "center");
    cJSON* x = cJSON_IsObject(center) ? cJSON_GetObjectItemCaseSensitive(center, "x") : nullptr;
    cJSON* y = cJSON_IsObject(center) ? cJSON_GetObjectItemCaseSensitive(center, "y") : nullptr;
    if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
        target->center_x = static_cast<float>(x->valuedouble);
        target->center_y = static_cast<float>(y->valuedouble);
    } else {
        cJSON* left = cJSON_GetObjectItemCaseSensitive(face, "left");
        cJSON* right = cJSON_GetObjectItemCaseSensitive(face, "right");
        cJSON* top = cJSON_GetObjectItemCaseSensitive(face, "top");
        cJSON* bottom = cJSON_GetObjectItemCaseSensitive(face, "bottom");
        if (!cJSON_IsNumber(left) || !cJSON_IsNumber(right) || !cJSON_IsNumber(top) || !cJSON_IsNumber(bottom)) {
            cJSON_Delete(root);
            return false;
        }
        target->center_x = static_cast<float>((left->valuedouble + right->valuedouble) * 0.5);
        target->center_y = static_cast<float>((top->valuedouble + bottom->valuedouble) * 0.5);
    }
    cJSON* area = cJSON_GetObjectItemCaseSensitive(face, "area");
    target->area = cJSON_IsNumber(area) ? static_cast<float>(area->valuedouble) : 0.0f;
    target->found = true;
    cJSON_Delete(root);
    return true;
}

static bool upload_tracking_frame(const camera_fb_t* frame, FaceTarget* target)
{
    if (frame == nullptr || frame->buf == nullptr || frame->len == 0) {
        set_tracking_status("Capture Fail", "Empty camera frame", "", "", false, false);
        return false;
    }

    std::string upload_url = make_server_url("/upload-image");
    ESP_LOGI(TAG, "Uploading camera frame to %s, len=%u size=%ux%u format=%d", upload_url.c_str(),
             static_cast<unsigned>(frame->len), static_cast<unsigned>(frame->width), static_cast<unsigned>(frame->height),
             static_cast<int>(frame->format));
    set_tracking_status("Uploading", upload_url.c_str(), "Sending RGB565 frame", "");

    esp_http_client_config_t config = {};
    config.url = upload_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 20000;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_tracking_status("Upload Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    char width[16];
    char height[16];
    snprintf(width, sizeof(width), "%u", static_cast<unsigned>(frame->width));
    snprintf(height, sizeof(height), "%u", static_cast<unsigned>(frame->height));
    esp_http_client_set_header(client, "Content-Type", "image/rgb565");
    esp_http_client_set_header(client, "X-Image-Format", "rgb565");
    esp_http_client_set_header(client, "X-Image-Width", width);
    esp_http_client_set_header(client, "X-Image-Height", height);
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "X-Client-Id", client_id);
    esp_http_client_set_header(client, "X-Visual-Tracking", "false");

    esp_err_t err = esp_http_client_open(client, frame->len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera upload open failed: %s", esp_err_to_name(err));
        set_tracking_status("Upload Fail", esp_err_to_name(err), "Check server URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t offset = 0;
    while (offset < frame->len && !tracking_stop_requested) {
        size_t chunk = std::min<size_t>(kHttpBufferSize, frame->len - offset);
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(frame->buf + offset), chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "Camera upload write failed at offset=%u", static_cast<unsigned>(offset));
            set_tracking_status("Upload Fail", "HTTP write failed", "See USB serial log", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        offset += written;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Camera upload HTTP status=%d content_length=%d", status, content_length);

    std::string response;
    char response_chunk[256];
    while (response.size() < 2048) {
        int read_len = esp_http_client_read(client, response_chunk, sizeof(response_chunk) - 1);
        if (read_len <= 0) {
            break;
        }
        response.append(response_chunk, read_len);
    }
    if (!response.empty()) {
        ESP_LOGI(TAG, "Camera upload response: %s", response.c_str());
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (tracking_stop_requested) {
        set_tracking_status("Stopped", "Tracking stopped", "", "", false, false);
        return false;
    }

    if (status >= 200 && status < 300) {
        if (parse_face_target(response, target)) {
            char line1[64];
            snprintf(line1, sizeof(line1), "face %.0f, %.0f", target->center_x, target->center_y);
            set_tracking_status("Detected", line1, "Calculating head angle", "");
            return true;
        }
        set_tracking_status("No Face", "No face detected", "Try facing the camera", "", false, false);
        return false;
    }

    char line[48];
    snprintf(line, sizeof(line), "HTTP status %d", status);
    set_tracking_status("Upload Fail", line, "See USB serial log", "", false, false);
    return false;
}

static bool upload_camera_frame_only(const camera_fb_t* frame)
{
    if (frame == nullptr || frame->buf == nullptr || frame->len == 0) {
        set_camera_status("Capture Fail", "Empty camera frame", "", "", false, false);
        return false;
    }

    std::string upload_url = make_server_url("/upload-image");
    ESP_LOGI(TAG, "Uploading one camera frame to %s, len=%u size=%ux%u format=%d", upload_url.c_str(),
             static_cast<unsigned>(frame->len), static_cast<unsigned>(frame->width),
             static_cast<unsigned>(frame->height), static_cast<int>(frame->format));
    set_camera_status("Uploading", upload_url.c_str(), "Sending RGB565 frame", "");

    esp_http_client_config_t config = {};
    config.url = upload_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 20000;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_camera_status("Upload Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    char width[16];
    char height[16];
    snprintf(width, sizeof(width), "%u", static_cast<unsigned>(frame->width));
    snprintf(height, sizeof(height), "%u", static_cast<unsigned>(frame->height));
    esp_http_client_set_header(client, "Content-Type", "image/rgb565");
    esp_http_client_set_header(client, "X-Image-Format", "rgb565");
    esp_http_client_set_header(client, "X-Image-Width", width);
    esp_http_client_set_header(client, "X-Image-Height", height);
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "X-Client-Id", client_id);

    esp_err_t err = esp_http_client_open(client, frame->len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Single camera upload open failed: %s", esp_err_to_name(err));
        set_camera_status("Upload Fail", esp_err_to_name(err), "Check server URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t offset = 0;
    while (offset < frame->len) {
        size_t chunk = std::min<size_t>(kHttpBufferSize, frame->len - offset);
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(frame->buf + offset), chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "Single camera upload write failed at offset=%u", static_cast<unsigned>(offset));
            set_camera_status("Upload Fail", "HTTP write failed", "See USB serial log", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        offset += written;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Single camera upload HTTP status=%d content_length=%d", status, content_length);

    std::string response;
    char response_chunk[256];
    while (response.size() < 1024) {
        int read_len = esp_http_client_read(client, response_chunk, sizeof(response_chunk) - 1);
        if (read_len <= 0) {
            break;
        }
        response.append(response_chunk, read_len);
    }
    if (!response.empty()) {
        ESP_LOGI(TAG, "Single camera upload response: %s", response.c_str());
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        char line1[64];
        snprintf(line1, sizeof(line1), "uploaded %u bytes", static_cast<unsigned>(frame->len));
        set_camera_status("Done", line1, "Saved on local server", "", false, true);
        return true;
    }

    char line[48];
    snprintf(line, sizeof(line), "HTTP status %d", status);
    set_camera_status("Upload Fail", line, "See USB serial log", "", false, false);
    return false;
}

static bool capture_face_at_pose(const ScanPose& pose, int step_index, int step_count, ScanCandidate* candidate)
{
    if (candidate == nullptr) {
        return false;
    }

    char line1[64];
    char line2[64];
    snprintf(line1, sizeof(line1), "scan %d/%d", step_index, step_count);
    snprintf(line2, sizeof(line2), "yaw %.0f pitch %.0f", pose.yaw, pose.pitch);
    set_tracking_status("Scanning", line1, line2, "");

    if (!move_head_to_tracking_angles(pose.yaw, pose.pitch, 700)) {
        set_tracking_status("Servo Fail", "Scan pose write failed", "Check ID/wiring", "", false, false);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(950));

    if (!init_camera_once()) {
        return false;
    }

    set_tracking_status("Capturing", pose.label, "Detecting face via server", "");
    camera_fb_t* frame = get_fresh_camera_frame("scan");
    if (frame == nullptr) {
        set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        release_camera_driver();
        return false;
    }

    FaceTarget target;
    bool detected = upload_tracking_frame(frame, &target);
    esp_camera_fb_return(frame);
    release_camera_driver();

    if (detected) {
        float dx = target.center_x - kTrackingCx;
        float dy = target.center_y - kTrackingCy;
        candidate->found = true;
        candidate->face = target;
        candidate->pose_yaw = tracking_yaw_deg;
        candidate->pose_pitch = tracking_pitch_deg;
        candidate->pixel_error = std::sqrt(dx * dx + dy * dy);
        ESP_LOGI(TAG, "Scan candidate %s pose=(%.1f,%.1f) face=(%.1f,%.1f) err=%.1f area=%.1f",
                 pose.label, candidate->pose_yaw, candidate->pose_pitch, target.center_x, target.center_y,
                 candidate->pixel_error, target.area);
    } else {
        ESP_LOGI(TAG, "Scan pose %s found no face", pose.label);
    }

    if (!tracking_stop_requested) {
        set_tracking_status("Center", "Returning home", "Next scan/photo follows", "");
        if (!move_center_for_test()) {
            set_tracking_status("Servo Fail", "Center write failed", "Check ID/wiring", "", false, false);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }

    return detected;
}

static bool capture_face_at_current_pose(const char* stage, const char* line1, const char* line2, FaceTarget* target)
{
    if (target == nullptr) {
        return false;
    }
    if (!init_camera_once()) {
        return false;
    }

    set_tracking_status(stage, line1, line2, "");
    camera_fb_t* frame = get_fresh_camera_frame("find_owner");
    if (frame == nullptr) {
        set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        release_camera_driver();
        return false;
    }

    bool detected = upload_tracking_frame(frame, target);
    esp_camera_fb_return(frame);
    return detected;
}

static bool refine_head_toward_face(const FaceTarget& target, uint16_t duration_ms, float gain_x, float gain_y)
{
    float dx = target.center_x - kTrackingCx;
    float dy = target.center_y - kTrackingCy;
    float yaw_delta_deg = std::atan(dx / kTrackingFx) * 180.0f / kPi;
    float pitch_delta_deg = std::atan(dy / kTrackingFy) * 180.0f / kPi;
    float yaw_step = yaw_delta_deg * gain_x * kFindOwnerYawDirection;
    float pitch_step = pitch_delta_deg * gain_y * kFindOwnerPitchDirection;
    float next_yaw = tracking_yaw_deg + yaw_step;
    float next_pitch = tracking_pitch_deg + pitch_step;
    ESP_LOGI(TAG, "Find owner adjust face=(%.1f,%.1f) dx=%.1f dy=%.1f step=(%.1f,%.1f) next=(%.1f,%.1f)",
             target.center_x, target.center_y, dx, dy, yaw_step, pitch_step, next_yaw, next_pitch);
    return move_head_to_tracking_angles(next_yaw, next_pitch, duration_ms);
}

static bool run_find_owner_command(int rounds, const char* reply, float gain_x = kFindOwnerYawGain,
                                   float gain_y = kFindOwnerPitchGain, float stop_pixels = kFindOwnerStopPixels,
                                   bool preserve_speech_playback = false, bool wait_for_speech = false)
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return false;
    }
    if (!ensure_server_selected()) {
        return false;
    }

    if (wait_for_speech) {
        const int max_wait_ms = 3000;
        int waited_ms = 0;
        while ((speak_command_active || speech_playback_active || speak_animation_task_handle != nullptr ||
                (speak_command_queue != nullptr && uxQueueMessagesWaiting(speak_command_queue) > 0)) &&
               waited_ms < max_wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(20));
            waited_ms += 20;
        }
        ESP_LOGI(TAG, "Find owner waited for speech: %dms", waited_ms);
    }

    VoiceListenerPauseGuard voice_pause("find owner");
    if (preserve_speech_playback) {
        ESP_LOGI(TAG, "Find owner preserving speech playback");
    } else {
        request_speak_preempt("find owner");
        M5.Speaker.end();
    }

    int capped_rounds = std::max(1, std::min(rounds, kFindOwnerMaxRounds));
    gain_x = std::max(0.0f, gain_x);
    gain_y = std::max(0.0f, gain_y);
    stop_pixels = std::max(0.0f, stop_pixels);
    bool saw_face = false;
    bool aligned = false;
    for (int round = 1; round <= capped_rounds; ++round) {
        FaceTarget target;
        char line1[48];
        snprintf(line1, sizeof(line1), "round %d/%d", round, capped_rounds);
        if (!capture_face_at_current_pose("Find Owner", line1, "Detecting face", &target)) {
            vTaskDelay(pdMS_TO_TICKS(220));
            continue;
        }

        saw_face = true;
        float dx = target.center_x - kTrackingCx;
        float dy = target.center_y - kTrackingCy;
        float pixel_error = std::sqrt(dx * dx + dy * dy);
        if (pixel_error <= stop_pixels) {
            aligned = true;
            ESP_LOGI(TAG, "Find owner aligned: err=%.1f", pixel_error);
            break;
        }

        if (round == capped_rounds && capped_rounds > 1) {
            ESP_LOGI(TAG, "Find owner reached final capture: err=%.1f, skip final adjustment", pixel_error);
            break;
        }

        if (!refine_head_toward_face(target, 520, gain_x, gain_y)) {
            set_tracking_status("Servo Fail", "Find-owner move failed", "Check ID/wiring", "", false, false);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(650));
    }

    ESP_LOGI(TAG, "Find owner finished: saw_face=%d aligned=%d", saw_face, aligned);
    if (reply != nullptr && reply[0] != '\0') {
        return execute_speak_command_internal(reply, false);
    }
    return saw_face;
}

void run_camera_upload_app()
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return;
    }
    if (!ensure_server_selected()) {
        return;
    }

    VoiceListenerPauseGuard voice_pause("camera upload");
    M5.Speaker.stop();
    M5.Speaker.end();

    set_camera_status("Camera", "Initializing camera", "Taking one photo");
    if (!init_camera_once()) {
        return;
    }

    set_camera_status("Capturing", "Taking photo", "Uploading follows");
    camera_fb_t* frame = get_fresh_camera_frame("capture_image");
    if (frame == nullptr) {
        set_camera_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        release_camera_driver();
        return;
    }

    upload_camera_frame_only(frame);
    esp_camera_fb_return(frame);
    release_camera_driver();
}

void run_tracking_user_demo()
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return;
    }
    if (!ensure_server_selected()) {
        return;
    }

    VoiceListenerPauseGuard voice_pause("tracking demo");
    M5.Speaker.stop();
    M5.Speaker.end();

    set_tracking_status("Servo", "Centering head", "Scan starts next");
    move_center_for_test();
    vTaskDelay(pdMS_TO_TICKS(650));

    const ScanPose scan_poses[kTrackingScanStepCount] = {
        {"Home", 0.0f, kTrackingHomePitchDeg},
        {"Yaw A", kTrackingScanYawDeg, kTrackingHomePitchDeg},
        {"Yaw B", -kTrackingScanYawDeg, kTrackingHomePitchDeg},
        {"Pitch Up", 0.0f, kTrackingHomePitchDeg + kTrackingScanPitchDeltaDeg},
        {"Pitch Down", 0.0f, kTrackingHomePitchDeg - kTrackingScanPitchDeltaDeg},
    };

    ScanCandidate best;
    for (int i = 0; i < kTrackingScanStepCount && !tracking_stop_requested; ++i) {
        ScanCandidate candidate;
        capture_face_at_pose(scan_poses[i], i + 1, kTrackingScanStepCount, &candidate);
        if (!candidate.found) {
            continue;
        }
        if (!best.found || candidate.pixel_error < best.pixel_error ||
            (candidate.pixel_error == best.pixel_error && candidate.face.area > best.face.area)) {
            best = candidate;
        }
    }

    if (tracking_stop_requested) {
        set_tracking_status("Stopped", "Tracking stopped", "", "", false, false);
        return;
    }

    if (!best.found) {
        set_tracking_status("No Face", "No face in scan photos", "Try standing closer", "", false, false);
        move_center_for_test();
        return;
    }

    float dx = best.face.center_x - kTrackingCx;
    float dy = best.face.center_y - kTrackingCy;
    float yaw_delta_deg = std::atan(dx / kTrackingFx) * 180.0f / kPi;
    float pitch_delta_deg = std::atan(dy / kTrackingFy) * 180.0f / kPi;
    float final_yaw = best.pose_yaw + yaw_delta_deg * kTrackingYawGain * kTrackingYawDirection;
    float final_pitch = best.pose_pitch + pitch_delta_deg * kTrackingPitchGain * kTrackingPitchDirection;

    char line1[80];
    char line2[80];
    snprintf(line1, sizeof(line1), "best %.0f,%.0f err %.0f", best.face.center_x, best.face.center_y, best.pixel_error);
    snprintf(line2, sizeof(line2), "move %.1f / %.1f", final_yaw, final_pitch);
    set_tracking_status("Facing", line1, line2, "");
    ESP_LOGI(TAG, "Scan best pose=(%.1f,%.1f) face=(%.1f,%.1f) dx=%.1f dy=%.1f final=(%.1f,%.1f)",
             best.pose_yaw, best.pose_pitch, best.face.center_x, best.face.center_y, dx, dy, final_yaw, final_pitch);
    if (!move_head_to_tracking_angles(final_yaw, final_pitch, 800)) {
        set_tracking_status("Servo Fail", "Final face move failed", "Check ID/wiring", "", false, false);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(900));

    FaceTarget verify_target;
    float verify_error = best.pixel_error;
    for (int round = 1; round <= kTrackingRefineRounds && !tracking_stop_requested; ++round) {
        char verify_line[48];
        snprintf(verify_line, sizeof(verify_line), "refine %d/%d", round, kTrackingRefineRounds);
        if (!capture_face_at_current_pose("Verify", verify_line, "Checking face center", &verify_target)) {
            set_tracking_status("Done", "Moved to best scan pose", "No final face detected", "", false, false);
            return;
        }

        float verify_dx = verify_target.center_x - kTrackingCx;
        float verify_dy = verify_target.center_y - kTrackingCy;
        verify_error = std::sqrt(verify_dx * verify_dx + verify_dy * verify_dy);
        ESP_LOGI(TAG, "Refine %d face=(%.1f,%.1f) dx=%.1f dy=%.1f err=%.1f pose=(%.1f,%.1f)", round,
                 verify_target.center_x, verify_target.center_y, verify_dx, verify_dy, verify_error,
                 tracking_yaw_deg, tracking_pitch_deg);
        if (verify_error <= kTrackingStopPixels || round == kTrackingRefineRounds) {
            snprintf(line1, sizeof(line1), "final err %.0fpx", verify_error);
            snprintf(line2, sizeof(line2), "face %.0f, %.0f", verify_target.center_x, verify_target.center_y);
            set_tracking_status(verify_error <= kTrackingStopPixels ? "Aligned" : "Done", line1, line2, "", false,
                                verify_error <= kTrackingStopPixels);
            return;
        }

        yaw_delta_deg = std::atan(verify_dx / kTrackingFx) * 180.0f / kPi;
        pitch_delta_deg = std::atan(verify_dy / kTrackingFy) * 180.0f / kPi;
        final_yaw = tracking_yaw_deg + yaw_delta_deg * kTrackingYawGain * kTrackingYawDirection;
        final_pitch = tracking_pitch_deg + pitch_delta_deg * kTrackingPitchGain * kTrackingPitchDirection;
        snprintf(line1, sizeof(line1), "face %.0f,%.0f err %.0f", verify_target.center_x, verify_target.center_y,
                 verify_error);
        snprintf(line2, sizeof(line2), "move %.1f / %.1f", final_yaw, final_pitch);
        set_tracking_status("Refining", line1, line2, "");
        if (!move_head_to_tracking_angles(final_yaw, final_pitch, 700)) {
            set_tracking_status("Servo Fail", "Refine move failed", "Check ID/wiring", "", false, false);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(850));
    }
    return;

#if 0
    for (int round = 1; round <= kTrackingMaxRounds && !tracking_stop_requested; ++round) {
        if (!init_camera_once()) {
            return;
        }

        char round_line[48];
        snprintf(round_line, sizeof(round_line), "round %d/%d", round, kTrackingMaxRounds);
        set_tracking_status("Capturing", round_line, "Detecting face via server", "");
        camera_fb_t* frame = get_fresh_camera_frame("tracking_loop");
        if (frame == nullptr) {
            set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
            ESP_LOGE(TAG, "esp_camera_fb_get failed");
            release_camera_driver();
            return;
        }

        if (frame->format != PIXFORMAT_RGB565) {
            ESP_LOGW(TAG, "Unexpected camera pixel format: %d", static_cast<int>(frame->format));
        }

        FaceTarget target;
        bool detected = upload_tracking_frame(frame, &target);
        esp_camera_fb_return(frame);
        release_camera_driver();
        if (!detected) {
            return;
        }

        float dx = target.center_x - kTrackingCx;
        float dy = target.center_y - kTrackingCy;
        float pixel_error = std::sqrt(dx * dx + dy * dy);
        float yaw_delta_deg = std::atan(dx / kTrackingFx) * 180.0f / kPi;
        float pitch_delta_deg = std::atan(dy / kTrackingFy) * 180.0f / kPi;
        float new_yaw = tracking_yaw_deg + yaw_delta_deg * kTrackingGain * kTrackingYawDirection;
        float new_pitch = tracking_pitch_deg + pitch_delta_deg * kTrackingGain * kTrackingPitchDirection;

        char line1[80];
        char line2[80];
        snprintf(line1, sizeof(line1), "err %.0fpx yaw %+0.1f", pixel_error, yaw_delta_deg);
        snprintf(line2, sizeof(line2), "pitch %+0.1f -> %.1f/%.1f", pitch_delta_deg, new_yaw, new_pitch);
        set_tracking_status("Moving", line1, line2, "");
        ESP_LOGI(TAG, "Tracking round=%d face=(%.1f,%.1f) dx=%.1f dy=%.1f yaw_delta=%.2f pitch_delta=%.2f",
                 round, target.center_x, target.center_y, dx, dy, yaw_delta_deg, pitch_delta_deg);

        if (pixel_error <= kTrackingStopPixels) {
            set_tracking_status("Aligned", line1, "Face is near center", "", false, true);
            return;
        }

        if (!move_head_to_tracking_angles(new_yaw, new_pitch, 500)) {
            set_tracking_status("Servo Fail", "SCS0009 write failed", "Check servo power/wiring", "", false, false);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }

    char final_line[64];
    snprintf(final_line, sizeof(final_line), "yaw %.1f pitch %.1f", tracking_yaw_deg, tracking_pitch_deg);
    set_tracking_status("Done", final_line, "Tap again to refine", "", false, true);
#endif
}

static bool play_stream_pcm_chunk(const int16_t* samples, size_t sample_count)
{
    if (sample_count == 0 || app2_stop_requested) {
        return true;
    }
    if (!M5.Speaker.playRaw(samples, sample_count, kTtsStreamSampleRate, false, 1, 0, false)) {
        set_tts_status("Play Fail", "M5.Speaker.playRaw failed", "", "", false, false);
        return false;
    }
    return true;
}

static void drain_speaker_playback()
{
    while (M5.Speaker.isPlaying() && !app2_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!app2_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(kSpeakerDrainMs));
    }
}

static bool stream_pcm_url(const std::string& url, const char* label, bool update_tts_status)
{
    ESP_LOGI(TAG, "Stream PCM URL: %s", url.c_str());
    if (update_tts_status) {
        set_tts_status("Requesting", url.c_str(), label != nullptr ? label : "");
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 60000;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (update_tts_status) {
            set_tts_status("HTTP Fail", "esp_http_client_init failed", "", "", false, false);
        }
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream PCM open failed: %s", esp_err_to_name(err));
        if (update_tts_status) {
            set_tts_status("HTTP Fail", esp_err_to_name(err), "Check PCM URL/IP", "", false, false);
        }
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Stream PCM HTTP status=%d content_length=%d", status, content_length);
    if (status < 200 || status >= 300) {
        if (update_tts_status) {
            char line[48];
            snprintf(line, sizeof(line), "HTTP status %d", status);
            set_tts_status("HTTP Status", line, "Expected PCM stream", "", false, false);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    if (update_tts_status) {
        set_tts_status("Playing", "Receiving pcm_s16le", "16kHz mono stream", "");
    }
    std::vector<std::vector<int16_t>> buffers(3, std::vector<int16_t>(kTtsStreamBufferSamples));
    int buffer_index = 0;
    size_t sample_pos = 0;
    uint8_t pending_byte = 0;
    bool has_pending_byte = false;
    size_t total_bytes = 0;
    uint8_t read_buffer[kHttpBufferSize];

    auto flush_samples = [&]() -> bool {
        if (sample_pos == 0) {
            return true;
        }
        bool ok = play_stream_pcm_chunk(buffers[buffer_index].data(), sample_pos);
        buffer_index = (buffer_index + 1) % buffers.size();
        sample_pos = 0;
        return ok;
    };

    while (!app2_stop_requested) {
        int read_len = esp_http_client_read(client, reinterpret_cast<char*>(read_buffer), sizeof(read_buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Stream PCM read failed");
            if (update_tts_status) {
                set_tts_status("Read Fail", "HTTP body read failed", "", "", false, false);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }
        total_bytes += read_len;

        int offset = 0;
        if (has_pending_byte && read_len > 0) {
            buffers[buffer_index][sample_pos++] = static_cast<int16_t>(pending_byte | (read_buffer[0] << 8));
            has_pending_byte = false;
            offset = 1;
            if (sample_pos == kTtsStreamBufferSamples && !flush_samples()) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }
        }

        while (offset + 1 < read_len) {
            buffers[buffer_index][sample_pos++] =
                static_cast<int16_t>(read_buffer[offset] | (read_buffer[offset + 1] << 8));
            offset += 2;
            if (sample_pos == kTtsStreamBufferSamples && !flush_samples()) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }
        }

        if (offset < read_len) {
            pending_byte = read_buffer[offset];
            has_pending_byte = true;
        }
    }

    if (!app2_stop_requested && !flush_samples()) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    drain_speaker_playback();
    M5.Speaker.stop();

    if (app2_stop_requested) {
        if (update_tts_status) {
            set_tts_status("Stopped", "TTS stopped", "", "", false, false);
        }
        return false;
    }

    if (update_tts_status) {
        char line[64];
        snprintf(line, sizeof(line), "streamed %u bytes", static_cast<unsigned>(total_bytes));
        set_tts_status("Done", line, "Tap TTS to speak again", "", false, true);
    }
    return true;
}

static bool stream_tts_pcm_for_text(const char* text)
{
    std::string url = make_stream_tts_url_for_text(text);
    ESP_LOGI(TAG, "Stream TTS URL: %s", url.c_str());
    return stream_pcm_url(url, text, true);
}

static bool stream_cached_reply_pcm(const char* cache_name, const char* label)
{
    std::string url = make_event_audio_url(cache_name);
    ESP_LOGI(TAG, "Stream cached reply URL: %s", url.c_str());
    return stream_pcm_url(url, label, false);
}

static const char* cached_wake_reply_name_for_text(const char* text)
{
    if (text == nullptr) {
        return "";
    }
    if (strcmp(text, "我在") == 0 || strcmp(text, "我在。") == 0) {
        return "wake_reply";
    }
    if (strcmp(text, "有什么要帮忙的") == 0) {
        return "wake_reply_help";
    }
    if (strcmp(text, "你好呀") == 0) {
        return "wake_reply_hello";
    }
    if (strcmp(text, "我在呢") == 0) {
        return "wake_reply_here";
    }
    if (strcmp(text, "小派在呢") == 0) {
        return "wake_reply_xiaopai_here";
    }
    return "";
}

static bool stream_tts_pcm()
{
    return stream_tts_pcm_for_text(CONFIG_STACKCHAN_TTS_TEXT);
}

void run_stream_tts_demo()
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return;
    }
    if (!ensure_server_selected()) {
        return;
    }

    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();

    if (!M5.Speaker.begin()) {
        set_tts_status("Speaker Fail", "M5.Speaker.begin failed", "", "", false, false);
        ESP_LOGE(TAG, "M5.Speaker.begin failed");
        return;
    }
    apply_speaker_volume();
    stream_tts_pcm();
}

static bool http_get_string(const std::string& url, std::string* response, int timeout_ms)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = timeout_ms;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (response != nullptr) {
        char buffer[512];
        while (response->size() < 4096) {
            int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
            if (read_len <= 0) {
                break;
            }
            response->append(buffer, read_len);
            if (content_length > 0 && static_cast<int>(response->size()) >= content_length) {
                break;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP GET %s -> status=%d content_length=%d response_len=%u", url.c_str(), status, content_length,
             response != nullptr ? static_cast<unsigned>(response->size()) : 0);
    return status >= 200 && status < 300;
}

static double json_number_value(const cJSON* root, const char* key, double default_value)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valuedouble : default_value;
}

static bool json_bool_value(const cJSON* root, const char* key, bool default_value)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(value)) {
        return cJSON_IsTrue(value);
    }
    if (cJSON_IsNumber(value)) {
        return value->valuedouble != 0;
    }
    if (cJSON_IsString(value)) {
        return strcmp(value->valuestring, "1") == 0 || strcmp(value->valuestring, "true") == 0 ||
               strcmp(value->valuestring, "yes") == 0 || strcmp(value->valuestring, "on") == 0;
    }
    return default_value;
}

static bool send_command_ack(const char* cmd_id, const char* status, const char* message = "")
{
    std::string url = make_server_url("/device/ack");
    url += "?device_id=";
    url += url_encode(mac_address().c_str());
    url += "&cmd_id=";
    url += url_encode(cmd_id != nullptr ? cmd_id : "");
    url += "&status=";
    url += url_encode(status != nullptr ? status : "received");
    if (message != nullptr && message[0] != '\0') {
        url += "&message=";
        url += url_encode(message);
    }
    std::string response;
    return http_get_string(url, &response, 5000);
}

static const ExpressionAsset* find_expression_asset(const char* expression)
{
    const char* name = expression != nullptr && expression[0] != '\0' ? expression : kDefaultExpression;
    if (strcmp(name, "listening") == 0 || strcmp(name, "default") == 0 || strcmp(name, "stopped") == 0) {
        name = kDefaultExpression;
    }

    for (const auto& asset : kExpressionAssets) {
        if (strcmp(asset.name, name) == 0) {
            return &asset;
        }
    }

    for (const auto& asset : kExpressionAssets) {
        if (strcmp(asset.name, kDefaultExpression) == 0) {
            return &asset;
        }
    }
    return nullptr;
}

static const ExpressionAnimation* find_expression_animation(const char* expression)
{
    const char* name = expression != nullptr && expression[0] != '\0' ? expression : "";
    for (const auto& animation : kExpressionAnimations) {
        if (strcmp(animation.name, name) == 0) {
            return &animation;
        }
    }
    return nullptr;
}

static void render_expression_frame(const char* expression)
{
    {
        M5Lock lock;
        auto& display = M5.Display;
        const ExpressionAsset* asset = find_expression_asset(expression);
        if (asset != nullptr) {
            const uint32_t image_len = static_cast<uint32_t>(asset->end - asset->start);
            const int draw_x = (display.width() - asset->width) / 2;
            const int draw_y = (display.height() - asset->height) / 2;
            if (!expression_screen_visible || current_expression_asset == nullptr ||
                current_expression_asset->width != asset->width || current_expression_asset->height != asset->height) {
                if (display.drawPng(asset->start, image_len, draw_x, draw_y)) {
                    expression_screen_visible = true;
                    current_expression_asset = asset;
                    return;
                }
            } else if (current_expression_asset != asset) {
                if (display.drawPng(asset->start, image_len, draw_x, draw_y)) {
                    current_expression_asset = asset;
                    return;
                }
            } else {
                return;
            }
        }
        display.fillScreen(TFT_BLACK);
        mark_expression_screen_dirty();
    }
}

static void stop_expression_animation()
{
    expression_animation_running = false;
    for (int i = 0; i < 30 && expression_animation_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    current_expression_animation = nullptr;
}

static void start_expression_animation(const ExpressionAnimation* animation)
{
    if (animation == nullptr || animation->frame_count == 0) {
        return;
    }
    if (expression_animation_task_handle != nullptr && current_expression_animation == animation) {
        return;
    }
    stop_expression_animation();
    expression_animation_running = true;
    current_expression_animation = animation;
    xTaskCreatePinnedToCore([](void* arg) {
        const auto* animation = static_cast<const ExpressionAnimation*>(arg);
        size_t frame = 0;
        while (expression_animation_running && animation != nullptr && animation->frame_count > 0) {
            render_expression_frame(animation->frames[frame]);
            frame = (frame + 1) % animation->frame_count;
            vTaskDelay(pdMS_TO_TICKS(animation->frame_ms));
        }
        expression_animation_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "expr_anim", 4 * 1024, const_cast<ExpressionAnimation*>(animation), 2, &expression_animation_task_handle, 0);
}

static void show_expression(const char* expression)
{
    const ExpressionAnimation* animation = find_expression_animation(expression);
    if (animation != nullptr) {
        start_expression_animation(animation);
        return;
    }
    stop_expression_animation();
    render_expression_frame(expression);
}

static void start_speaking_animation()
{
    speak_animation_running = true;
    if (speak_animation_task_handle != nullptr) {
        return;
    }
    xTaskCreatePinnedToCore([](void*) {
        bool open = false;
        while (speak_animation_running) {
            show_expression(open ? "speak2" : "speak1");
            open = !open;
            vTaskDelay(pdMS_TO_TICKS(160));
        }
        speak_animation_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "speak_anim", 4 * 1024, nullptr, 2, &speak_animation_task_handle, 0);
}

static void stop_speaking_animation()
{
    speak_animation_running = false;
    for (int i = 0; i < 30 && speak_animation_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (speech_expression_overridden) {
        speech_expression_overridden = false;
        return;
    }
    show_expression(kDefaultExpression);
}

static void request_speak_preempt(const char* reason)
{
    app2_stop_requested = true;
    M5.Speaker.stop();
    speak_animation_running = false;
    ESP_LOGI(TAG, "Speak preempt requested: %s", reason != nullptr ? reason : "unspecified");
}

static bool execute_speak_command_internal(const char* text, bool pause_voice_listener, const char* cache_name)
{
    if (text == nullptr || text[0] == '\0') {
        return true;
    }

    VoiceListenerPauseGuard* voice_pause = pause_voice_listener ? new VoiceListenerPauseGuard("speak command") : nullptr;

    if (audio_mutex != nullptr) {
        xSemaphoreTake(audio_mutex, portMAX_DELAY);
    }
    if (!M5.Speaker.begin()) {
        ESP_LOGE(TAG, "M5.Speaker.begin failed for command speak");
        if (audio_mutex != nullptr) {
            xSemaphoreGive(audio_mutex);
        }
        delete voice_pause;
        return false;
    }
    apply_speaker_volume();
    app2_stop_requested = false;
    M5.Speaker.stop();
    speech_playback_active = true;
    speech_expression_overridden = false;
    start_speaking_animation();
    const char* selected_cache_name = cache_name != nullptr && cache_name[0] != '\0'
                                          ? cache_name
                                          : cached_wake_reply_name_for_text(text);
    bool ok = selected_cache_name[0] != '\0' ? stream_cached_reply_pcm(selected_cache_name, text)
                                             : stream_tts_pcm_for_text(text);
    M5.Speaker.end();
    stop_speaking_animation();
    speech_playback_active = false;
    if (audio_mutex != nullptr) {
        xSemaphoreGive(audio_mutex);
    }
    delete voice_pause;
    return ok;
}

static bool execute_speak_command(const char* text)
{
    return execute_speak_command_internal(text, true);
}

static bool enqueue_speak_command(const char* cmd_id, const char* text, const char* cache_name, bool pause_voice_listener)
{
    if (speak_command_queue == nullptr || text == nullptr || text[0] == '\0') {
        return false;
    }

    SpeakCommandItem item = {};
    snprintf(item.cmd_id, sizeof(item.cmd_id), "%s", cmd_id != nullptr ? cmd_id : "");
    snprintf(item.text, sizeof(item.text), "%s", text);
    snprintf(item.cache_name, sizeof(item.cache_name), "%s", cache_name != nullptr ? cache_name : "");
    item.pause_voice_listener = pause_voice_listener;
    request_speak_preempt("new speak command");
    return xQueueOverwrite(speak_command_queue, &item) == pdPASS;
}

static void run_speak_command_loop()
{
    while (true) {
        SpeakCommandItem item = {};
        if (xQueueReceive(speak_command_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        app2_stop_requested = false;
        speak_command_active = true;
        bool ok = execute_speak_command_internal(item.text, item.pause_voice_listener, item.cache_name);
        speak_command_active = false;
        send_command_ack(item.cmd_id, ok ? "done" : "failed", ok ? "" : "speak command interrupted or failed");
    }
}

static void start_speak_command_service()
{
    if (speak_command_queue == nullptr) {
        speak_command_queue = xQueueCreate(1, sizeof(SpeakCommandItem));
    }
    if (speak_command_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create speak command queue");
        return;
    }
    if (speak_command_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_speak_command_loop();
        }, "speak_cmd", kSpeakCommandTaskStackBytes, nullptr, 2, &speak_command_task_handle, 0);
    }
}

static bool execute_volume_command(const cJSON* payload)
{
    std::string mode = json_string_value(payload, "mode");
    std::string direction = json_string_value(payload, "direction");
    if (direction.empty()) {
        direction = json_string_value(payload, "action");
    }
    if (direction.empty()) {
        direction = json_string_value(payload, "type");
    }

    int step = static_cast<int>(json_number_value(payload, "step", kSpeakerVolumeDefaultStep));
    if (step <= 0) {
        step = kSpeakerVolumeDefaultStep;
    }

    const cJSON* value = cJSON_GetObjectItemCaseSensitive(payload, "value");
    if (mode == "set" || cJSON_IsNumber(value)) {
        speaker_volume_percent = static_cast<int>(json_number_value(payload, "value", speaker_volume_percent));
    } else if (direction == "down" || direction == "lower" || direction == "small" || direction == "quiet") {
        speaker_volume_percent -= step;
    } else {
        speaker_volume_percent += step;
    }
    speaker_volume_percent =
        std::max(kSpeakerVolumePercentMin, std::min(kSpeakerVolumePercentMax, speaker_volume_percent));

    char reply[64];
    snprintf(reply, sizeof(reply), "已经将声音调到%d", speaker_volume_percent);
    ESP_LOGI(TAG, "Speaker volume adjusted: percent=%d raw=%d", speaker_volume_percent,
             speaker_volume_raw_from_percent(speaker_volume_percent));
    if (!execute_speak_command(reply)) {
        ESP_LOGW(TAG, "Speaker volume reply failed after adjustment");
    }
    return true;
}

static const char* head_touch_event_name(HeadTouchEvent event)
{
    switch (event) {
        case HeadTouchEvent::Press:
            return "press";
        case HeadTouchEvent::Click:
            return "click";
        case HeadTouchEvent::SwipeForward:
            return "swipe_forward";
        case HeadTouchEvent::SwipeBackward:
            return "swipe_backward";
    }
    return "press";
}

static void enqueue_head_touch_event(HeadTouchEvent event)
{
    if (head_touch_event_queue == nullptr) {
        return;
    }
    xQueueSend(head_touch_event_queue, &event, 0);
    ESP_LOGI(TAG, "Head touch event: %s", head_touch_event_name(event));
}

static bool si12t_write_reg(uint8_t reg, uint8_t value)
{
    M5Lock lock;
    return M5.In_I2C.writeRegister8(kSi12tAddress, reg, value, kSi12tI2cFreq);
}

static bool si12t_read_reg(uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return false;
    }
    M5Lock lock;
    if (!M5.In_I2C.scanID(kSi12tAddress, kSi12tI2cFreq)) {
        return false;
    }
    *value = M5.In_I2C.readRegister8(kSi12tAddress, reg, kSi12tI2cFreq);
    return true;
}

static bool init_head_touch_sensor()
{
    {
        M5Lock lock;
        if (!M5.In_I2C.scanID(kSi12tAddress, kSi12tI2cFreq)) {
            ESP_LOGW(TAG, "Si12T head touch sensor not found at 0x%02x", kSi12tAddress);
            return false;
        }
    }

    bool ok = true;
    ok &= si12t_write_reg(kSi12tRefRst1Reg, 0x00);
    ok &= si12t_write_reg(kSi12tRefRst2Reg, 0x00);
    ok &= si12t_write_reg(kSi12tChHold1Reg, 0x00);
    ok &= si12t_write_reg(kSi12tChHold2Reg, 0x00);
    ok &= si12t_write_reg(kSi12tCalHold1Reg, 0x00);
    ok &= si12t_write_reg(kSi12tCalHold2Reg, 0x00);
    ok &= si12t_write_reg(kSi12tCtrl2Reg, 0x0f);
    ok &= si12t_write_reg(kSi12tCtrl2Reg, 0x07);
    ok &= si12t_write_reg(kSi12tCtrl1Reg, 0x22);
    for (uint8_t reg = kSi12tSensitivity1Reg; reg < kSi12tSensitivity1Reg + 5; ++reg) {
        ok &= si12t_write_reg(reg, 0xcc);
    }
    ESP_LOGI(TAG, "Si12T head touch init %s", ok ? "ok" : "failed");
    return ok;
}

static bool read_head_touch_intensities(uint8_t intensities[3])
{
    uint8_t raw = 0;
    if (!si12t_read_reg(kSi12tOutput1Reg, &raw)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        intensities[(3 - 1) - i] = (raw >> (i * 2)) & 0x03;
    }
    return true;
}

static bool head_touch_is_touched(const uint8_t intensities[3])
{
    return intensities[0] > 0 || intensities[1] > 0 || intensities[2] > 0;
}

static int16_t head_touch_position(const uint8_t intensities[3])
{
    uint16_t total = intensities[0] + intensities[1] + intensities[2];
    if (total == 0) {
        return 0;
    }
    int32_t weighted = static_cast<int32_t>(intensities[0]) * -100 + static_cast<int32_t>(intensities[2]) * 100;
    return static_cast<int16_t>(weighted / total);
}

static void run_head_touch_audio_loop()
{
    while (true) {
        HeadTouchEvent event = HeadTouchEvent::Press;
        if (xQueueReceive(head_touch_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Head touch local expression: %s -> shy", head_touch_event_name(event));
            show_expression("shy");
        }
    }
}

static void run_head_touch_loop()
{
    if (!init_head_touch_sensor()) {
        head_touch_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    bool was_touched = false;
    bool swipe_reported = false;
    int16_t initial_position = 0;
    int64_t touch_started_us = 0;
    uint8_t intensities[3] = {};

    while (true) {
        if (camera_owns_internal_i2c) {
            vTaskDelay(pdMS_TO_TICKS(kHeadTouchPollMs));
            continue;
        }

        if (!read_head_touch_intensities(intensities)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool touched = head_touch_is_touched(intensities);
        int16_t position = head_touch_position(intensities);
        int64_t now_us = esp_timer_get_time();

        if (touched && !was_touched) {
            was_touched = true;
            swipe_reported = false;
            initial_position = position;
            touch_started_us = now_us;
        } else if (touched && was_touched && !swipe_reported) {
            int16_t delta = position - initial_position;
            if (delta > kHeadTouchSwipeThreshold) {
                swipe_reported = true;
                enqueue_head_touch_event(HeadTouchEvent::SwipeBackward);
            } else if (delta < -kHeadTouchSwipeThreshold) {
                swipe_reported = true;
                enqueue_head_touch_event(HeadTouchEvent::SwipeForward);
            }
        } else if (!touched && was_touched) {
            uint32_t held_ms = static_cast<uint32_t>((now_us - touch_started_us) / 1000);
            if (!swipe_reported && held_ms <= kHeadTouchClickMaxMs) {
                enqueue_head_touch_event(HeadTouchEvent::Click);
            } else if (!swipe_reported) {
                enqueue_head_touch_event(HeadTouchEvent::Press);
            }
            was_touched = false;
        }

        vTaskDelay(pdMS_TO_TICKS(kHeadTouchPollMs));
    }
}

static void start_head_touch_services()
{
    if (head_touch_event_queue == nullptr) {
        head_touch_event_queue = xQueueCreate(8, sizeof(HeadTouchEvent));
    }
    if (head_touch_event_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create head touch event queue");
        return;
    }
    if (head_touch_audio_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_head_touch_audio_loop();
        }, "headtouch_audio", kHeadTouchAudioTaskStackBytes, nullptr, 2, &head_touch_audio_task_handle, 0);
    }
    if (head_touch_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_head_touch_loop();
        }, "headtouch", kHeadTouchTaskStackBytes, nullptr, 2, &head_touch_task_handle, 1);
    }
}

static bool execute_command_object(const cJSON* command)
{
    if (!cJSON_IsObject(command)) {
        return false;
    }

    std::string type = json_string_value(command, "type");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    if (!cJSON_IsObject(payload) && !cJSON_IsArray(payload)) {
        payload = command;
    }

    if (type == "face") {
        std::string expression = json_string_value(payload, "expression");
        if (speak_command_active || speak_animation_running || speech_playback_active) {
            speech_expression_overridden = true;
            speak_animation_running = false;
        }
        show_expression(expression.empty() ? kDefaultExpression : expression.c_str());
        return true;
    }
    if (type == "speak") {
        std::string text = json_string_value(payload, "text");
        std::string cache_name = json_string_value(payload, "cache_name");
        return execute_speak_command_internal(text.c_str(), false, cache_name.c_str());
    }
    if (type == "volume" || type == "sound") {
        return execute_volume_command(payload);
    }
    if (type == "capture_image" || type == "track_once" || type == "camera") {
        run_camera_upload_app();
        return true;
    }
    if (type == "find_owner" || type == "locate_owner") {
        int rounds = static_cast<int>(json_number_value(payload, "rounds", kFindOwnerMaxRounds));
        float gain_x = static_cast<float>(json_number_value(payload, "gain_x", kFindOwnerYawGain));
        float gain_y = static_cast<float>(json_number_value(payload, "gain_y", kFindOwnerPitchGain));
        float stop_pixels = static_cast<float>(json_number_value(payload, "stop_pixels", kFindOwnerStopPixels));
        const cJSON* reply_item = cJSON_GetObjectItemCaseSensitive(payload, "reply");
        std::string reply = cJSON_IsString(reply_item) ? reply_item->valuestring : "";
        if (!cJSON_IsString(reply_item)) {
            reply = "我在";
        }
        bool preserve_speech = json_bool_value(payload, "preserve_speech", false);
        bool wait_for_speech = json_bool_value(payload, "wait_for_speech", false);
        return run_find_owner_command(rounds, reply.c_str(), gain_x, gain_y, stop_pixels, preserve_speech,
                                      wait_for_speech);
    }
    if (type == "motion" || type == "move") {
        std::string motion_type = json_string_value(payload, "type");
        if (motion_type == "motion" || motion_type == "move") {
            motion_type.clear();
        }
        if (motion_type.empty()) {
            motion_type = json_string_value(payload, "action");
        }
        if (motion_type.empty()) {
            motion_type = json_string_value(payload, "direction");
        }
        float degree = static_cast<float>(json_number_value(payload, "degree", json_number_value(payload, "degrees", 15)));
        float pan = static_cast<float>(json_number_value(payload, "pan", tracking_yaw_deg));
        float tilt = static_cast<float>(json_number_value(payload, "tilt", tracking_pitch_deg));
        int duration_ms = static_cast<int>(json_number_value(payload, "duration_ms", 500));

        if (motion_type == "left") {
            pan = tracking_yaw_deg - degree;
            tilt = tracking_pitch_deg;
        } else if (motion_type == "right") {
            pan = tracking_yaw_deg + degree;
            tilt = tracking_pitch_deg;
        } else if (motion_type == "up") {
            pan = tracking_yaw_deg;
            tilt = tracking_pitch_deg + degree;
        } else if (motion_type == "down") {
            pan = tracking_yaw_deg;
            tilt = tracking_pitch_deg - degree;
        } else if (motion_type == "center" || motion_type == "home") {
            pan = 0.0f;
            tilt = kTrackingHomePitchDeg;
        }
        return move_head_to_tracking_angles(pan, tilt, duration_ms);
    }
    if (type == "sequence") {
        if (!cJSON_IsArray(payload)) {
            return false;
        }
        const int count = cJSON_GetArraySize(payload);
        for (int i = 0; i < count; ++i) {
            const cJSON* step = cJSON_GetArrayItem(payload, i);
            if (!execute_command_object(step)) {
                return false;
            }
        }
        return true;
    }
    if (type == "stop") {
        app2_stop_requested = true;
        M5.Speaker.stop();
        show_expression("stopped");
        return true;
    }
    if (type == "play_audio") {
        ESP_LOGW(TAG, "play_audio command is not implemented yet");
        return false;
    }

    ESP_LOGW(TAG, "Unknown command type: %s", type.c_str());
    return false;
}

static bool handle_command_response(const std::string& response)
{
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Command response is not JSON: %s", response.c_str());
        return false;
    }

    std::string response_type = json_string_value(root, "type");
    if (response_type == "noop") {
        cJSON_Delete(root);
        return true;
    }
    if (response_type != "command") {
        ESP_LOGW(TAG, "Unexpected command response type: %s", response_type.c_str());
        cJSON_Delete(root);
        return false;
    }

    const cJSON* command = cJSON_GetObjectItemCaseSensitive(root, "command");
    std::string cmd_id = json_string_value(command, "cmd_id");
    std::string cmd_type = json_string_value(command, "type");
    ESP_LOGI(TAG, "Command received: id=%s type=%s", cmd_id.c_str(), cmd_type.c_str());
    send_command_ack(cmd_id.c_str(), "received");

    if (cmd_type == "speak") {
        const cJSON* payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
        if (!cJSON_IsObject(payload)) {
            payload = command;
        }
        std::string text = json_string_value(payload, "text");
        std::string cache_name = json_string_value(payload, "cache_name");
        bool pause_listener = json_bool_value(payload, "pause_listener", false) ||
                              json_bool_value(payload, "pause_voice_listener", false);
        bool queued = enqueue_speak_command(cmd_id.c_str(), text.c_str(), cache_name.c_str(), pause_listener);
        if (!queued) {
            send_command_ack(cmd_id.c_str(), "failed", "speak queue unavailable or empty text");
        }
        cJSON_Delete(root);
        return queued;
    }

    if (cmd_type == "stop") {
        request_speak_preempt("stop command");
    }

    bool ok = execute_command_object(command);
    char message[96] = {};
    if (!ok) {
        snprintf(message, sizeof(message), "command execution failed: %s", cmd_type.c_str());
    }
    send_command_ack(cmd_id.c_str(), ok ? "done" : "failed", message);
    cJSON_Delete(root);
    return ok;
}

static void run_command_http_loop()
{
    ensure_client_id();
    while (!wifi_is_connected() || !active_server_selected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    show_expression(kDefaultExpression);

    while (true) {
        std::string url = make_server_url("/device/next-command");
        url += "?device_id=";
        url += url_encode(mac_address().c_str());
        url += "&timeout=25";
        std::string response;
        if (http_get_string(url, &response, 35000) && !response.empty()) {
            handle_command_response(response);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void start_background_services()
{
    current_app = AppId::WifiConnect;
    voice_status_screen_suppressed = false;

    if (boot_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void*) {
        ensure_client_id();
        while (!ensure_network_ready()) {
            ESP_LOGW(TAG, "WiFi or server not ready, retrying in 3 seconds");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        show_expression(kDefaultExpression);
        enable_servo_power();
        if (init_camera_once()) {
            ESP_LOGI(TAG, "Camera pre-initialized for find-owner flow");
        } else {
            ESP_LOGW(TAG, "Camera pre-initialization failed; will retry on demand");
        }
        current_app = AppId::VoiceDemo;
        voice_status_screen_suppressed = true;
        start_head_touch_services();
        start_speak_command_service();

        if (xiaozhi_task_handle == nullptr) {
            app1_stop_requested = false;
            xTaskCreatePinnedToCore([](void*) {
                run_xiaozhi_ota_probe();
                xiaozhi_task_handle = nullptr;
            }, "bg_voice", kApp1TaskStackBytes, nullptr, 3, &xiaozhi_task_handle, 1);
        }

        if (command_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                run_command_http_loop();
                command_task_handle = nullptr;
            }, "bg_command", kCommandTaskStackBytes, nullptr, 3, &command_task_handle, 0);
        }

        boot_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "bg_boot", kWifiTaskStackBytes, nullptr, 4, &boot_task_handle, 1);

}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs_once());
    force_core_s3_display_board();
    m5_mutex = xSemaphoreCreateMutex();
    audio_mutex = xSemaphoreCreateMutex();
    auto cfg = M5.config();
    cfg.internal_mic = true;
    cfg.internal_spk = true;
    M5.begin(cfg);
    configure_speaker_for_tts();

    M5.Display.setBrightness(180);
    M5.Display.setRotation(1);
    M5.Touch.setHoldThresh(500);
    M5.Touch.setFlickThresh(12);

    start_background_services();

    while (true) {
        {
            M5Lock lock;
            if (!camera_owns_internal_i2c) {
                M5.update();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
