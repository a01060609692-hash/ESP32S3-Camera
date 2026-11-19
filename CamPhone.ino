// ESP32-S3 N16R8 + OV5640
// Wi-Fi setup on first boot (AP), then normal STA connection.
// Camera GPIO pins deliberately kept identical to the working original.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "ESP32_OV5640_AF.h"  // Arduino library installed by build.yml

// IMPORTANT: Before publishing the device, change this setup AP password.
const char* AP_NAME = "S3-OV5640-CAM";
const char* AP_PASSWORD = "S3cam2026!";

static bool cameraReady = false;
static bool usingSetupAP = false;
static httpd_handle_t controlServer = nullptr;
static httpd_handle_t streamServer = nullptr;
static OV5640 autofocus;

static const char PAGE[] = R"PAGE(
<!doctype html><html lang="ko"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 OV5640</title>
<style>body{font:16px sans-serif;max-width:780px;margin:auto;padding:15px}
img{width:100%;height:auto;background:#111}button,select{padding:9px;margin:5px}
a{margin-right:12px}</style></head><body>
<h2>ESP32-S3 OV5640 Camera</h2>
<p><a href="/capture" target="_blank">사진 촬영</a>
<a id="full" href="#">전체화면 스트림</a><a href="/wifi">Wi-Fi 설정</a></p>
<img id="live" alt="camera stream">
<p>화면 좌우 반전 <select id="mirror"><option value="0">꺼짐</option><option value="1">켜짐</option></select></p>
<p>해상도 <select id="res">
<option value="vga">VGA 640x480</option>
<option value="svga" selected>SVGA 800x600</option>
<option value="xga">XGA 1024x768</option>
<option value="uxga">UXGA 1600x1200</option>
<option value="qsxga">QSXGA 2592x1944 (사진용, 스트림 느림)</option>
</select></p>
<p>JPEG 품질 <select id="quality"><option value="10" selected>높음 (10)</option><option value="12">보통 (12)</option><option value="14">속도 우선 (14)</option></select></p>
<button onclick="applySettings()">설정 적용</button>
<p style="font-size:13px;color:#666">AF 렌즈가 장착된 경우 부팅 시 연속 AF 초기화를 시도합니다.</p>
<script>
const stream='http://'+location.hostname+':81/stream';
document.getElementById('full').href=stream;
const live=document.getElementById('live');
live.src=stream;
async function applySettings(){
 live.removeAttribute('src');
 const q=new URLSearchParams({mirror:document.getElementById('mirror').value,
 res:document.getElementById('res').value,quality:document.getElementById('quality').value});
 try { const r=await fetch('/control?'+q); if(!r.ok)throw Error('HTTP '+r.status);
 await new Promise(resolve=>setTimeout(resolve,500)); live.src=stream+'?t='+Date.now(); }
 catch(e){alert('설정 오류: '+e.message);live.src=stream;}
}
</script></body></html>
)PAGE";

static const char WIFI_PAGE[] = R"PAGE(
<!doctype html><html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>카메라 Wi-Fi 설정</title></head><body style="font:16px sans-serif;padding:18px">
<h2>카메라 Wi-Fi 설정</h2>
<p>인터넷 공유기의 2.4GHz Wi-Fi 이름과 비밀번호를 입력하세요.</p>
<form method="POST" action="/wifi" accept-charset="utf-8">
<p>Wi-Fi 이름(SSID):<br><input name="ssid" maxlength="32" required style="font-size:18px"></p>
<p>Wi-Fi 비밀번호:<br><input type="password" name="pass" maxlength="63" style="font-size:18px"></p>
<button type="submit" style="font-size:18px">저장하고 재시작</button></form>
<p>저장 후 카메라가 공유기에 접속하면 <b>S3-OV5640-CAM</b> Wi-Fi는 사라지고,
카메라 주소는 공유기가 부여한 IP로 변경됩니다.</p>
<p>연결 실패 시 설정용 카메라 Wi-Fi가 다시 나타납니다.</p>
<p><a href="/">카메라 화면으로</a></p></body></html>
)PAGE";

static String urlDecode(const String& s) {
  String o;
  for (size_t i=0; i<s.length(); ++i) {
    char c=s[i];
    if (c=='+') o+=' ';
    else if (c=='%' && i+2<s.length()) {
      auto hex=[](char h)->int { if(h>='0'&&h<='9')return h-'0';
        if(h>='A'&&h<='F')return h-'A'+10;
        if(h>='a'&&h<='f')return h-'a'+10; return -1; };
      int hi=hex(s[i+1]), lo=hex(s[i+2]);
      if (hi>=0 && lo>=0) {o+=(char)((hi<<4)|lo); i+=2;} else o+=c;
    } else o+=c;
  }
  return o;
}
static String formValue(const String& body, const String& key) {
  int start=0;
  while(start<(int)body.length()) {
    int end=body.indexOf('&',start); if(end<0) end=body.length();
    int eq=body.indexOf('=',start);
    if(eq>=start && eq<end && body.substring(start,eq)==key)
      return urlDecode(body.substring(eq+1,end));
    start=end+1;
  }
  return "";
}

static esp_err_t homeHandler(httpd_req_t* req) {
  httpd_resp_set_type(req,"text/html; charset=utf-8");
  if (!cameraReady) return httpd_resp_send(req,
    "<h2>Camera initialization failed</h2><p>Check camera wiring.</p><a href='/wifi'>Wi-Fi settings</a>",
    HTTPD_RESP_USE_STRLEN);
  return httpd_resp_send(req,PAGE,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t captureHandler(httpd_req_t* req) {
  if(!cameraReady) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Camera not ready");
  camera_fb_t* fb=esp_camera_fb_get();
  if(!fb) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Capture failed");
  httpd_resp_set_type(req,"image/jpeg");
  httpd_resp_set_hdr(req,"Content-Disposition","inline; filename=capture.jpg");
  esp_err_t e=httpd_resp_send(req,(const char*)fb->buf,fb->len);
  esp_camera_fb_return(fb);
  return e;
}
static esp_err_t settingsHandler(httpd_req_t* req) {
  if(!cameraReady) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Camera not ready");
  char query[160]={}; char val[24]={};
  if(httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK)
    return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Missing settings");
  sensor_t* s=esp_camera_sensor_get();
  if(!s) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"No sensor");
  if(httpd_query_key_value(query,"mirror",val,sizeof(val))==ESP_OK)
    s->set_hmirror(s,atoi(val)?1:0);
  if(httpd_query_key_value(query,"quality",val,sizeof(val))==ESP_OK) {
    int q=atoi(val); if(q>=6&&q<=30) s->set_quality(s,q);
  }
  if(httpd_query_key_value(query,"res",val,sizeof(val))==ESP_OK) {
    framesize_t f=FRAMESIZE_SVGA;
    if(strcmp(val,"vga")==0)f=FRAMESIZE_VGA;
    else if(strcmp(val,"xga")==0)f=FRAMESIZE_XGA;
    else if(strcmp(val,"uxga")==0)f=FRAMESIZE_UXGA;
    else if(strcmp(val,"qsxga")==0)f=FRAMESIZE_QSXGA;
    s->set_framesize(s,f);
  }
  httpd_resp_set_type(req,"text/plain; charset=utf-8");
  return httpd_resp_sendstr(req,"OK");
}
static esp_err_t wifiFormHandler(httpd_req_t* req) {
  httpd_resp_set_type(req,"text/html; charset=utf-8");
  return httpd_resp_send(req,WIFI_PAGE,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t wifiSaveHandler(httpd_req_t* req) {
  if(req->content_len<=0 || req->content_len>512)
    return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid form length");
  char body[513]; size_t done=0; size_t total=(size_t)req->content_len;
  while(done<total) {
    int n=httpd_req_recv(req,body+done,total-done);
    if(n<=0) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Form read failed");
    done+=(size_t)n;
  }
  body[done]='\0';
  String form(body), ssid=formValue(form,"ssid"), pass=formValue(form,"pass");
  if(ssid.length()<1 || ssid.length()>32 || pass.length()>63 || (pass.length()>0&&pass.length()<8))
    return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Check SSID/password length");
  Preferences p;
  if(!p.begin("camwifi",false))
    return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Storage failed");
  p.putString("ssid",ssid); p.putString("pass",pass); p.end();
  httpd_resp_set_type(req,"text/html; charset=utf-8");
  httpd_resp_sendstr(req,"<h3>Saved. Camera is rebooting...</h3><p>Reconnect your phone to your normal Wi-Fi. Find camera IP in your router or serial log.</p>");
  delay(1200);
  ESP.restart();
  return ESP_OK;
}
static esp_err_t streamHandler(httpd_req_t* req) {
  if(!cameraReady) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Camera not ready");
  httpd_resp_set_type(req,"multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req,"Cache-Control","no-store");
  while(true) {
    camera_fb_t* fb=esp_camera_fb_get(); if(!fb)break;
    char h[120];
    int len=snprintf(h,sizeof(h),"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",(unsigned)fb->len);
    esp_err_t e=httpd_resp_send_chunk(req,h,len);
    if(e==ESP_OK)e=httpd_resp_send_chunk(req,(const char*)fb->buf,fb->len);
    if(e==ESP_OK)e=httpd_resp_send_chunk(req,"\r\n",2);
    esp_camera_fb_return(fb);
    if(e!=ESP_OK)break;
    vTaskDelay(pdMS_TO_TICKS(60));
  }
  return ESP_OK;
}
static void registerHandler(httpd_handle_t server,const char* uri,httpd_method_t method,
                            esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t u={}; u.uri=uri;u.method=method;u.handler=handler;
  httpd_register_uri_handler(server,&u);
}
static void startWeb() {
  httpd_config_t config=HTTPD_DEFAULT_CONFIG();
  config.server_port=80; config.max_uri_handlers=8;
  if(httpd_start(&controlServer,&config)==ESP_OK) {
    registerHandler(controlServer,"/",HTTP_GET,homeHandler);
    registerHandler(controlServer,"/capture",HTTP_GET,captureHandler);
    registerHandler(controlServer,"/control",HTTP_GET,settingsHandler);
    registerHandler(controlServer,"/wifi",HTTP_GET,wifiFormHandler);
    registerHandler(controlServer,"/wifi",HTTP_POST,wifiSaveHandler);
  }
  config.server_port=81;config.ctrl_port=32769;
  if(httpd_start(&streamServer,&config)==ESP_OK)
    registerHandler(streamServer,"/stream",HTTP_GET,streamHandler);
}
static void connectWifiOrSetupAP() {
  Preferences p; String ssid, pass;
  if(p.begin("camwifi",true)) {
    ssid=p.getString("ssid","");pass=p.getString("pass","");p.end();
  }
  if(ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(),pass.c_str());
    Serial.printf("Connecting to Wi-Fi SSID: %s\n",ssid.c_str());
    unsigned long start=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<20000)delay(400);
    if(WiFi.status()==WL_CONNECTED) {
      Serial.print("CAMERA URL: http://");Serial.print(WiFi.localIP());Serial.println("/");
      return;
    }
    Serial.println("STA failed. Falling back to setup AP.");
    WiFi.disconnect(true);delay(250);
  }
  usingSetupAP=true;
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(AP_NAME,AP_PASSWORD))Serial.println("Setup AP failed");
  else Serial.println("Setup Wi-Fi: S3-OV5640-CAM / http://192.168.4.1/wifi");
}
void setup() {
  Serial.begin(115200);delay(300);
  camera_config_t c={};
  c.ledc_channel=LEDC_CHANNEL_0;c.ledc_timer=LEDC_TIMER_0;
  // IMPORTANT: verified working pin mapping, do not change
  c.pin_d0=11;c.pin_d1=9;c.pin_d2=8;c.pin_d3=10;
  c.pin_d4=12;c.pin_d5=18;c.pin_d6=17;c.pin_d7=16;
  c.pin_xclk=15;c.pin_pclk=13;c.pin_vsync=6;c.pin_href=7;
  c.pin_sccb_sda=4;c.pin_sccb_scl=5;c.pin_pwdn=-1;c.pin_reset=-1;
  c.xclk_freq_hz=20000000;
  c.pixel_format=PIXFORMAT_JPEG;
  c.frame_size=psramFound()?FRAMESIZE_SVGA:FRAMESIZE_VGA;
  c.jpeg_quality=10;
  c.fb_count=psramFound()?2:1;
  c.fb_location=psramFound()?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM;
  c.grab_mode=psramFound()?CAMERA_GRAB_LATEST:CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t err=esp_camera_init(&c);
  cameraReady=(err==ESP_OK);
  if(!cameraReady)Serial.printf("Camera init failed: 0x%x\n",err);
  else {
    Serial.println("Camera initialized");
    sensor_t* s=esp_camera_sensor_get();
    if(s) {
      s->set_hmirror(s,0); // If image is still reversed, use UI switch
      s->set_quality(s,10);
      if(autofocus.start(s)) {
        uint8_t fi=autofocus.focusInit();
        Serial.printf("AF firmware init status: %u (0=success)\n",fi);
        if(fi==0) {
          uint8_t fm=autofocus.autoFocusMode();
          Serial.printf("AF continuous mode status: %u (0=success)\n",fm);
        }
      } else Serial.println("AF sensor is not supported");
    }
  }
  connectWifiOrSetupAP();
  startWeb();
}
void loop(){delay(1000);}
