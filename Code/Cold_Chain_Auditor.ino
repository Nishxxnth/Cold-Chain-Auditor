/*
 ╔══════════════════════════════════════════════════════╗
 ║         COLD CHAIN AUDITOR — Full Firmware           ║
 ║  ESP32 · DS18B20 · MPU6050 · Reed Switch · SD · OLED ║
 ╚══════════════════════════════════════════════════════╝

 Wiring:
   DS18B20  → GPIO 4
   MPU6050  → SDA 21, SCL 22
   OLED     → SDA 21, SCL 22  (0x3C)
   Reed SW  → GPIO 27 (INPUT_PULLUP — LOW=closed/magnet present)
   SD CS    → GPIO 5
   LED Red  → GPIO 25
   LED Grn  → GPIO 26

 Libraries (install via Arduino Library Manager):
   MPU6050        
   DallasTemperature
   OneWire
   Adafruit SSD1306
   Adafruit GFX
*/

#include <WiFi.h>
#include <Wire.h>
#include <MPU6050.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>

// ─── WiFi ─────────────────────────────────────────────
const char* ssid     = "Nishaanth";   //Put your own WiFi Username
const char* password = "nisco@06";    //Put your WiFi Password
WiFiServer server(80);

// ─── OLED ─────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ─── Pins ─────────────────────────────────────────────
#define ONE_WIRE_BUS  4
#define RED_PIN      25
#define GREEN_PIN    26
#define REED_PIN     27
#define SD_CS         5

// ─── Sensors ──────────────────────────────────────────
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
MPU6050 mpu;

// ─── State ────────────────────────────────────────────
float    temp          = 4.0f;
float    cts           = 100.0f;
bool     motion        = false;
bool     doorOpen      = false;
bool     sdReady       = false;
bool     wifiConnected = false;

// Door tracking
unsigned long doorOpenSince     = 0;
bool          doorWasOpen       = false;
bool          doorPenaltyActive = false;

// Counters
int  motionEventCount = 0;
int  breachEventCount = 0;
bool lastInRange      = true;

// Timers
unsigned long lastLogMs    = 0;
unsigned long lastSensorMs = 0;
unsigned long lastOledMs   = 0;

// ─── CTS DECAY CONSTANTS (practical / real-world tuned) ───────────
//
// Cold chain context: 2–8 °C vaccine storage, multi-hour transport.
// A perfect run should stay near 100. A single brief door-open or
// small temp blip should not tank the score. Sustained excursions
// should matter clearly.
//
// All rates are per second unless noted.

const float TEMP_HIGH_RATE   = 0.003f;   // per °C above 8°C, per second
const float TEMP_LOW_RATE    = 0.005f;   // per °C below 2°C, per second
const float TEMP_FREEZE_XTRA = 0.010f;   // extra per second when below 0°C

const float DOOR_OPEN_RATE   = 0.008f;   // per second while door open >20 s

// Motion: one-shot penalty on each rising edge (not per-second)
const float MOTION_PENALTY   = 0.05f;    // deducted once per motion event

// Slow recovery when everything is fine — restores monitoring confidence
const float RECOVERY_RATE    = 0.001f;   // per second when all is well

const unsigned long DOOR_THRESHOLD_MS = 20000UL;

// ─── SD LOG ───────────────────────────────────────────────────────
void logData() {
  if (!sdReady) return;
  File f = SD.open("/log.csv", FILE_APPEND);
  if (f) {
    f.printf("%lu,%.2f,%.2f,%d,%d\n",
             millis(), temp, cts, (int)motion, (int)doorOpen);
    f.close();
  }
}

// ─── OLED DRAW ────────────────────────────────────────────────────
void drawOLED() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  // Row 0: Safe / At Risk / Compromised
  display.setTextSize(1);
  display.setCursor(0, 0);
  const char* status = cts >= 70 ? "** SAFE **"
                     : cts >= 40 ? "** AT RISK **"
                     :             "** COMPROMISED **";
  // Centre it roughly
  int px = max(0, (128 - (int)strlen(status) * 6) / 2);
  display.setCursor(px, 0);
  display.print(status);

  display.drawLine(0, 9, 127, 9, WHITE);

  // Row 1: Temp large
  display.setCursor(0, 12);
  display.setTextSize(1);
  display.print("TEMP");
  display.setTextSize(2);
  display.setCursor(30, 10);
  char tbuf[10];
  snprintf(tbuf, sizeof(tbuf), "%.1fC", temp);
  display.print(tbuf);

  // Row 2: CTS bar
  display.setTextSize(1);
  display.setCursor(0, 30);
  char cbuf[16];
  snprintf(cbuf, sizeof(cbuf), "CTS %.1f", cts);
  display.print(cbuf);

  display.drawRect(0, 40, 128, 6, WHITE);
  int bw = constrain((int)(cts / 100.0f * 124), 0, 124);
  if (bw > 0) display.fillRect(2, 42, bw, 2, WHITE);

  // Row 3: icons row
  display.setTextSize(1);
  display.setCursor(0, 50);
  // Door | Motion | SD | WiFi
  display.printf("D:%-5s M:%c SD:%c W:%c",
    doorOpen ? "OPEN" : "CLSD",
    motion        ? 'Y' : 'N',
    sdReady       ? 'Y' : '-',
    wifiConnected ? 'Y' : '-');

  display.display();
}

// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  Wire.setClock(100000);
  delay(100);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Cold Chain Auditor");
    display.println("Booting...");
    display.display();
  }

  // GPIO
  pinMode(RED_PIN,   OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(REED_PIN,  INPUT_PULLUP);
  digitalWrite(RED_PIN,   LOW);
  digitalWrite(GREEN_PIN, LOW);

  // Sensors
  sensors.begin();
  mpu.initialize();
  mpu.setSleepEnabled(false);
  Serial.println(mpu.testConnection() ? "MPU6050 OK" : "MPU6050 FAIL");

  // SD
  if (SD.begin(SD_CS)) {
    sdReady = true;
    if (!SD.exists("/log.csv")) {
      File f = SD.open("/log.csv", FILE_WRITE);
      if (f) { f.println("Time_ms,Temp_C,CTS,Motion,Door"); f.close(); }
    }
    Serial.println("SD Ready");
  } else {
    Serial.println("SD Failed");
  }

  // WiFi
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(ssid, password);
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    server.begin();
    Serial.printf("\nWiFi: %s\n", WiFi.localIP().toString().c_str());
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Connected");
    display.println(WiFi.localIP().toString());
    display.display();
    delay(1500);
  } else {
    Serial.println("\nWiFi failed — running offline");
  }
}

// ════════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── Sensor + CTS update every 500 ms ─────────────────────────────
  if (now - lastSensorMs >= 500) {
    lastSensorMs = now;

    // Temperature
    sensors.requestTemperatures();
    float raw = sensors.getTempCByIndex(0);
    if (raw > -55.0f) temp = raw;

    // MPU6050 — rising-edge motion detection
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    bool motNow = (abs(ax) > 7000 || abs(ay) > 7000 || abs(az - 16384) > 7000);
    if (motNow && !motion) {
      // Rising edge only
      cts -= MOTION_PENALTY;
      motionEventCount++;
    }
    motion = motNow;

    // Reed switch: LOW = magnet = door CLOSED
    doorOpen = (digitalRead(REED_PIN) == HIGH);

    if (doorOpen) {
      if (!doorWasOpen) doorOpenSince = now;
      doorWasOpen = true;
    } else {
      doorWasOpen       = false;
      doorPenaltyActive = false;
    }
    unsigned long openMs = doorOpen ? (now - doorOpenSince) : 0;
    doorPenaltyActive    = doorOpen && (openMs > DOOR_THRESHOLD_MS);

    // ── CTS decay (dt = 0.5 s) ────────────────────────────────────
    const float dt       = 0.5f;
    bool        inRange  = (temp >= 2.0f && temp <= 8.0f);

    if (temp > 8.0f)
      cts -= TEMP_HIGH_RATE * (temp - 8.0f) * dt;
    if (temp < 2.0f)
      cts -= TEMP_LOW_RATE  * (2.0f - temp)  * dt;
    if (temp < 0.0f)
      cts -= TEMP_FREEZE_XTRA * dt;
    if (doorPenaltyActive)
      cts -= DOOR_OPEN_RATE * dt;

    // Slow recovery when everything is fine
    if (inRange && !doorPenaltyActive && cts < 100.0f)
      cts += RECOVERY_RATE * dt;

    if (!inRange && lastInRange) breachEventCount++;
    lastInRange = inRange;

    cts = constrain(cts, 0.0f, 100.0f);

    // ── LEDs ──────────────────────────────────────────────────────
    if (cts >= 70) {
      digitalWrite(GREEN_PIN, HIGH);
      digitalWrite(RED_PIN,   LOW);
    } else if (cts >= 40) {
      bool blink = (now / 600) & 1;
      digitalWrite(GREEN_PIN, blink);
      digitalWrite(RED_PIN,   !blink);
    } else {
      digitalWrite(GREEN_PIN, LOW);
      digitalWrite(RED_PIN,   HIGH);
    }

    // ── SD log every 1 s ──────────────────────────────────────────
    if (now - lastLogMs >= 1000) {
      logData();
      lastLogMs = now;
    }
  }

  // ── OLED refresh every 1 s ────────────────────────────────────────
  if (now - lastOledMs >= 1000) {
    lastOledMs = now;
    drawOLED();
  }

  // ── Web server ────────────────────────────────────────────────────
  if (!wifiConnected) { delay(5); return; }

  WiFiClient client = server.available();
  if (!client) { delay(5); return; }

  client.setTimeout(300);
  String req = client.readStringUntil('\r');
  client.flush();

  // ── /data → JSON ──────────────────────────────────────────────────
  if (req.indexOf("GET /data") != -1) {
    unsigned long openSec = doorOpen ? (millis() - doorOpenSince) / 1000UL : 0;
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.printf(
      "{\"temp\":%.2f,\"cts\":%.2f,\"motion\":%s,\"door\":%s,"
      "\"doorOpenSec\":%lu,\"motionEvents\":%d,\"breachEvents\":%d,"
      "\"sdReady\":%s,\"wifi\":%s}",
      temp, cts,
      motion        ? "true" : "false",
      doorOpen      ? "true" : "false",
      openSec,
      motionEventCount, breachEventCount,
      sdReady       ? "true" : "false",
      wifiConnected ? "true" : "false"
    );
    client.stop();
    return;
  }

  // ── /download → send log.csv ──────────────────────────────────────
  if (req.indexOf("GET /download") != -1) {
    if (!sdReady) {
      client.println("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n");
      client.println("SD not available."); client.stop(); return;
    }
    File f = SD.open("/log.csv", FILE_READ);
    if (!f) {
      client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n");
      client.println("Log file missing."); client.stop(); return;
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/csv");
    client.println("Content-Disposition: attachment; filename=\"cold_chain_log.csv\"");
    client.println("Connection: close");
    client.println();
    uint8_t buf[512];
    while (f.available()) {
      int n = f.read(buf, sizeof(buf));
      client.write(buf, n);
    }
    f.close();
    client.stop();
    return;
  }

  // ── / → Dashboard HTML ───────────────────────────────────────────
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.print(R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Cold Chain Auditor</title>
<link rel="preconnect" href="https://fonts.googleapis.com"/>
<link href="https://fonts.googleapis.com/css2?family=DM+Mono:wght@400;500&family=DM+Sans:wght@300;400;500;600&display=swap" rel="stylesheet"/>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
:root{
  --bg:#0d1117; --surf:#161b22; --surf2:#1c2128;
  --bdr:rgba(255,255,255,0.07);
  --txt:#e6edf3; --muted:#7d8590;
  --safe:#3fb950; --safe-bg:rgba(63,185,80,.10); --safe-bdr:rgba(63,185,80,.22);
  --warn:#d29922; --warn-bg:rgba(210,153,34,.10); --warn-bdr:rgba(210,153,34,.22);
  --dngr:#f85149; --dngr-bg:rgba(248,81,73,.10);  --dngr-bdr:rgba(248,81,73,.22);
  --blue:#58a6ff; --blue-bg:rgba(88,166,255,.08); --blue-bdr:rgba(88,166,255,.20);
  --mono:'DM Mono',monospace; --sans:'DM Sans',sans-serif;
  --r:10px; --rl:14px;
}
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0;}
body{background:var(--bg);color:var(--txt);font-family:var(--sans);min-height:100vh;padding:1.5rem 1rem 3rem;}
.wrap{max-width:1000px;margin:0 auto;display:flex;flex-direction:column;gap:.875rem;}

/* header */
.hdr{display:flex;align-items:center;justify-content:space-between;padding-bottom:1rem;border-bottom:1px solid var(--bdr);}
.hl{display:flex;align-items:center;gap:12px;}
.h-icon{width:38px;height:38px;border-radius:10px;background:var(--blue-bg);border:1px solid var(--blue-bdr);display:flex;align-items:center;justify-content:center;}
.h-icon svg{width:18px;height:18px;}
.h-title{font-size:17px;font-weight:600;letter-spacing:-.01em;}
.h-sub{font-size:11px;color:var(--muted);font-family:var(--mono);margin-top:2px;}
.live{display:flex;align-items:center;gap:7px;font-size:11px;color:var(--muted);font-family:var(--mono);}
.ldot{width:7px;height:7px;border-radius:50%;background:var(--safe);animation:blink 2s infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.25}}

/* banner */
.banner{border-radius:var(--r);padding:.8rem 1.1rem;display:flex;align-items:center;gap:12px;border:1px solid;transition:all .4s;}
.banner.safe  {background:var(--safe-bg);border-color:var(--safe-bdr);}
.banner.warn  {background:var(--warn-bg);border-color:var(--warn-bdr);}
.banner.danger{background:var(--dngr-bg);border-color:var(--dngr-bdr);}
.b-ico{width:28px;height:28px;border-radius:50%;display:flex;align-items:center;justify-content:center;flex-shrink:0;}
.safe .b-ico{background:var(--safe);}.warn .b-ico{background:var(--warn);}.danger .b-ico{background:var(--dngr);}
.b-ico svg{width:13px;height:13px;}
.b-title{font-size:14px;font-weight:600;}
.safe .b-title{color:var(--safe);}.warn .b-title{color:var(--warn);}.danger .b-title{color:var(--dngr);}
.b-sub{font-size:11px;color:var(--muted);font-family:var(--mono);margin-top:2px;}
.b-right{margin-left:auto;}
.dl-btn{display:flex;align-items:center;gap:6px;background:var(--blue-bg);border:1px solid var(--blue-bdr);color:var(--blue);font-family:var(--mono);font-size:11px;padding:6px 14px;border-radius:20px;cursor:pointer;text-decoration:none;transition:background .2s;white-space:nowrap;}
.dl-btn:hover{background:rgba(88,166,255,.15);}
.dl-btn svg{width:12px;height:12px;}

/* metrics */
.metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;}
.mc{background:var(--surf);border:1px solid var(--bdr);border-radius:var(--rl);padding:1rem 1.1rem;}
.mc-lbl{font-size:10px;text-transform:uppercase;letter-spacing:.07em;color:var(--muted);font-family:var(--mono);margin-bottom:10px;}
.mc-val{font-size:30px;font-weight:300;line-height:1;letter-spacing:-.02em;}
.mc-unit{font-size:13px;color:var(--muted);margin-left:2px;}
.mc-tag{display:inline-flex;align-items:center;font-size:11px;font-weight:500;padding:3px 10px;border-radius:20px;margin-top:10px;font-family:var(--mono);}
.ts{background:var(--safe-bg);color:var(--safe);border:1px solid var(--safe-bdr);}
.tw{background:var(--warn-bg);color:var(--warn);border:1px solid var(--warn-bdr);}
.td{background:var(--dngr-bg);color:var(--dngr);border:1px solid var(--dngr-bdr);}

/* indicators */
.inds{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
.ind{background:var(--surf);border:1px solid var(--bdr);border-radius:var(--rl);padding:1rem 1.1rem;display:flex;align-items:center;gap:14px;transition:border-color .3s;}
.i-blob{width:44px;height:44px;border-radius:12px;display:flex;align-items:center;justify-content:center;flex-shrink:0;transition:background .3s,border-color .3s;}
.i-blob svg{width:21px;height:21px;}
.i-lbl{font-size:10px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);font-family:var(--mono);margin-bottom:4px;}
.i-status{font-size:15px;font-weight:600;transition:color .3s;}
.i-sub{font-size:11px;color:var(--muted);margin-top:3px;font-family:var(--mono);}

.door-ok  .ind{border-color:var(--safe-bdr);}
.door-ok  .i-blob{background:var(--safe-bg);border:1px solid var(--safe-bdr);}
.door-ok  .i-status{color:var(--safe);}
.door-bad .ind{border-color:var(--dngr-bdr);}
.door-bad .i-blob{background:var(--dngr-bg);border:1px solid var(--dngr-bdr);}
.door-bad .i-status{color:var(--dngr);}

.mot-still .ind{border-color:var(--bdr);}
.mot-still .i-blob{background:var(--surf2);border:1px solid var(--bdr);}
.mot-still .i-status{color:var(--muted);}
.mot-still .i-blob svg{opacity:.35;}
.mot-act .ind{border-color:var(--warn-bdr);}
.mot-act .i-blob{background:var(--warn-bg);border:1px solid var(--warn-bdr);animation:mp .5s infinite;}
.mot-act .i-status{color:var(--warn);}
@keyframes mp{0%,100%{opacity:1}50%{opacity:.55}}

/* CTS panel */
.cts-panel{background:var(--surf);border:1px solid var(--bdr);border-radius:var(--rl);padding:1.25rem 1.4rem;}
.cts-top{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:1.1rem;}
.cts-lbl{font-size:10px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);font-family:var(--mono);margin-bottom:6px;}
.cts-num{font-size:52px;font-weight:300;line-height:1;letter-spacing:-.03em;transition:color .4s;}
.cts-denom{font-size:18px;color:var(--muted);}
.cts-leg{text-align:right;display:flex;flex-direction:column;gap:5px;padding-top:4px;}
.cts-leg-r{display:flex;align-items:center;gap:6px;justify-content:flex-end;font-size:11px;font-family:var(--mono);}
.cts-leg-d{width:6px;height:6px;border-radius:50%;}
.cts-track{background:var(--surf2);border-radius:4px;height:6px;overflow:hidden;margin-bottom:6px;}
.cts-fill{height:100%;border-radius:4px;transition:width .6s cubic-bezier(.4,0,.2,1),background .4s;}
.cts-ticks{display:flex;justify-content:space-between;font-size:10px;color:var(--muted);font-family:var(--mono);}
.cts-chart{position:relative;height:120px;border-top:1px solid var(--bdr);padding-top:1rem;margin-top:.875rem;}

/* charts row */
.charts{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
.cc{background:var(--surf);border:1px solid var(--bdr);border-radius:var(--rl);padding:1.1rem 1.25rem;}
.cc-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:.875rem;}
.cc-title{font-size:12px;font-weight:600;}
.cc-tag{font-size:10px;font-family:var(--mono);background:var(--surf2);color:var(--muted);padding:3px 8px;border-radius:20px;}
.cc-wrap{position:relative;height:150px;}

/* log */
.log{background:var(--surf);border:1px solid var(--bdr);border-radius:var(--rl);padding:1.1rem 1.25rem;}
.log-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:.75rem;}
.log-title{font-size:12px;font-weight:600;}
.log-cnt{font-size:10px;font-family:var(--mono);color:var(--muted);background:var(--surf2);padding:2px 8px;border-radius:20px;}
.li{display:flex;align-items:baseline;gap:10px;padding:5px 0;border-bottom:1px solid var(--bdr);font-size:11px;}
.li:last-child{border-bottom:none;}
.li-pip{width:5px;height:5px;border-radius:50%;flex-shrink:0;margin-top:1px;}
.li-ts{font-family:var(--mono);color:var(--muted);min-width:60px;}
.li-msg{color:var(--txt);}

/* footer */
.foot{display:flex;justify-content:space-between;font-size:11px;color:var(--muted);font-family:var(--mono);padding-top:.5rem;border-top:1px solid var(--bdr);}

@media(max-width:640px){
  .metrics{grid-template-columns:1fr 1fr;}
  .charts{grid-template-columns:1fr;}
}
</style>
</head>
<body>
<div class="wrap">

<!-- Header -->
<div class="hdr">
  <div class="hl">
    <div class="h-icon">
      <svg viewBox="0 0 18 18" fill="none">
        <rect x="8" y="1" width="2" height="10" rx="1" fill="#58a6ff"/>
        <circle cx="9" cy="13" r="3" fill="#58a6ff"/>
        <rect x="10.5" y="3" width="2.5" height="1.5" rx=".5" fill="#79c0ff"/>
        <rect x="10.5" y="6" width="2" height="1.5" rx=".5" fill="#79c0ff"/>
      </svg>
    </div>
    <div>
      <div class="h-title">Cold Chain Auditor</div>
      <div class="h-sub">ESP32 · DS18B20 · MPU6050 · SD · OLED</div>
    </div>
  </div>
  <div class="live"><span class="ldot"></span><span id="updated">connecting...</span></div>
</div>

<!-- Banner -->
<div class="banner safe" id="banner">
  <div class="b-ico">
    <svg viewBox="0 0 13 13" fill="none">
      <path id="b-path" d="M2 7l3 3 6-6" stroke="white" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>
    </svg>
  </div>
  <div>
    <div class="b-title" id="b-title">Safe — Chain Integrity Maintained</div>
    <div class="b-sub"  id="b-sub">All parameters within acceptable range</div>
  </div>
  <div class="b-right">
    <a href="/download" class="dl-btn">
      <svg viewBox="0 0 12 12" fill="none"><path d="M6 1v7M3 6l3 3 3-3M1 10h10" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>
      Download CSV
    </a>
  </div>
</div>

<!-- Metrics -->
<div class="metrics">
  <div class="mc">
    <div class="mc-lbl">Temperature</div>
    <div><span class="mc-val" id="m-temp">--</span><span class="mc-unit">°C</span></div>
    <div class="mc-tag ts" id="temp-tag">In range</div>
  </div>
  <div class="mc">
    <div class="mc-lbl">CTS Score</div>
    <div><span class="mc-val" id="m-cts">--</span><span class="mc-unit">/100</span></div>
    <div class="mc-tag ts" id="cts-tag">Excellent</div>
  </div>
  <div class="mc">
    <div class="mc-lbl">Temp Breaches</div>
    <div><span class="mc-val" id="m-breach">0</span></div>
    <div style="font-size:11px;color:var(--muted);margin-top:10px;font-family:var(--mono);">excursion events</div>
  </div>
  <div class="mc">
    <div class="mc-lbl">Motion Events</div>
    <div><span class="mc-val" id="m-motev">0</span></div>
    <div style="font-size:11px;color:var(--muted);margin-top:10px;font-family:var(--mono);">impacts / vibrations</div>
  </div>
</div>

<!-- Indicators -->
<div class="inds">
  <div class="door-ok" id="door-wrap">
    <div class="ind" id="door-card">
      <div class="i-blob">
        <svg viewBox="0 0 21 21" fill="none">
          <rect x="4" y="2" width="13" height="17" rx="2" stroke="#3fb950" stroke-width="1.5" id="door-rect"/>
          <circle cx="14" cy="10.5" r="1.2" fill="#3fb950" id="door-knob"/>
          <line x1="4" y1="2" x2="4" y2="19" stroke="#3fb950" stroke-width="1.5" id="door-line"/>
        </svg>
      </div>
      <div>
        <div class="i-lbl">Door Status</div>
        <div class="i-status" id="door-status">Closed</div>
        <div class="i-sub" id="door-sub">Sealed — no entry</div>
      </div>
    </div>
  </div>
  <div class="mot-still" id="mot-wrap">
    <div class="ind" id="mot-card">
      <div class="i-blob">
        <svg viewBox="0 0 21 21" fill="none">
          <path d="M10.5 3v4M10.5 14v4M3 10.5h4M14 10.5h4" stroke="#7d8590" stroke-width="1.5" stroke-linecap="round" id="mot-lines"/>
          <circle cx="10.5" cy="10.5" r="3.5" stroke="#7d8590" stroke-width="1.5" id="mot-circle"/>
        </svg>
      </div>
      <div>
        <div class="i-lbl">Motion / Impact</div>
        <div class="i-status" id="mot-status">Still</div>
        <div class="i-sub" id="mot-sub">No vibration detected</div>
      </div>
    </div>
  </div>
</div>

<!-- CTS Panel -->
<div class="cts-panel">
  <div class="cts-top">
    <div>
      <div class="cts-lbl">Cold Chain Trust Score</div>
      <div><span class="cts-num" id="cts-big" style="color:var(--safe)">--</span><span class="cts-denom">/100</span></div>
    </div>
    <div class="cts-leg">
      <div class="cts-leg-r"><span class="cts-leg-d" style="background:var(--safe)"></span><span style="color:var(--safe)">Safe ≥ 70</span></div>
      <div class="cts-leg-r"><span class="cts-leg-d" style="background:var(--warn)"></span><span style="color:var(--warn)">At Risk 40–70</span></div>
      <div class="cts-leg-r"><span class="cts-leg-d" style="background:var(--dngr)"></span><span style="color:var(--dngr)">Compromised &lt;40</span></div>
    </div>
  </div>
  <div class="cts-track"><div class="cts-fill" id="cts-bar" style="width:100%;background:#3fb950"></div></div>
  <div class="cts-ticks"><span>0</span><span>20</span><span>40</span><span>60</span><span>80</span><span>100</span></div>
  <div class="cts-chart"><canvas id="ctsChart" role="img" aria-label="CTS trend">CTS trend</canvas></div>
</div>

<!-- Charts -->
<div class="charts">
  <div class="cc">
    <div class="cc-hdr">
      <span class="cc-title">Temperature</span>
      <span class="cc-tag">°C · safe zone 2–8°C</span>
    </div>
    <div class="cc-wrap"><canvas id="tempChart" role="img" aria-label="Temperature trend">Temperature</canvas></div>
  </div>
  <div class="cc">
    <div class="cc-hdr">
      <span class="cc-title">CTS Drop Rate</span>
      <span class="cc-tag">points / min</span>
    </div>
    <div class="cc-wrap"><canvas id="dropChart" role="img" aria-label="CTS drop rate">Drop rate</canvas></div>
  </div>
</div>

<!-- Event log -->
<div class="log">
  <div class="log-hdr">
    <span class="log-title">Event Log</span>
    <span class="log-cnt" id="log-cnt">0 events</span>
  </div>
  <div id="log-list">
    <div class="li">
      <span class="li-pip" style="background:var(--safe)"></span>
      <span class="li-ts" id="start-ts">--:--:--</span>
      <span class="li-msg">Monitoring started</span>
    </div>
  </div>
</div>

<!-- Footer -->
<div class="foot">
  <span>ESP32 · DS18B20 · MPU6050 · Reed · SD · OLED</span>
  <span id="sample-cnt">0 samples</span>
</div>

</div><!-- /wrap -->
<script>
const MAX=40;
const lb=[],ta=[],ca=[],da=[];
let samples=0,logN=0;
let lCts=null,lTemp=null,lDoor=false,lMot=false;
let pCts=null,pCtsT=null;

function ts(){return new Date().toLocaleTimeString();}

function addLog(msg,col){
  const list=document.getElementById('log-list');
  const el=document.createElement('div');el.className='li';
  el.innerHTML=`<span class="li-pip" style="background:${col}"></span>`+
    `<span class="li-ts">${ts()}</span><span class="li-msg">${msg}</span>`;
  list.insertBefore(el,list.firstChild);
  if(list.children.length>12)list.removeChild(list.lastChild);
  logN++;document.getElementById('log-cnt').textContent=logN+' events';
}

const gOpts={
  responsive:true,maintainAspectRatio:false,animation:{duration:250},
  plugins:{legend:{display:false}},
  elements:{line:{tension:.35,borderWidth:1.5},point:{radius:0}}
};
const grid='rgba(255,255,255,0.05)';
const tick={color:'#7d8590',font:{family:"'DM Mono',monospace",size:9}};

const tempChart=new Chart(document.getElementById('tempChart'),{
  type:'line',
  data:{labels:lb,datasets:[
    {label:'Temp',data:ta,borderColor:'#58a6ff',backgroundColor:'rgba(88,166,255,.06)',fill:true},
    {label:'Lo',data:[],borderColor:'rgba(63,185,80,.4)',borderDash:[4,4],borderWidth:1,pointRadius:0,fill:false},
    {label:'Hi',data:[],borderColor:'rgba(248,81,73,.4)',borderDash:[4,4],borderWidth:1,pointRadius:0,fill:false}
  ]},
  options:{...gOpts,scales:{x:{display:false},y:{min:-3,max:14,grid:{color:grid},ticks:{...tick,maxTicksLimit:5},border:{color:'transparent'}}}}
});

const ctsChart=new Chart(document.getElementById('ctsChart'),{
  type:'line',
  data:{labels:lb,datasets:[{label:'CTS',data:ca,borderColor:'#3fb950',backgroundColor:'rgba(63,185,80,.06)',fill:true}]},
  options:{...gOpts,scales:{x:{display:false},y:{min:0,max:100,grid:{color:grid},ticks:{...tick,maxTicksLimit:4},border:{color:'transparent'}}}}
});

const dropChart=new Chart(document.getElementById('dropChart'),{
  type:'bar',
  data:{labels:lb,datasets:[{label:'Drop',data:da,backgroundColor:'rgba(248,81,73,.5)',borderColor:'#f85149',borderWidth:1,borderRadius:2}]},
  options:{responsive:true,maintainAspectRatio:false,animation:{duration:150},
    plugins:{legend:{display:false}},
    scales:{x:{display:false},y:{min:0,grid:{color:grid},ticks:{...tick,maxTicksLimit:4},border:{color:'transparent'}}}}
});

function push(arr,v){arr.push(v);if(arr.length>MAX)arr.shift();}

function ctsCol(v){return v>=70?'#3fb950':v>=40?'#d29922':'#f85149';}
function ctsBg(v) {return v>=70?'#3fb950':v>=40?'#d29922':'#f85149';}
function ctsCls(v){
  if(v>=80)return['Excellent','ts'];if(v>=70)return['Good','ts'];
  if(v>=40)return['At Risk','tw'];return['Compromised','td'];
}
function tCls(t){
  if(t>=2&&t<=8) return['In range','ts'];
  if(t>8&&t<=10) return['Warm','tw'];
  if(t<2&&t>=0)  return['Cool','tw'];
  if(t>10)       return['Hot — breach','td'];
  return['Freeze risk','td'];
}

function updateBanner(cts,temp,door,mot){
  const b=document.getElementById('banner');
  b.className='banner ';
  const bt=document.getElementById('b-title');
  const bs=document.getElementById('b-sub');
  if(cts<40){
    b.className+='danger';bt.textContent='Compromised — Escalate Immediately';
    bs.textContent='CTS critically low — chain integrity may be violated';
  }else if(cts<70){
    b.className+='warn';bt.textContent='At Risk — Review Conditions';
    bs.textContent='Conditions outside safe range — immediate attention required';
  }else{
    b.className+='safe';bt.textContent='Safe — Chain Integrity Maintained';
    bs.textContent='Temp '+temp.toFixed(1)+'°C · CTS '+cts.toFixed(1)+' · Door '+(door?'OPEN':'CLOSED')+' · Motion '+(mot?'YES':'NO');
  }
}

let fails=0;
async function update(){
  let d;
  try{const r=await fetch('/data');d=await r.json();fails=0;}
  catch(e){if(++fails===3)document.getElementById('updated').textContent='connection lost';return;}

  const{temp,cts,motion,door,doorOpenSec,motionEvents,breachEvents}=d;
  samples++;

  const now=ts();
  push(lb,now);push(ta,+temp.toFixed(2));push(ca,+cts.toFixed(2));

  // safe-zone reference lines
  [1,2].forEach(i=>{
    tempChart.data.datasets[i].data.push(i===1?2:8);
    if(tempChart.data.datasets[i].data.length>MAX)tempChart.data.datasets[i].data.shift();
  });

  // drop rate
  const nowMs=Date.now();
  let dr=0;
  if(pCts!==null&&pCtsT!==null){
    const dtm=(nowMs-pCtsT)/60000;
    dr=Math.max(0,(pCts-cts)/dtm);
  }
  pCts=cts;pCtsT=nowMs;
  push(da,+dr.toFixed(3));
  dropChart.data.datasets[0].backgroundColor=da.map(v=>
    v>3?'rgba(248,81,73,.75)':v>1?'rgba(210,153,34,.6)':'rgba(63,185,80,.4)');

  tempChart.update('none');ctsChart.update('none');dropChart.update('none');

  // metrics
  document.getElementById('m-temp').textContent   =temp.toFixed(1);
  document.getElementById('m-cts').textContent    =cts.toFixed(1);
  document.getElementById('m-breach').textContent =breachEvents??0;
  document.getElementById('m-motev').textContent  =motionEvents??0;
  document.getElementById('updated').textContent  =now;
  document.getElementById('sample-cnt').textContent=samples+' samples';

  const[tl,tc]=tCls(temp);const tt=document.getElementById('temp-tag');tt.textContent=tl;tt.className='mc-tag '+tc;
  const[cl,cc]=ctsCls(cts);const ct=document.getElementById('cts-tag');ct.textContent=cl;ct.className='mc-tag '+cc;

  const cn=document.getElementById('cts-big');cn.textContent=cts.toFixed(1);cn.style.color=ctsCol(cts);
  const bar=document.getElementById('cts-bar');bar.style.width=cts.toFixed(1)+'%';bar.style.background=ctsBg(cts);

  updateBanner(cts,temp,door,motion);

  // door
  const dw=document.getElementById('door-wrap');
  dw.className=door?'door-bad':'door-ok';
  document.getElementById('door-status').textContent=door?'Open':'Closed';
  document.getElementById('door-sub').textContent=door
    ?(doorOpenSec>20?`Open ${doorOpenSec}s — CTS penalty active`:`Open ${doorOpenSec}s — threshold in ${20-doorOpenSec}s`)
    :'Sealed — no entry';

  // motion
  const mw=document.getElementById('mot-wrap');
  mw.className=motion?'mot-act':'mot-still';
  document.getElementById('mot-status').textContent=motion?'Motion Detected':'Still';
  document.getElementById('mot-sub').textContent=motion?'Vibration/impact in progress':'No vibration detected';

  // log events
  if(door&&!lDoor) addLog('Door opened','#f85149');
  if(!door&&lDoor) addLog('Door closed','#3fb950');
  if(motion&&!lMot) addLog('Motion event detected','#d29922');
  if(cts<40&&(lCts===null||lCts>=40)) addLog('CTS critical — below 40 (Compromised)','#f85149');
  else if(cts<70&&(lCts===null||lCts>=70)) addLog('CTS at risk — below 70','#d29922');
  if((temp>8||temp<2)&&lTemp!==null&&lTemp>=2&&lTemp<=8)
    addLog('Temperature excursion: '+temp.toFixed(1)+'°C','#f85149');
  if(door&&doorOpenSec===20) addLog('Door open >20s — CTS drain active','#d29922');

  lCts=cts;lTemp=temp;lDoor=door;lMot=motion;
}

document.getElementById('start-ts').textContent=ts();
update();setInterval(update,1000);
</script>
</body>
</html>)rawliteral");

  client.stop();
}
