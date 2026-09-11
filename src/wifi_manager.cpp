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

static const char *AP_NAME = "TARS-SETUP";
static const char *AP_PASSWORD = "12345678";

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t PORTAL_DNS_PORT = 53;

static String savedSSID;
static String savedPassword;

static bool portalRunning = false;
static bool wifiReady = false;
static bool routesRegistered = false;

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

    // Simpan credential.
    prefs.begin("wifi", false);

    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);

    // Tandai bahwa boot berikutnya harus langsung
    // mencoba credential yang baru disimpan.
    prefs.putBool("pending", true);

    prefs.end();

    savedSSID = ssid;
    savedPassword = password;

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

  bool apOK = WiFi.softAP(
    AP_NAME,
    AP_PASSWORD
  );

  if (!apOK) {
    Serial.println("TARS: AP START FAILED");
    return;
  }

  IPAddress apIP = WiFi.softAPIP();

  Serial.print("TARS: AP IP = ");
  Serial.println(apIP);

  dnsServer.start(
    PORTAL_DNS_PORT,
    "*",
    apIP
  );

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

  Serial.println(
    "TARS: WAITING FOR WIFI CONFIGURATION..."
  );

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

  esp_reset_reason_t reason = esp_reset_reason();

  bool powerBoot =
    reason == ESP_RST_POWERON;

  // ----------------------------------------------------------
  // POWER ON BARU
  // ----------------------------------------------------------

  if (powerBoot) {

    Serial.println();
    Serial.println("TARS: POWER ON BOOT");
    Serial.println("TARS: WIFI SETUP REQUIRED");

    // Jangan gunakan credential lama sebagai credential runtime.
    savedSSID = "";
    savedPassword = "";

    // Pending lama tidak boleh dipakai setelah power-on baru.
    prefs.begin("wifi", false);
    prefs.putBool("pending", false);
    prefs.end();

    startPortal();
    portalWaitLoop();

    return false;
  }

  // ----------------------------------------------------------
  // BOOT SETELAH ESP.RESTART()
  // ----------------------------------------------------------

  prefs.begin("wifi", false);

  bool pending =
    prefs.getBool("pending", false);

  savedSSID =
    prefs.getString("ssid", "");

  savedPassword =
    prefs.getString("pass", "");

  prefs.end();

  if (pending && savedSSID.length()) {

    Serial.println(
      "TARS: WIFI SETUP RESTART DETECTED"
    );

    Serial.print(
      "TARS: SAVED WIFI = "
    );

    Serial.println(savedSSID);

    return true;
  }

  // ----------------------------------------------------------
  // RESET LAIN TANPA KONFIGURASI PENDING
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("TARS: WIFI SETUP REQUIRED");

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

  // ----------------------------------------------------------
  // Load credential kalau belum ada di RAM.
  // ----------------------------------------------------------

  if (!savedSSID.length()) {

    prefs.begin("wifi", true);

    savedSSID =
      prefs.getString("ssid", "");

    savedPassword =
      prefs.getString("pass", "");

    prefs.end();
  }

  if (!savedSSID.length()) {

    Serial.println(
      "TARS: NO WIFI CREDENTIALS"
    );

    startPortal();
    portalWaitLoop();

    return false;
  }

  Serial.print(
    "TARS: WiFi connecting to "
  );

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

  // ----------------------------------------------------------
  // BERHASIL
  // ----------------------------------------------------------

  if (WiFi.status() == WL_CONNECTED) {

    wifiReady = true;

    // Konfigurasi sudah berhasil dipakai.
    // Hapus status pending supaya tidak dianggap
    // sebagai restart setelah setup lagi.
    prefs.begin("wifi", false);

    prefs.putBool("pending", false);

    prefs.end();

    Serial.println(
      "TARS: WIFI CONNECTED"
    );

    Serial.print(
      "TARS: IP = "
    );

    Serial.println(WiFi.localIP());

    Serial.print(
      "TARS: RSSI = "
    );

    Serial.println(WiFi.RSSI());

    return true;
  }

  // ----------------------------------------------------------
  // GAGAL CONNECT
  // ----------------------------------------------------------

  wifiReady = false;

  Serial.println(
    "TARS: WIFI FAILED"
  );

  Serial.println(
    "TARS: STARTING WIFI SETUP PORTAL"
  );

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

  Serial.println(
    "TARS: WiFi OFF"
  );

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
