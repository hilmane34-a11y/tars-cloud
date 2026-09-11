#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#include "wifi_manager.h"

static Preferences prefs;
static WebServer server(80);
static DNSServer dnsServer;

static const char *AP_NAME = "TARS-SETUP";
static const char *AP_PASSWORD = "12345678";

static const uint32_t WIFI_TIMEOUT_MS = 15000;

static String savedSSID;
static String savedPassword;
static bool portalRunning = false;
static bool wifiReady = false;

static String htmlPage() {
  String page =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TARS WiFi</title>"
    "<style>"
    "body{font-family:Arial;text-align:center;padding:30px;background:#111;color:#fff}"
    "input{width:90%;padding:12px;margin:8px 0;font-size:16px}"
    "button{padding:12px 30px;font-size:16px}"
    "</style></head><body>"
    "<h1>TARS</h1>"
    "<p>WiFi Setup</p>"
    "<form action='/save' method='POST'>"
    "<input name='ssid' placeholder='Nama WiFi' required>"
    "<input name='password' type='password' placeholder='Password WiFi'>"
    "<br><button type='submit'>SIMPAN</button>"
    "</form>"
    "<p>Hubungkan HP ke <b>TARS-SETUP</b></p>"
    "</body></html>";

  return page;
}

static void startPortal() {
  if (portalRunning) return;

  Serial.println("TARS: WIFI SETUP MODE");
  Serial.println("TARS: Connect HP to TARS-SETUP");
  Serial.println("TARS: Password = 12345678");
  Serial.println("TARS: Open http://192.168.4.1");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME, AP_PASSWORD);

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage());
  });

  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");

    ssid.trim();
    password.trim();

    if (!ssid.length()) {
      server.send(400, "text/plain", "SSID kosong");
      return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);
    prefs.end();

    server.send(
      200,
      "text/html",
      "<html><body style='font-family:Arial;text-align:center;padding:30px'>"
      "<h2>TARS</h2>"
      "<p>WiFi tersimpan.</p>"
      "<p>TARS sedang restart...</p>"
      "</body></html>"
    );

    delay(1000);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.send(200, "text/html", htmlPage());
  });

  server.begin();
  portalRunning = true;
}

bool wifiManagerBegin() {
  prefs.begin("wifi", true);

  savedSSID = prefs.getString("ssid", "");
  savedPassword = prefs.getString("pass", "");

  prefs.end();

  if (!savedSSID.length()) {
    startPortal();

    while (true) {
      dnsServer.processNextRequest();
      server.handleClient();
      delay(2);
    }
  }

  return true;
}

bool wifiManagerConnect(bool requireTime) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (!savedSSID.length()) {
    startPortal();
    return false;
  }

  Serial.print("TARS: WiFi connecting to ");
  Serial.println(savedSSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

  uint32_t start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_TIMEOUT_MS) {

    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;

    Serial.print("TARS: IP = ");
    Serial.println(WiFi.localIP());

    return true;
  }

  Serial.println("TARS: WIFI FAILED");
  startPortal();

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2);
  }

  return false;
}

void wifiManagerDisconnect() {
  Serial.println("TARS: WiFi OFF");

  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);

  wifiReady = false;
  delay(300);
}
