#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>


const char* WIFI_SSID = "iPhone17 max";
const char* WIFI_PASS = "00000000";
const char* LAPTOP_IP = "172.20.10.12";           
const uint16_t EVENT_PORT = 4210;     
const uint16_t CMD_PORT   = 4211;      

// Pins (ESP32-S3-DevKitC-1)
const int MOTOR   = 4;
const int BUZZER  = 5;
const int LED1    = 6;
const int BUTTON  = 7;
const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int LED2    = 10;                // set to -1 if both LEDs share GPIO6


float FI_ON    = 2.0f;                
float FI_OFF   = 1.0f;                 
float POWER_TH = 0.10f;              
const uint32_t MAX_FREEZE_MS = 20000;

// Cue
enum { CUE_BEAT = 0, CUE_VIBE = 1, CUE_VOICE = 2 };
int cueMode = CUE_BEAT;
const uint32_t BEAT_PERIOD = 545;    
const uint32_t BEAT_ON_MS  = 80;
const uint32_t VIBE_ON_MS  = 150;
const int BUZZ_CH   = 0;
const int BUZZ_FREQ = 2000;
const int DOSE_FREQ = 800;

// Sampling and spectrum
const uint8_t  MPU       = 0x68;
const uint32_t PERIOD_US = 15625;      
const int N   = 256;                  
const int HOP = 32;                    // new result every 0.5 s
const int LOCO_LO = 2,  LOCO_HI = 11;  // 0.5 to 2.75 Hz  (walking)
const int FRZ_LO  = 12, FRZ_HI  = 32;  // 3.0 to 8.0 Hz   (freezing)


WiFiUDP udp;
bool wifiUp = false;
uint32_t lastWifiCheck = 0;

float ring[N];
float x[N], hann[N], cosT[N], sinT[N];
int ringPos = 0, filled = 0, sinceHop = 0;
uint32_t nextSampleUs = 0;
int imuFails = 0;

bool freezing = false;
int onCount = 0, offCount = 0;
uint32_t freezeStart = 0;

bool cueActive = false;
int activeCue = CUE_BEAT;
bool beatOn = false, vibeOn = false;
uint32_t lastBeat = 0;

bool lastButton = HIGH;
uint32_t lastButtonMs = 0;
uint32_t blipUntil = 0;

String serialLine;


void ledsSet(bool on) {
  digitalWrite(LED1, on ? HIGH : LOW);
  if (LED2 >= 0) digitalWrite(LED2, on ? HIGH : LOW);
}

void buzzerTone(int freq) {            // 0 = off
  ledcWriteTone(BUZZ_CH, freq);
}

void sendEvent(const char* json) {
  Serial.println(json);
  if (!wifiUp) return;
  IPAddress dst(255, 255, 255, 255);
  if (strlen(LAPTOP_IP) > 0) dst.fromString(LAPTOP_IP);
  udp.beginPacket(dst, EVENT_PORT);
  udp.write((const uint8_t*)json, strlen(json));
  udp.endPacket();
}

void sendConfig() {
  char b[160];
  snprintf(b, sizeof(b),
           "{\"t\":\"cfg\",\"fion\":%.2f,\"fioff\":%.2f,\"pw\":%.3f,\"cue\":%d}",
           FI_ON, FI_OFF, POWER_TH, cueMode);
  sendEvent(b);
}


void beatPulse(uint32_t now) {
  lastBeat = now;
  beatOn = true;
  ledsSet(true);
  if (activeCue == CUE_BEAT) buzzerTone(BUZZ_FREQ);
  if (activeCue == CUE_BEAT || activeCue == CUE_VIBE) { digitalWrite(MOTOR, HIGH); vibeOn = true; }
  // CUE_VOICE: LEDs only, the laptop plays the spoken prompt
}

void cueStart(int mode) {
  activeCue = mode;
  cueActive = true;
  blipUntil = 0;
  beatPulse(millis());
}

void cueStop() {
  cueActive = false;
  beatOn = vibeOn = false;
  ledsSet(false);
  buzzerTone(0);
  digitalWrite(MOTOR, LOW);
}

void cueUpdate(uint32_t now) {
  if (!cueActive) return;
  if (beatOn && now - lastBeat >= BEAT_ON_MS) {
    beatOn = false;
    ledsSet(false);
    buzzerTone(0);
  }
  if (vibeOn && now - lastBeat >= VIBE_ON_MS) {
    vibeOn = false;
    digitalWrite(MOTOR, LOW);
  }
  if (now - lastBeat >= BEAT_PERIOD) beatPulse(now);
}


void startFreeze() {
  freezing = true;
  freezeStart = millis();
  offCount = 0;
  int mode = cueMode;
  if (mode == CUE_VOICE && !wifiUp) mode = CUE_BEAT;   // no laptop, no voice
  cueStart(mode);
  char b[96];
  snprintf(b, sizeof(b), "{\"t\":\"freeze_start\",\"ms\":%lu,\"cue\":%d}",
           (unsigned long)freezeStart, mode);
  sendEvent(b);
}

void endFreeze(const char* reason) {
  freezing = false;
  onCount = 0;
  uint32_t dur = millis() - freezeStart;
  cueStop();
  char b[128];
  snprintf(b, sizeof(b),
           "{\"t\":\"freeze_end\",\"ms\":%lu,\"dur\":%lu,\"cue\":%d,\"why\":\"%s\"}",
           (unsigned long)millis(), (unsigned long)dur, activeCue, reason);
  sendEvent(b);
}

void detect(float fi, float pw) {
  if (!freezing) {
    if (pw > POWER_TH && fi > FI_ON) onCount++; else onCount = 0;
    if (onCount >= 2) startFreeze();                 // 1 s of evidence
  } else {
    if (fi < FI_OFF || pw < POWER_TH * 0.5f) offCount++; else offCount = 0;
    if (offCount >= 2) endFreeze("resolved");
    else if (millis() - freezeStart > MAX_FREEZE_MS) endFreeze("timeout");
  }
}


void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void mpuInit() {
  mpuWrite(0x6B, 0x00);   // wake
  delay(100);
  mpuWrite(0x1A, 0x04);   // DLPF ~20 Hz: anti-alias and blocks motor buzz
  mpuWrite(0x1C, 0x08);   // +/-4 g
  mpuWrite(0x19, 0x00);   // fastest internal rate, we read at 64 Hz
}

bool readAccelMag(float& g) {
  Wire.beginTransmission(MPU);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU, (uint8_t)6) < 6) return false;
  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();
  float fx = ax / 8192.0f, fy = ay / 8192.0f, fz = az / 8192.0f;
  g = sqrtf(fx * fx + fy * fy + fz * fz);   // orientation independent
  return true;
}


void computeBands(float& fi, float& pw) {
  float mean = 0;
  for (int i = 0; i < N; i++) {
    x[i] = ring[(ringPos + i) % N];          // oldest to newest
    mean += x[i];
  }
  mean /= N;
  for (int i = 0; i < N; i++) x[i] = (x[i] - mean) * hann[i];

  float pl = 0, pf = 0;
  for (int k = LOCO_LO; k <= FRZ_HI; k++) {
    float re = 0, im = 0;
    int idx = 0;
    for (int n = 0; n < N; n++) {
      re += x[n] * cosT[idx];
      im -= x[n] * sinT[idx];
      idx += k;
      if (idx >= N) idx -= N;
    }
    float p = (re * re + im * im) / N;
    if (k <= LOCO_HI) pl += p; else pf += p;
  }
  pw = pl + pf;
  fi = pf / (pl + 1e-6f);
}


void handleCommand(String c) {
  c.trim();
  if (c.length() == 0) return;
  float v;
  int m;
  if (sscanf(c.c_str(), "cue %d", &m) == 1) {
    if (m >= 0 && m <= 2) cueMode = m;
  } else if (sscanf(c.c_str(), "fion %f", &v) == 1) {
    FI_ON = v;
  } else if (sscanf(c.c_str(), "fioff %f", &v) == 1) {
    FI_OFF = v;
  } else if (sscanf(c.c_str(), "pw %f", &v) == 1) {
    POWER_TH = v;
  } else if (c == "f") {
    if (!freezing) startFreeze();
  } else if (c == "s") {
    if (freezing) endFreeze("manual");
  } else if (c != "cfg") {
    Serial.println("commands: cue 0|1|2, fion X, fioff X, pw X, f, s, cfg");
    return;
  }
  sendConfig();
}

void serialUpdate() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (serialLine.length()) handleCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 64) {
      serialLine += ch;
    }
  }
}

void udpUpdate() {
  if (!wifiUp) return;
  int len = udp.parsePacket();
  if (len <= 0) return;
  char b[65];
  int n = udp.read(b, 64);
  if (n <= 0) return;
  b[n] = 0;
  handleCommand(String(b));
}

void buttonUpdate(uint32_t now) {
  bool s = digitalRead(BUTTON);
  if (lastButton == HIGH && s == LOW && now - lastButtonMs > 200) {
    lastButtonMs = now;
    char b[64];
    snprintf(b, sizeof(b), "{\"t\":\"dose\",\"ms\":%lu}", (unsigned long)now);
    sendEvent(b);
    if (!cueActive) {                         // short low blip as acknowledgment
      buzzerTone(DOSE_FREQ);
      blipUntil = now + 120;
    }
  }
  lastButton = s;
}

void blipUpdate(uint32_t now) {
  if (blipUntil && now >= blipUntil) {
    blipUntil = 0;
    if (!cueActive) buzzerTone(0);
  }
}

void wifiUpdate(uint32_t now) {
  if (now - lastWifiCheck < 500) return;
  lastWifiCheck = now;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiUp) {
    wifiUp = true;
    udp.begin(CMD_PORT);
    char b[96];
    snprintf(b, sizeof(b), "{\"t\":\"hello\",\"ip\":\"%s\"}",
             WiFi.localIP().toString().c_str());
    sendEvent(b);
    sendConfig();
  } else if (!up && wifiUp) {
    wifiUp = false;
    udp.stop();
  }
}


void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(MOTOR, OUTPUT);
  pinMode(LED1, OUTPUT);
  if (LED2 >= 0) pinMode(LED2, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  digitalWrite(MOTOR, LOW);
  ledsSet(false);

  ledcSetup(BUZZ_CH, BUZZ_FREQ, 8);
  ledcAttachPin(BUZZER, BUZZ_CH);
  buzzerTone(0);

  for (int i = 0; i < N; i++) {
    hann[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (N - 1)));
    cosT[i] = cosf(2.0f * PI * i / N);
    sinT[i] = sinf(2.0f * PI * i / N);
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  mpuInit();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.println("{\"t\":\"boot\"}");
  sendConfig();
  nextSampleUs = micros();
}

void loop() {
  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextSampleUs) >= 0) {
    nextSampleUs += PERIOD_US;
    if ((int32_t)(nowUs - nextSampleUs) > (int32_t)PERIOD_US) {
      nextSampleUs = nowUs + PERIOD_US;      // fell behind, resync
    }

    float g;
    if (readAccelMag(g)) {
      imuFails = 0;
      ring[ringPos] = g;
      ringPos = (ringPos + 1) % N;
      if (filled < N) filled++;
      sinceHop++;
    } else if (++imuFails == 64) {
      sendEvent("{\"t\":\"err\",\"msg\":\"imu not responding\"}");
    }

    if (filled == N && sinceHop >= HOP) {
      sinceHop = 0;
      float fi, pw;
      computeBands(fi, pw);
      detect(fi, pw);
      char b[96];
      snprintf(b, sizeof(b), "{\"t\":\"fi\",\"fi\":%.2f,\"pw\":%.3f,\"st\":%d}",
               fi, pw, freezing ? 1 : 0);
      sendEvent(b);
    }
  }

  uint32_t now = millis();
  cueUpdate(now);
  buttonUpdate(now);
  blipUpdate(now);
  serialUpdate();
  udpUpdate();
  wifiUpdate(now);
}
