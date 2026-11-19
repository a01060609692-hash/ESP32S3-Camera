// ESP32-S3 N16R8 + OV5640: phone-only local Wi-Fi camera demo.
// IMPORTANT: pin mapping is for ESP32-S3-EYE-compatible generic CAM boards.
// Check your board schematic before flashing.
#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

const char* AP_NAME = "S3-OV5640-CAM";
const char* AP_PASSWORD = "S3cam2026!"; // Change this before sharing the device.

static bool cameraReady = false;
static httpd_handle_t controlServer = nullptr;
static httpd_handle_t streamServer = nullptr;

static esp_err_t homeHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  if (!cameraReady) {
    return httpd_resp_send(req,
      "<h2>Camera initialization failed</h2><p>Check camera ribbon and GPIO pin map.</p>",
      HTTPD_RESP_USE_STRLEN);
  }
  return httpd_resp_send(req,
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>S3 OV5640 Camera</title></head><body style='font-family:sans-serif;max-width:800px;margin:auto;padding:12px'>"
    "<h2>ESP32-S3 OV5640 Camera</h2>"
    "<p><a href='/capture'>Take photo</a> | <a href='http://192.168.4.1:81/stream'>Full-screen stream</a></p>"
    "<img src='http://192.168.4.1:81/stream' alt='Live camera' style='width:100%;height:auto'>"
    "</body></html>",
    HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captureHandler(httpd_req_t* req) {
  if (!cameraReady) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera not ready");
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t result = httpd_resp_send(req, reinterpret_cast<const char*>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return result;
}

static esp_err_t streamHandler(httpd_req_t* req) {
  if (!cameraReady) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera not ready");
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) break;
    char header[120];
    int headerLen = snprintf(header, sizeof(header),
       "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
       static_cast<unsigned>(fb->len));
    esp_err_t result = httpd_resp_send_chunk(req, header, headerLen);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf), fb->len);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);
    if (result != ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(60));
  }
  return ESP_OK;
}

static void startWeb() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  if (httpd_start(&controlServer, &config) == ESP_OK) {
    httpd_uri_t home = {};
    home.uri = "/";
    home.method = HTTP_GET;
    home.handler = homeHandler;
    httpd_register_uri_handler(controlServer, &home);
    httpd_uri_t capture = {};
    capture.uri = "/capture";
    capture.method = HTTP_GET;
    capture.handler = captureHandler;
    httpd_register_uri_handler(controlServer, &capture);
  }
  config.server_port = 81;
  config.ctrl_port = 32769;
  if (httpd_start(&streamServer, &config) == ESP_OK) {
    httpd_uri_t stream = {};
    stream.uri = "/stream";
    stream.method = HTTP_GET;
    stream.handler = streamHandler;
    httpd_register_uri_handler(streamServer, &stream);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  // ESP32-S3-EYE compatible generic N16R8 CAM pinout
  c.pin_d0 = 11; c.pin_d1 = 9; c.pin_d2 = 8; c.pin_d3 = 10;
  c.pin_d4 = 12; c.pin_d5 = 18; c.pin_d6 = 17; c.pin_d7 = 16;
  c.pin_xclk = 15;
  c.pin_pclk = 13;
  c.pin_vsync = 6;
  c.pin_href = 7;
  c.pin_sccb_sda = 4;
  c.pin_sccb_scl = 5;
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA; // Start with a stable 640 x 480 stream.
  c.jpeg_quality = 14;
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t status = esp_camera_init(&c);
  cameraReady = (status == ESP_OK);
  if (!cameraReady) Serial.printf("Camera init failed: 0x%x\n", status);
  else Serial.println("Camera initialized");

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_NAME, AP_PASSWORD)) {
    Serial.println("Wi-Fi access point failed");
    return;
  }
  startWeb();
  Serial.println("SSID: S3-OV5640-CAM");
  Serial.println("Web URL: http://192.168.4.1/");
}

void loop() {
  delay(1000);
}
