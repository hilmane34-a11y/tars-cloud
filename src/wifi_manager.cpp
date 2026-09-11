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
static const uint32_t PORTAL_DNS_PORT = 53;

static String savedSSID;
static String savedPassword;

static bool portalRunning = false;
static bool wifiReady = false;
static bool routesRegistered = false;

// Flag hanya untuk restart setelah SIMPAN konfigurasi.
RTC_DATA_ATTR static uint32_t setupBootMagic = 0;

static const uint32_t SETUP_MAGIC = 0x54415253; // "TARS"

// ============================================================
// HTML WIFI SETUP
// ============================================================

static String htmlPage() {
  return
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TARS WiFi</title>"
    "<style>"
    "body{font-family:Arial;text-align:center;padding:30px;background:#111;color:#fff}"
    "h1{margin-bottom:8px}"
    "p{line-height:1.5}"
    "input{box-sizing:border-box;width:90%;max-width:360px;padding:12px;margin:8px 0;font-size:16px}"
    "button{padding:12px 30px;font-size:16px;cursor:pointer}"
    "</style></head><body>"
    "<h1>TARS</h1>"
    "<p>WiFi Setup</p>"
    "<form action='/save' method='POST'>"
    "<input name='ssid' placeholder='Nama WiFi' required>"
    "<input name='password' type='password' placeholder='Password WiFi'>"
    "<br><button type='submit'>SIMPAN</button>"
    "</form>"
    "<p>Hubungkan HP ke <b>TARS-SETUP</b></p>"
    "<p>Password: <b>12345678</b></p>"
    "<p>Buka <b>192.168.4.1</b></p>"
    "</body></html>";
}

// ============================================================
// PORTAL ROUTES
// ============================================================

static void registerPortalRoutes() {
  if (routesRegistered) return;

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

    Serial.println("TARS: SAVING WIFI CREDENTIALS");

    prefs.begin("wifi", false);

    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);

    prefs.end();

    savedSSID = ssid;
    savedPassword = password;

    // Tandai bahwa restart berikutnya adalah
    // restart setelah konfigurasi WiFi.
    setupBootMagic = SETUP_MAGIC;

    server.send(
      200,
      "text/html",
      "<!DOCTYPE html><html><body "
      "style='font-family:Arial;text-align:center;padding:30px'>"
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

  routesRegistered = true;
}

// ============================================================
// START PORTAL
// ============================================================

static void startPortal() {
  if (portalRunning) return;

  Serial.println();
  Serial.println("========================================");
  Serial.println("       TARS WIFI SETUP MODE");
  Serial.println("========================================");
  Serial.println("TARS: Connect HP to TARS-SETUP");
  Serial.println("TARS: Password = 12345678");
  Serial.println("TARS: Open http://192.168.4.1");
  Serial.println("========================================");

  WiFi.persistent(false);

  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_AP_STA);

  bool apOK = WiFi.softAP(AP_NAME, AP_PASSWORD);

  if (!apOK) {
    Serial.println("TARS: AP START FAILED");
    return;
  }

  IPAddress apIP = WiFi.softAPIP();

  Serial.print("TARS: AP IP = ");
  Serial.println(apIP);

  dnsServer.start(PORTAL_DNS_PORT, "*", apIP);

  registerPortalRoutes();

  server.begin();

  portalRunning = true;
}

// ============================================================
// HANDLE PORTAL
// ============================================================

static void handlePortal() {
  if (!portalRunning) return;

  dnsServer.processNextRequest();
  server.handleClient();
}

// ============================================================
// WAIT CONFIGURATION
// ============================================================

static void portalWaitLoop() {
  if (!portalRunning) return;

  Serial.println("TARS: WAITING FOR WIFI CONFIGURATION...");

  while (portalRunning) {
    handlePortal();
    delay(2);
    yield();
  }
}

// ============================================================
// BEGIN
// ============================================================

bool wifiManagerBegin() {

  WiFi.persistent(false);

  // ----------------------------------------------------------
  // Jika ini adalah restart langsung setelah SIMPAN,
  // gunakan credential yang baru saja disimpan.
  // ----------------------------------------------------------

  bool afterSetupRestart = (setupBootMagic == SETUP_MAGIC);

  if (afterSetupRestart) {

    Serial.println("TARS: SETUP RESTART DETECTED");

    // Konsumsi flag SEKARANG supaya setelah boot ini selesai
    // flag tidak dipakai lagi.
    setupBootMagic = 0;

    prefs.begin("wifi", true);

    savedSSID = prefs.getString("ssid", "");
    savedPassword = prefs.getString("pass", "");

    prefs.end();

    if (!savedSSID.length()) {

      Serial.println("TARS: SAVED WIFI NOT FOUND");

      startPortal();
      portalWaitLoop();

      return false;
    }

    Serial.print("TARS: NEW WIFI READY = ");
    Serial.println(savedSSID);

    return true;
  }

  // ----------------------------------------------------------
  // Boot normal / power-on:
  // SELALU minta konfigurasi baru.
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("TARS: NEW POWER BOOT");
  Serial.println("TARS: WIFI SETUP REQUIRED");

  // Hapus credential runtime.
  savedSSID = "";
  savedPassword = "";

  startPortal();
  portalWaitLoop();

  return false;
}

// ============================================================
// CONNECT WIFI
// ============================================================

bool wifiManagerConnect(bool requireTime) {
  (void)requireTime;

  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    return true;
  }

  // Jika belum ada credential di RAM,
  // ambil credential yang baru disimpan.
  if (!savedSSID.length()) {

    prefs.begin("wifi", true);

    savedSSID = prefs.getString("ssid", "");
    savedPassword = prefs.getString("pass", "");

    prefs.end();
  }

  if (!savedSSID.length()) {

    Serial.println("TARS: NO WIFI CREDENTIALS");

    startPortal();
    portalWaitLoop();

    return false;
  }

  Serial.print("TARS: WiFi connecting to ");
  Serial.println(savedSSID);

  WiFi.persistent(false);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  WiFi.begin(
    savedSSID.c_str(),
    savedPassword.c_str()
  );

  uint32_t start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < WIFI_TIMEOUT_MS
  ) {

    delay(250);

    Serial.print(".");

    yield();
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    wifiReady = true;

    Serial.println("TARS: WIFI CONNECTED");

    Serial.print("TARS: IP = ");
    Serial.println(WiFi.localIP());

    Serial.print("TARS: RSSI = ");
    Serial.println(WiFi.RSSI());

    return true;
  }

  // ----------------------------------------------------------
  // Credential gagal / WiFi tidak ditemukan.
  // Buka setup lagi.
  // ----------------------------------------------------------

  wifiReady = false;

  Serial.println("TARS: WIFI FAILED");
  Serial.println("TARS: STARTING WIFI SETUP PORTAL");

  WiFi.disconnect(true);

  delay(100);

  startPortal();
  portalWaitLoop();

  return false;
}

// ============================================================
// DISCONNECT / WIFI OFF
// ============================================================

void wifiManagerDisconnect() {

  Serial.println("TARS: WiFi OFF");

  if (portalRunning) {

    server.stop();
    dnsServer.stop();

    WiFi.softAPdisconnect(true);

    portalRunning = false;
  }

  WiFi.disconnect(true);

  delay(100);

  WiFi.mode(WIFI_OFF);

  wifiReady = false;

  delay(300);
}
