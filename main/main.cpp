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

extern const uint8_t happy_face_png_start[] asm("_binary_happy_face_png_start");
extern const uint8_t happy_face_png_end[] asm("_binary_happy_face_png_end");

namespace {

enum class AppId {
    Launcher,
    WifiConnect,
    VoiceDemo,
    StreamTtsDemo,
    CameraUpload,
    TrackingUser,
};

static constexpr const char* TAG = "StackChan";
static constexpr int kWifiConnectedBit = BIT0;
static constexpr int kWifiFailedBit = BIT1;
static constexpr int kHttpBufferSize = 4096;
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
static constexpr int kTtsStreamSampleRate = 16000;
static constexpr size_t kTtsStreamBufferSamples = 2048;
static constexpr int kLauncherAppCount = 5;
static constexpr uint32_t kWifiTaskStackBytes = 8 * 1024;
static constexpr uint32_t kApp1TaskStackBytes = 24 * 1024;
static constexpr uint32_t kApp2TaskStackBytes = 16 * 1024;
static constexpr uint32_t kCommandTaskStackBytes = 16 * 1024;
static constexpr uint32_t kCameraTaskStackBytes = 16 * 1024;
static constexpr uint32_t kTrackingTaskStackBytes = 20 * 1024;
static constexpr size_t kTtsMaxBytes = CONFIG_STACKCHAN_TTS_MAX_BYTES;
static constexpr int kCameraWidth = 320;
static constexpr int kCameraHeight = 240;
static constexpr float kTrackingCx = 160.0f;
static constexpr float kTrackingCy = 120.0f;
static constexpr float kTrackingFx = 364.0f;
static constexpr float kTrackingFy = 364.0f;
static constexpr float kTrackingYawGain = 0.75f;
static constexpr float kTrackingPitchGain = 0.90f;
static constexpr float kTrackingYawDirection = 1.0f;
static constexpr float kTrackingPitchDirection = -1.0f;
static constexpr int kTrackingScanStepCount = 5;
static constexpr int kTrackingRefineRounds = 3;
static constexpr float kTrackingStopPixels = 16.0f;
static constexpr float kTrackingYawMinDeg = -75.0f;
static constexpr float kTrackingYawMaxDeg = 75.0f;
static constexpr float kTrackingPitchMinDeg = 0.0f;
static constexpr float kTrackingPitchMaxDeg = 90.0f;
static constexpr float kTrackingHomePitchDeg = 45.0f;
static constexpr float kTrackingScanYawDeg = 25.0f;
static constexpr float kTrackingScanPitchDeltaDeg = 20.0f;
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

AppId current_app = AppId::Launcher;
int selected_menu = 0;
TaskHandle_t wifi_task_handle = nullptr;
TaskHandle_t xiaozhi_task_handle = nullptr;
TaskHandle_t stream_tts_task_handle = nullptr;
TaskHandle_t camera_upload_task_handle = nullptr;
TaskHandle_t tracking_task_handle = nullptr;
TaskHandle_t command_task_handle = nullptr;
TaskHandle_t boot_task_handle = nullptr;
EventGroupHandle_t wifi_event_group = nullptr;
SemaphoreHandle_t m5_mutex = nullptr;
int wifi_retry_count = 0;
bool wifi_started = false;
volatile bool wifi_manual_switching = false;
volatile bool app1_stop_requested = false;
volatile bool app2_stop_requested = false;
volatile bool tracking_stop_requested = false;
volatile bool voice_listener_paused = false;
volatile bool voice_status_screen_suppressed = false;
bool camera_initialized = false;
volatile bool camera_owns_internal_i2c = false;
bool servo_uart_initialized = false;
float tracking_yaw_deg = 0.0f;
float tracking_pitch_deg = kTrackingHomePitchDeg;
char client_id[37] = {};
std::string active_wifi_ssid = CONFIG_STACKCHAN_WIFI_SSID;
std::string active_server_base = "http://192.168.21.15:8091";
bool active_server_selected = false;

struct WifiCandidate {
    const char* ssid;
    const char* password;
};

static constexpr WifiCandidate kWifiCandidates[] = {
    {CONFIG_STACKCHAN_WIFI_SSID, CONFIG_STACKCHAN_WIFI_PASSWORD},
    {"myendless", "88888888"},
};

static constexpr const char* kServerBaseCandidates[] = {
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

static constexpr const char* kDefaultExpression = "happy";
static const ExpressionAsset kExpressionAssets[] = {
    {"happy", happy_face_png_start, happy_face_png_end, 320, 240},
};

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

void draw_header(const char* title)
{
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

void draw_app1_status()
{
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
    if (voice_status_screen_suppressed) {
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
    if (voice_status_screen_suppressed) {
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
    set_wifi_status("Starting", "Connecting WiFi");
    if (wifi_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            ::run_wifi_connect_app();
            wifi_task_handle = nullptr;
            vTaskDelete(nullptr);
        }, "app0_wifi", kWifiTaskStackBytes, nullptr, 3, &wifi_task_handle, 1);
    } else {
        set_wifi_status("Running", "WiFi task is already running");
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
        if (wifi_is_connected()) {
            set_wifi_status("Connected", active_wifi_ssid.c_str(), "WiFi SSID connected", "", false, true);
        } else {
            start_wifi_connect_task();
        }
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
    esp_err_t err = nvs_open("stackchan", NVS_READWRITE, &nvs_handle);
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
        ESP_LOGI(TAG, "WiFi STA start, connecting to SSID '%s'", active_wifi_ssid.c_str());
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event ? event->reason : -1);
        if (wifi_manual_switching) {
            return;
        }
        if (wifi_retry_count++ < 5) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "Retry WiFi connection (%d/5)", wifi_retry_count);
        } else {
            xEventGroupSetBits(wifi_event_group, kWifiFailedBit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
    }
}

static bool configure_wifi_candidate(const WifiCandidate& candidate)
{
    if (candidate.ssid == nullptr || strlen(candidate.ssid) == 0) {
        return false;
    }
    active_wifi_ssid = candidate.ssid;

    wifi_config_t wifi_config = {};
    snprintf(reinterpret_cast<char*>(wifi_config.sta.ssid), sizeof(wifi_config.sta.ssid), "%s", candidate.ssid);
    snprintf(reinterpret_cast<char*>(wifi_config.sta.password), sizeof(wifi_config.sta.password), "%s",
             candidate.password ? candidate.password : "");
    wifi_config.sta.threshold.authmode =
        candidate.password && strlen(candidate.password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed for %s: %s", candidate.ssid, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool wifi_is_connected()
{
    if (!wifi_started) {
        return false;
    }

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        if (wifi_event_group != nullptr) {
            xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
        }
        return true;
    }

    if (wifi_event_group != nullptr) {
        EventBits_t bits = xEventGroupGetBits(wifi_event_group);
        return (bits & kWifiConnectedBit) != 0;
    }
    return false;
}

static bool ensure_wifi_connected(bool allow_connect)
{
    if (wifi_event_group == nullptr) {
        wifi_event_group = xEventGroupCreate();
    }

    if (wifi_is_connected()) {
        set_current_network_status("WiFi OK", "Already connected", active_wifi_ssid.c_str(), "", false, true);
        return true;
    }

    if (!allow_connect) {
        set_current_network_status("Need WiFi", "Open WiFi Connect", active_wifi_ssid.c_str(), "", false, false);
        ESP_LOGW(TAG, "WiFi is not connected; network app will not reconnect automatically");
        return false;
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

    if (!wifi_started) {
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                                            nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                                            nullptr));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    for (const WifiCandidate& candidate : kWifiCandidates) {
        if (candidate.ssid == nullptr || strlen(candidate.ssid) == 0) {
            continue;
        }
        set_current_network_status("WiFi", "Connecting...", candidate.ssid);
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
            active_server_selected = false;
            set_current_network_status("WiFi OK", "Connected", active_wifi_ssid.c_str());
            return true;
        }
        ESP_LOGW(TAG, "WiFi candidate failed: %s", candidate.ssid);
    }

    set_current_network_status("WiFi Fail", "Could not connect", "Check SSID/password", "", false, false);
    ESP_LOGE(TAG, "WiFi connection failed or timed out");
    return false;
}

static bool ensure_wifi_connected()
{
    return ensure_wifi_connected(false);
}

void run_wifi_connect_app()
{
    ensure_client_id();
    if (ensure_wifi_connected(true)) {
        set_wifi_status("Connected", active_wifi_ssid.c_str(), "WiFi SSID connected", "", false, true);
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

    for (const char* base : kServerBaseCandidates) {
        if (base == nullptr || strlen(base) == 0) {
            continue;
        }
        set_current_network_status("Server", "Testing local server", base);
        if (http_health_ok(base)) {
            active_server_base = base;
            active_server_selected = true;
            set_current_network_status("Server OK", active_server_base.c_str(), "Using this endpoint", "", false, true);
            return true;
        }
    }

    active_server_selected = false;
    set_current_network_status("Server Fail", "No local server reached", "Check IP/port 8091", "", false, false);
    return false;
}

static bool ensure_network_ready()
{
    if (!ensure_wifi_connected(true)) {
        return false;
    }
    return ensure_server_selected();
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
             "\"board\":{\"type\":\"stackchan-m5unified-demo\",\"name\":\"StackChan Probe\"}}",
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

    std::string ua = std::string("stackchan-m5unified/") + esp_app_get_description()->version;
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
    esp_transport_ws_set_user_agent(ws, "stackchan-m5unified");

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
    config.timeout_ms = 20000;
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
    while (!app1_stop_requested && elapsed_ms < kRecordMaxMs && pcm.size() < max_samples) {
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

        if (smooth_level >= kVoiceStartThreshold) {
            ESP_LOGI(TAG, "Voice threshold triggered: level=%ld smooth=%ld start=%d stop=%d pre_roll=%ums sample_rate=%d",
                     static_cast<long>(level), static_cast<long>(smooth_level), kVoiceStartThreshold,
                     kVoiceStopThreshold, kPreRollMs, kRecordSampleRate);
            auto pcm = record_pcm_after_trigger(pre_roll, smooth_level);
            if (!pcm.empty() && !app1_stop_requested) {
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
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
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
    M5Lock lock;
    uint8_t version = M5.In_I2C.readRegister8(kPy32Address, 0x02, kPy32I2cFreq);
    if (version == 0 || version == 0xff) {
        ESP_LOGW(TAG, "PY32 IO expander not detected; servo power may already be on");
        return;
    }
    py32_write_bit(0x03, 0x04, kPy32ServoPowerPin, true);  // direction output
    py32_write_bit(0x0b, 0x0c, kPy32ServoPowerPin, false); // pull-down off
    py32_write_bit(0x09, 0x0a, kPy32ServoPowerPin, true);  // pull-up on
    py32_write_bit(0x05, 0x06, kPy32ServoPowerPin, true);  // power on
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
    camera_fb_t* frame = esp_camera_fb_get();
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
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) {
        set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        release_camera_driver();
        return false;
    }

    bool detected = upload_tracking_frame(frame, target);
    esp_camera_fb_return(frame);
    release_camera_driver();
    return detected;
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

    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();
    M5.Speaker.stop();
    M5.Speaker.end();

    set_camera_status("Camera", "Initializing camera", "Taking one photo");
    if (!init_camera_once()) {
        return;
    }

    set_camera_status("Capturing", "Taking photo", "Uploading follows");
    camera_fb_t* frame = esp_camera_fb_get();
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

    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();
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
        camera_fb_t* frame = esp_camera_fb_get();
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

static bool stream_tts_pcm_for_text(const char* text)
{
    std::string url = make_stream_tts_url_for_text(text);
    ESP_LOGI(TAG, "Stream TTS URL: %s", url.c_str());
    set_tts_status("Requesting", make_server_url("/stream-speak").c_str(), text != nullptr ? text : "");

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 60000;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_tts_status("HTTP Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream TTS open failed: %s", esp_err_to_name(err));
        set_tts_status("HTTP Fail", esp_err_to_name(err), "Check stream TTS URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Stream TTS HTTP status=%d content_length=%d", status, content_length);
    if (status < 200 || status >= 300) {
        char line[48];
        snprintf(line, sizeof(line), "HTTP status %d", status);
        set_tts_status("HTTP Status", line, "Expected PCM stream", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    set_tts_status("Playing", "Receiving pcm_s16le", "16kHz mono stream", "");
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
            ESP_LOGE(TAG, "Stream TTS read failed");
            set_tts_status("Read Fail", "HTTP body read failed", "", "", false, false);
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

    while (M5.Speaker.isPlaying() && !app2_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    M5.Speaker.stop();

    if (app2_stop_requested) {
        set_tts_status("Stopped", "TTS stopped", "", "", false, false);
        return false;
    }

    char line[64];
    snprintf(line, sizeof(line), "streamed %u bytes", static_cast<unsigned>(total_bytes));
    set_tts_status("Done", line, "Tap TTS to speak again", "", false, true);
    return true;
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
    M5.Speaker.setVolume(CONFIG_STACKCHAN_TTS_VOLUME);
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

static void show_expression(const char* expression)
{
    {
        M5Lock lock;
        auto& display = M5.Display;
        const ExpressionAsset* asset = find_expression_asset(expression);
        if (asset != nullptr) {
            display.fillScreen(TFT_BLACK);
            const uint32_t image_len = static_cast<uint32_t>(asset->end - asset->start);
            if (display.drawPng(asset->start, image_len,
                                (display.width() - asset->width) / 2, (display.height() - asset->height) / 2)) {
                return;
            }
        }
        display.fillScreen(TFT_BLACK);
    }
}

static bool execute_speak_command(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return true;
    }

    voice_listener_paused = true;
    vTaskDelay(pdMS_TO_TICKS(160));
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();

    if (!M5.Speaker.begin()) {
        ESP_LOGE(TAG, "M5.Speaker.begin failed for command speak");
        voice_listener_paused = false;
        return false;
    }
    M5.Speaker.setVolume(CONFIG_STACKCHAN_TTS_VOLUME);
    app2_stop_requested = false;
    bool ok = stream_tts_pcm_for_text(text);
    M5.Speaker.end();
    voice_listener_paused = false;
    return ok;
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
        show_expression(expression.empty() ? "happy" : expression.c_str());
        return true;
    }
    if (type == "speak") {
        std::string text = json_string_value(payload, "text");
        return execute_speak_command(text.c_str());
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
    bool ok = execute_command_object(command);
    send_command_ack(cmd_id.c_str(), ok ? "done" : "failed");
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
    current_app = AppId::VoiceDemo;
    voice_status_screen_suppressed = true;
    show_expression(kDefaultExpression);
    set_app1_status("Booting", "Connecting WiFi", "Starting background services", "", true, false);

    if (boot_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void*) {
        ensure_client_id();
        while (!ensure_network_ready()) {
            set_app1_status("Retrying", "WiFi or server not ready", "Will retry in 3 seconds", "", true, false);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        show_expression(kDefaultExpression);

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
    auto cfg = M5.config();
    cfg.internal_mic = true;
    cfg.internal_spk = true;
    M5.begin(cfg);

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
