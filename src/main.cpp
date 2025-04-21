#include <Arduino.h>
#include <esp_wifi.h>
#include <soc/rtc_cntl_reg.h>
#include <IotWebConf.h>
#include <ESPmDNS.h>
#include "FS.h"
#include "SPIFFS.h"
#include <WiFi.h>
#include "VideoFrameProvider.h" 
#include "rtsp_server_video.h"  
#include <format_duration.h>
#include <format_number.h>
#include <moustache.h>
#include <settings.h>

// HTML files
extern const char index_html_min_start[] asm("_binary_html_index_min_html_start");

// Parameter values storage
char param_frame_duration_value[12]; // Enough for a number up to 9999
char param_video_quality_value[4];   // Enough for a number up to 100

// Parameter groups and parameters - fixed to use standard IotWebConf parameters
iotwebconf::ParameterGroup param_group_video("video", "Video settings");
// Fix: Using the correct constructor signature for NumberParameter
iotwebconf::NumberParameter param_frame_duration("fd", "Frame duration (ms)", 
                                               param_frame_duration_value, 
                                               sizeof(param_frame_duration_value));
iotwebconf::NumberParameter param_video_quality("q", "Video quality", 
                                              param_video_quality_value, 
                                              sizeof(param_video_quality_value));

// Video Frame Provider
VideoFrameProvider videoProvider;

// DNS Server
DNSServer dnsServer;

// RTSP Server
std::unique_ptr<rtsp_server_video> video_server;

// Web server
WebServer web_server(80);

// Create thing name with unique identifier
auto thingName = String(WIFI_SSID) + "-" + String(ESP.getEfuseMac(), 16);
IotWebConf iotWebConf(thingName.c_str(), &dnsServer, &web_server, WIFI_PASSWORD, CONFIG_VERSION);

// Initialization result
esp_err_t video_init_result = ESP_OK;

void handle_root()
{
  log_v("Handle root");
  // Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
    return;

  // Format hostname
  auto hostname = "esp32-" + WiFi.macAddress() + ".local";
  hostname.replace(":", "");
  hostname.toLowerCase();

  // Wifi Modes
  const char *wifi_modes[] = {"NULL", "STA", "AP", "STA+AP"};
  auto ipv4 = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIP() : WiFi.localIP();
  auto ipv6 = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIPv6() : WiFi.localIPv6();

  // Get numeric values from the parameter strings
  unsigned long frameDuration = DEFAULT_FRAME_DURATION;
  byte videoQuality = DEFAULT_JPEG_QUALITY;
  
  // Parse the parameters from strings to numbers
  if (strlen(param_frame_duration_value) > 0) {
    frameDuration = atol(param_frame_duration_value);
  }
  
  if (strlen(param_video_quality_value) > 0) {
    videoQuality = atoi(param_video_quality_value);
  }

  moustache_variable_t substitutions[] = {
      // Version / CPU
      {"AppTitle", APP_TITLE},
      {"AppVersion", APP_VERSION},
      {"BoardType", BOARD_NAME},
      {"ThingName", iotWebConf.getThingName()},
      {"SDKVersion", ESP.getSdkVersion()},
      {"ChipModel", ESP.getChipModel()},
      {"ChipRevision", String(ESP.getChipRevision())},
      {"CpuFreqMHz", String(ESP.getCpuFreqMHz())},
      {"CpuCores", String(ESP.getChipCores())},
      {"FlashSize", format_memory(ESP.getFlashChipSize(), 0)},
      {"HeapSize", format_memory(ESP.getHeapSize())},
      {"PsRamSize", format_memory(ESP.getPsramSize(), 0)},
      // Diagnostics
      {"Uptime", String(format_duration(millis() / 1000))},
      {"FreeHeap", format_memory(ESP.getFreeHeap())},
      {"MaxAllocHeap", format_memory(ESP.getMaxAllocHeap())},
      {"NumRTSPSessions", video_server != nullptr ? String(video_server->num_connected()) : "RTSP server disabled"},
      // Network
      {"HostName", hostname},
      {"MacAddress", WiFi.macAddress()},
      {"AccessPoint", WiFi.SSID()},
      {"SignalStrength", String(WiFi.RSSI())},
      {"WifiMode", wifi_modes[WiFi.getMode()]},
      {"IPv4", ipv4.toString()},
      {"IPv6", ipv6.toString()},
      {"NetworkState.ApMode", String(iotWebConf.getState() == iotwebconf::NetworkState::ApMode)},
      {"NetworkState.OnLine", String(iotWebConf.getState() == iotwebconf::NetworkState::OnLine)},
      // Video
      {"FrameDuration", String(frameDuration)},
      {"FrameFrequency", String(1000.0 / frameDuration, 1)},
      {"VideoQuality", String(videoQuality)},
      {"VideoInitialized", String(video_init_result == ESP_OK)},
      // RTSP
      {"RtspPort", String(RTSP_PORT)}
  };

  web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  auto html = moustache_render(index_html_min_start, substitutions);
  web_server.send(200, "text/html", html);
}

void handle_snapshot()
{
  log_v("handle_snapshot");
  if (video_init_result != ESP_OK)
  {
    web_server.send(404, "text/plain", "Video provider is not initialized");
    return;
  }

  // Get a frame from our video provider
  auto fb = videoProvider.getFrame();
  if (fb == nullptr)
  {
    web_server.send(404, "text/plain", "Unable to obtain frame from the video provider");
    return;
  }

  web_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  web_server.setContentLength(fb->len);
  web_server.send(200, "image/jpeg", "");
  web_server.sendContent((const char*)fb->buf, fb->len);
  
  // Return the frame buffer
  videoProvider.returnFrame(fb);
}

#define STREAM_CONTENT_BOUNDARY "123456789000000000000987654321"

void handle_stream()
{
  log_v("handle_stream");
  if (video_init_result != ESP_OK)
  {
    web_server.send(404, "text/plain", "Video provider is not initialized");
    return;
  }

  log_v("starting streaming");
  // Blocks further handling of HTTP server until stopped
  char size_buf[12];
  auto client = web_server.client();
  client.write("HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: multipart/x-mixed-replace; boundary=" STREAM_CONTENT_BOUNDARY "\r\n");
  
  while (client.connected())
  {
    // Get a frame
    auto fb = videoProvider.getFrame();
    if (fb) {
      client.write("\r\n--" STREAM_CONTENT_BOUNDARY "\r\n");
      client.write("Content-Type: image/jpeg\r\nContent-Length: ");
      sprintf(size_buf, "%d\r\n\r\n", fb->len);
      client.write(size_buf);
      client.write((const char*)fb->buf, fb->len);
      
      // Return the frame buffer
      videoProvider.returnFrame(fb);
    }
    
    // Short delay to control frame rate
    delay(10);
  }

  log_v("client disconnected");
  client.stop();
  log_v("stopped streaming");
}

bool initialize_video_provider()
{
  log_v("initialize_video_provider");
  unsigned long frameDuration = DEFAULT_FRAME_DURATION;
  
  // Parse the frame duration from string to number
  if (strlen(param_frame_duration_value) > 0) {
    frameDuration = atol(param_frame_duration_value);
  }
  
  log_i("Frame duration: %lu ms", frameDuration);
  
  // Initialize SPIFFS if not already initialized
  if (!SPIFFS.begin(true)) {
    log_e("Failed to initialize SPIFFS");
    return false;
  }
  
  // Initialize the video provider
  if (!videoProvider.init("/video_frames.bin", frameDuration)) {
    log_e("Failed to initialize video provider");
    return false;
  }
  
  return true;
}

void start_rtsp_server()
{
  log_v("start_rtsp_server");
  unsigned long frameDuration = DEFAULT_FRAME_DURATION;
  
  // Parse the frame duration from string to number
  if (strlen(param_frame_duration_value) > 0) {
    frameDuration = atol(param_frame_duration_value);
  }
  
  video_server = std::unique_ptr<rtsp_server_video>(new rtsp_server_video(videoProvider, frameDuration, RTSP_PORT));
  // Add RTSP service to mDNS
  // HTTP is already set by iotWebConf
  MDNS.addService("rtsp", "tcp", RTSP_PORT);
}

void on_connected()
{
  log_v("on_connected");
  // Start the RTSP Server if initialized
  if (video_init_result == ESP_OK)
    start_rtsp_server();
  else
    log_e("Not starting RTSP server: video provider not initialized");
}

void on_config_saved()
{
  log_v("on_config_saved");
  // Parse the frame duration from string to number
  float fps = 10.0f; // Default
  if (strlen(param_frame_duration_value) > 0) {
    unsigned long frameDuration = atol(param_frame_duration_value);
    if (frameDuration > 0) {
      fps = 1000.0f / frameDuration;
    }
  }
  
  // Update settings
  videoProvider.setFps(fps);
}

void setup()
{
  // Disable brownout
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.setDebugOutput(true);

#ifdef ARDUINO_USB_CDC_ON_BOOT
  // Delay for USB to connect/settle
  delay(5000);
#endif

  log_i("Core debug level: %d", CORE_DEBUG_LEVEL);
  log_i("CPU Freq: %d Mhz, %d core(s)", getCpuFrequencyMhz(), ESP.getChipCores());
  log_i("Free heap: %d bytes", ESP.getFreeHeap());
  log_i("SDK version: %s", ESP.getSdkVersion());
  log_i("Board: %s", BOARD_NAME);
  log_i("Starting " APP_TITLE "...");

  if (!psramInit()) {
    log_e("Failed to initialize PSRAM");
  }

  // Initialize SPIFFS for video frames
  if (!SPIFFS.begin(true)) {
    log_e("Failed to mount SPIFFS");
  }

  // Set default values
  snprintf(param_frame_duration_value, sizeof(param_frame_duration_value), "%lu", DEFAULT_FRAME_DURATION);
  snprintf(param_video_quality_value, sizeof(param_video_quality_value), "%d", DEFAULT_JPEG_QUALITY);
  
  // Add parameters to the group
  param_group_video.addItem(&param_frame_duration);
  param_group_video.addItem(&param_video_quality);
  iotWebConf.addParameterGroup(&param_group_video);

  iotWebConf.getApTimeoutParameter()->visible = true;
  iotWebConf.setConfigSavedCallback(on_config_saved);
  iotWebConf.setWifiConnectionCallback(on_connected);
  iotWebConf.init();

  // Initialize video provider
  video_init_result = initialize_video_provider() ? ESP_OK : ESP_FAIL;
  if (video_init_result != ESP_OK) {
    log_e("Failed to initialize video provider");
  }

  // Set up required URL handlers on the web server
  web_server.on("/", HTTP_GET, handle_root);
  web_server.on("/config", []
                { iotWebConf.handleConfig(); });
  // Video snapshot
  web_server.on("/snapshot", HTTP_GET, handle_snapshot);
  // Video stream
  web_server.on("/stream", HTTP_GET, handle_stream);

  web_server.onNotFound([]()
                        { iotWebConf.handleNotFound(); });
}

void loop()
{
  iotWebConf.doLoop();

  if (video_server)
    video_server->doLoop();
}