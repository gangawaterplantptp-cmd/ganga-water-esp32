/*
  Ganga Water Machine ESP32
  Firmware: 1.3.0 - Live Network Signal Reporting

  Machine ID: ganga01
  Cloudflare Worker:
  https://small-mud-0ce7.gagawaterplantptp.workers.dev

  Preserved dispense configuration:
  Relay GPIO: 22
  Relay: active-low
  Pulse ON: 80 ms
  Pulse OFF: 120 ms
  ₹5  -> 5 litres  -> PULSE_5  -> 5 pulses
  ₹10 -> 15 litres -> PULSE_10 -> 10 pulses
  ₹15 -> 20 litres -> PULSE_15 -> 15 pulses

  IMPORTANT:
  1. Enter your Wi-Fi name/password below.
  2. This sketch does not change the Cloudflare payment/queue logic.
  3. Test the relay with the water supply safely isolated before live use.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#define RELAY_PIN 22

// ---------------- WIFI SETTINGS ----------------
const char* WIFI_NAME = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---------------- CLOUD WORKER -----------------
const char* WORKER_URL =
  "https://small-mud-0ce7.gagawaterplantptp.workers.dev";

const char* MACHINE_ID = "ganga01";
const char* FIRMWARE_VERSION = "1.3.0";

// ---------------- TIMINGS -----------------------
const unsigned long POLL_INTERVAL = 3000;
const unsigned long HEARTBEAT_INTERVAL = 5000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

unsigned long lastPollTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastWiFiRetryTime = 0;

bool machineBusy = false;

// ---------------- RELAY FUNCTIONS ---------------
// Keep the existing active-low relay behavior unchanged.
void relayOn() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
}

void relayOff() {
  pinMode(RELAY_PIN, INPUT);
}

void sendOnePulse() {
  relayOn();
  delay(80);
  relayOff();
  delay(120);
}

void dispenseAmount(int amount) {
  Serial.printf("Dispensing %d pulses\n", amount);

  for (int i = 0; i < amount; i++) {
    sendOnePulse();
  }

  Serial.println("All pulses completed");
}

// ---------------- WIFI CONNECTION ----------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.println("Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  int count = 0;

  while (WiFi.status() != WL_CONNECTED && count < 40) {
    delay(500);
    Serial.print(".");
    count++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("Wi-Fi connection failed");
  }
}

// ---------------- INTERNET CHECK -----------------
// Wi-Fi connected does not always mean Internet is working.
// This checks whether the Cloudflare Worker can be reached.
bool checkInternetConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(5000);

  String url = String(WORKER_URL) + "/";

  if (!https.begin(client, url)) {
    Serial.println("Internet check: HTTPS begin failed");
    return false;
  }

  int httpCode = https.GET();
  https.end();

  bool connected = (httpCode >= 200 && httpCode < 400);

  Serial.print("Cloudflare connectivity HTTP code: ");
  Serial.println(httpCode);

  return connected;
}

// ---------------- HTTP POST ----------------------
bool postJson(String endpoint, String body) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("POST skipped: Wi-Fi disconnected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(10000);

  String url = String(WORKER_URL) + endpoint;

  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  int httpCode = https.POST(body);

  Serial.print("POST ");
  Serial.print(endpoint);
  Serial.print(" HTTP code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String response = https.getString();

    if (response.length() > 0) {
      Serial.println(response);
    }
  }

  https.end();

  return (httpCode >= 200 && httpCode < 300);
}

// ---------------- LIVE HEARTBEAT -----------------
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Heartbeat skipped: Wi-Fi offline");
    return;
  }

  int rssi = WiFi.RSSI();
  String ip = WiFi.localIP().toString();

  bool internetConnected = checkInternetConnection();

  String body = "{";
  body += "\"machine_id\":\"" + String(MACHINE_ID) + "\",";
  body += "\"rssi\":" + String(rssi) + ",";
  body += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
  body += "\"ip\":\"" + ip + "\",";
  body += "\"wifi_connected\":true,";
  body += "\"internet_connected\":";
  body += (internetConnected ? "true" : "false");
  body += "}";

  Serial.println("Sending network heartbeat...");
  Serial.print("RSSI: ");
  Serial.print(rssi);
  Serial.println(" dBm");

  Serial.print("Wi-Fi: ");
  Serial.println("CONNECTED");

  Serial.print("Internet/Worker: ");
  Serial.println(internetConnected ? "CONNECTED" : "NOT REACHABLE");

  postJson("/heartbeat", body);
}

// ---------------- COMMAND COMPLETION --------------
bool commandComplete() {
  // Keep the existing Worker behavior: it completes the oldest active command.
  return postJson("/command-complete", "{}");
}

// ---------------- PAYMENT COMMAND CHECK -----------
void checkForPayment() {
  if (WiFi.status() != WL_CONNECTED || machineBusy) {
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(10000);

  String url = String(WORKER_URL) + "/check-command";

  if (!https.begin(client, url)) {
    Serial.println("Could not start command request");
    return;
  }

  int httpCode = https.GET();

  if (httpCode != 200) {
    Serial.print("Command check HTTP error: ");
    Serial.println(httpCode);
    https.end();
    return;
  }

  String response = https.getString();
  https.end();

  if (response.indexOf("\"command\":\"NONE\"") >= 0 ||
      response.indexOf("\"command\": \"NONE\"") >= 0) {
    return;
  }

  int amount = 0;

  if (response.indexOf("PULSE_5") >= 0) {
    amount = 5;
  } else if (response.indexOf("PULSE_10") >= 0) {
    amount = 10;
  } else if (response.indexOf("PULSE_15") >= 0) {
    amount = 15;
  }

  if (amount == 0) {
    Serial.println("Unknown command received. Not dispensing.");
    Serial.println(response);
    return;
  }

  Serial.print("Dispense command received: ");
  Serial.println(amount);

  machineBusy = true;

  // Existing dispense sequence preserved.
  dispenseAmount(amount);

  // Existing completion retry behavior preserved.
  bool completed = false;

  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.print("Sending command completion, attempt ");
    Serial.println(attempt);

    if (commandComplete()) {
      completed = true;
      Serial.println("Command completion accepted by Worker");
      break;
    }

    delay(2000);
  }

  if (!completed) {
    Serial.println("WARNING: Could not confirm command completion.");
  }

  machineBusy = false;
}

// ---------------- SETUP ---------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==================================");
  Serial.println("GANGA WATER MACHINE");
  Serial.println("Firmware 1.3.0 - Network Monitor");
  Serial.println("Machine: ganga01");
  Serial.println("==================================");

  relayOff();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    sendHeartbeat();
  }

  lastHeartbeatTime = millis();
  lastPollTime = millis();
  lastWiFiRetryTime = millis();
}

// ---------------- MAIN LOOP -----------------------
void loop() {
  unsigned long now = millis();

  // Reconnect without repeatedly blocking the loop.
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiRetryTime >= WIFI_RETRY_INTERVAL) {
      lastWiFiRetryTime = now;
      connectWiFi();

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi restored. Sending heartbeat.");
        sendHeartbeat();
        lastHeartbeatTime = millis();
      }
    }

    delay(50);
    return;
  }

  // Check for paid dispense commands.
  if (!machineBusy && now - lastPollTime >= POLL_INTERVAL) {
    lastPollTime = now;
    checkForPayment();
  }

  // Send live network details to Cloudflare.
  if (!machineBusy && now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = now;
    sendHeartbeat();
  }

  delay(10);
}
