#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include "wifi_manager.h"

static Preferences prefs;
static WebServer server(80);
static DNSServer dnsServer;

static const char* AP_NAME="TARS-SETUP";
static const char* AP_PASSWORD="12345678";
static const uint32_t WIFI_TIMEOUT_MS=15000;
static const uint8_t WIFI_MAX_ATTEMPTS=3;

static String savedSSID,savedPassword;
static bool portalRunning=false,wifiReady=false,routesRegistered=false;

/* ================= HTML ================= */

static String htmlPage(){
  return "<!DOCTYPE html><html><head>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>TARS WiFi</title>"
         "<style>"
         "body{font-family:Arial;text-align:center;padding:30px;background:#111;color:#fff}"
         "input{box-sizing:border-box;width:90%;max-width:360px;padding:12px;margin:8px 0;font-size:16px}"
         "button{padding:12px 30px;font-size:16px}"
         "</style></head><body>"
         "<h1>TARS</h1><p>WiFi Setup</p>"
         "<form action='/save' method='POST'>"
         "<input name='ssid' placeholder='Nama WiFi' required>"
         "<input name='password' type='password' placeholder='Password WiFi'>"
         "<br><button type='submit'>SIMPAN</button></form>"
         "<p>Hubungkan HP ke <b>TARS-SETUP</b></p>"
         "<p>Password: <b>12345678</b></p>"
         "<p>Buka <b>192.168.4.1</b></p>"
         "</body></html>";
}

/* ================= PORTAL ================= */

static void registerPortalRoutes(){
  if(routesRegistered)return;

  server.on("/",HTTP_GET,[](){
    server.send(200,"text/html",htmlPage());
  });

  server.on("/save",HTTP_POST,[](){

    String ssid=server.arg("ssid");
    String pass=server.arg("password");

    ssid.trim();
    pass.trim();

    if(!ssid.length()){
      server.send(400,"text/plain","SSID kosong");
      return;
    }

    Serial.println("TARS: SAVING WIFI CREDENTIALS");

    prefs.begin("wifi",false);
    prefs.putString("ssid",ssid);
    prefs.putString("pass",pass);
    prefs.putBool("setupDone",true);
    prefs.putBool("pending",true);
    prefs.end();

    savedSSID=ssid;
    savedPassword=pass;

    server.send(
      200,
      "text/html",
      "<!DOCTYPE html><html><body style='font-family:Arial;text-align:center;padding:30px'>"
      "<h2>TARS</h2><p>WiFi tersimpan.</p>"
      "<p>TARS sedang restart...</p></body></html>"
    );

    delay(700);
    ESP.restart();
  });

  server.onNotFound([](){
    server.send(200,"text/html",htmlPage());
  });

  routesRegistered=true;
}

static void startPortal(){

  if(portalRunning)return;

  Serial.println();
  Serial.println("========================================");
  Serial.println("       TARS WIFI SETUP MODE");
  Serial.println("========================================");
  Serial.println("TARS: Connect HP to TARS-SETUP");
  Serial.println("TARS: Password = 12345678");
  Serial.println("TARS: Open http://192.168.4.1");
  Serial.println("========================================");

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME,AP_PASSWORD);

  IPAddress ip=WiFi.softAPIP();

  Serial.print("TARS: AP IP = ");
  Serial.println(ip);

  dnsServer.start(53,"*",ip);
  registerPortalRoutes();
  server.begin();

  portalRunning=true;
}

static void handlePortal(){
  if(!portalRunning)return;

  dnsServer.processNextRequest();
  server.handleClient();
}

static void portalWaitLoop(){
  if(!portalRunning)return;

  Serial.println("TARS: WAITING FOR WIFI CONFIGURATION...");

  while(portalRunning){
    handlePortal();
    delay(2);
    yield();
  }
}

/* ================= BEGIN ================= */

bool wifiManagerBegin(){

  WiFi.persistent(false);

  esp_reset_reason_t reason=esp_reset_reason();

  prefs.begin("wifi",false);

  savedSSID=prefs.getString("ssid","");
  savedPassword=prefs.getString("pass","");
  bool setupDone=prefs.getBool("setupDone",false);
  bool pending=prefs.getBool("pending",false);

  prefs.end();

  if(!setupDone||!savedSSID.length()){

    Serial.println();
    Serial.println("TARS: WIFI SETUP REQUIRED");

    startPortal();
    portalWaitLoop();

    return false;
  }

  if(reason!=ESP_RST_POWERON&&pending){

    Serial.println("TARS: WIFI SETUP RESTART DETECTED");
    Serial.print("TARS: SAVED WIFI = ");
    Serial.println(savedSSID);

  }else{

    Serial.print("TARS: SAVED WIFI = ");
    Serial.println(savedSSID);
  }

  return true;
}

/* ================= CONNECT ================= */

bool wifiManagerConnect(bool requireTime){
  (void)requireTime;

  if(WiFi.status()==WL_CONNECTED){
    wifiReady=true;
    return true;
  }

  if(!savedSSID.length()){

    prefs.begin("wifi",true);
    savedSSID=prefs.getString("ssid","");
    savedPassword=prefs.getString("pass","");
    prefs.end();
  }

  if(!savedSSID.length()){

    Serial.println("TARS: NO WIFI CREDENTIALS");

    startPortal();
    portalWaitLoop();

    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);

  Serial.print("TARS: WiFi connecting to ");
  Serial.println(savedSSID);

  for(uint8_t attempt=1;attempt<=WIFI_MAX_ATTEMPTS;attempt++){

    Serial.printf(
      "TARS: WIFI AUTH ATTEMPT %u/%u\n",
      attempt,WIFI_MAX_ATTEMPTS
    );

    WiFi.disconnect(false,true);
    delay(200);

    WiFi.begin(
      savedSSID.c_str(),
      savedPassword.c_str()
    );

    uint32_t start=millis();

    while(
      WiFi.status()!=WL_CONNECTED&&
      millis()-start<WIFI_TIMEOUT_MS
    ){
      wl_status_t s=WiFi.status();

      if(
        s==WL_CONNECT_FAILED||
        s==WL_NO_SSID_AVAIL
      ){
        break;
      }

      delay(100);
      yield();
    }

    if(WiFi.status()==WL_CONNECTED){

      wifiReady=true;

      prefs.begin("wifi",false);
      prefs.putBool("pending",false);
      prefs.putBool("setupDone",true);
      prefs.end();

      Serial.println();
      Serial.println("TARS: WIFI CONNECTED");
      Serial.print("TARS: IP = ");
      Serial.println(WiFi.localIP());
      Serial.print("TARS: RSSI = ");
      Serial.println(WiFi.RSSI());

      return true;
    }

    Serial.printf(
      "TARS: WIFI AUTH FAILED %u/%u\n",
      attempt,WIFI_MAX_ATTEMPTS
    );

    WiFi.disconnect(false,true);
    delay(300);
  }

  wifiReady=false;

  Serial.println();
  Serial.println("TARS: WIFI AUTH FAILED 3/3");
  Serial.println("TARS: WRONG PASSWORD OR WIFI UNAVAILABLE");
  Serial.println("TARS: STOPPING WIFI RETRIES");
  Serial.println("TARS: RETURNING TO WIFI SETUP");

  WiFi.disconnect(false,true);
  delay(300);

  startPortal();
  portalWaitLoop();

  return false;
}

/* ================= DISCONNECT ================= */

void wifiManagerDisconnect(){

  Serial.println("TARS: WiFi OFF");

  if(portalRunning){

    server.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);

    portalRunning=false;
  }

  WiFi.disconnect(false);

  wifiReady=false;
}
