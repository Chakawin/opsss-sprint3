// ลงไลบารตามนี้ U8g2 ArduinoJson TensorFlowLite_ESP32
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFiClientSecure.h>

// TinyML 
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "rain_model.h"  

// จำนวน ops ที่โมเดล Dense (FC -> ReLU -> FC -> ReLU -> FC -> Logistic) ต้องใช้
constexpr int NUM_TINYML_OPS = 3;

// ─────────────────────────────────────────────────────────
//  USER CONFIG
// ─────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Tanadon";
const char* WIFI_PASSWORD = "llllllll";
const char* API_KEY       = "45f616977bf863aa69b532f02b2def30";

String CHANNEL_ACCESS_TOKEN = "jjOJsKWjEEvtLNBZo7rtL8yQBf8Uu5UoNZvZKP+9mNwSvdvYVWpVToyxAXynYYvkZBYGx7V0hvvuzCV3nrP2AiDUZtzYUR2ck6TYY46wX6xHAB/jDptDi9gviCaO2aI2y92v0/GkjkuRgL+uQTKANAdB04t89/1O/w1cDnyilFU=";
String LINE_USER_ID        = "Ubff33259aeb3f3ae5d5d30f1d51e46de";

const float LATITUDE  = 13.3611;
const float LONGITUDE = 100.9847;

// ─────────────────────────────────────────────────────────
//  PIN CONFIG
// ─────────────────────────────────────────────────────────
#define PMS_RX      16
#define PMS_TX      17
#define BUZZER_PIN  25
#define OLED_SDA    21
#define OLED_SCL    22
#define OLED_ADDR   0x3C
#define BUTTON_PIN  18

// ─────────────────────────────────────────────────────────
//  TIMING
// ─────────────────────────────────────────────────────────
const unsigned long WEATHER_FETCH_MS = 60000UL;
const unsigned long LINE_REPORT_MS   = 86400000UL;

// ─────────────────────────────────────────────────────────
//  OLED
// ─────────────────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
HardwareSerial pmsSerial(2);

// ─────────────────────────────────────────────────────────
//  STRUCTS
// ─────────────────────────────────────────────────────────
struct WeatherData {
  char  desc[20];
  float tempC;
  float feelsLikeC;
  int   humidity;
  float windSpeed;
  int   windDeg;
  int   pressure;
  int   visibility;
  int   cloudiness;
  long  sunrise;
  long  sunset;
  bool  valid = false;
};

struct pms5003data {
  uint16_t framelen;
  uint16_t pm10_standard, pm25_standard, pm100_standard;
  uint16_t pm10_env, pm25_env, pm100_env;
  uint16_t particles_03um, particles_05um, particles_10um,
           particles_25um, particles_50um, particles_100um;
  uint16_t unused;
  uint16_t checksum;
};

// ─────────────────────────────────────────────────────────
//  GLOBAL STATE
// ─────────────────────────────────────────────────────────
WeatherData   weather;
pms5003data   pmsData;
int           currentPage      = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastLineReport   = 0;
unsigned long lastPmAlert      = 0;
bool          pmsValid         = false;
bool          lastButtonState  = HIGH;
float         rainProb         = -1.0f;  // ผลทำนายฝน 0.0-1.0

// ─────────────────────────────────────────────────────────
//  TINYML SETUP
// ─────────────────────────────────────────────────────────
constexpr int TENSOR_ARENA_SIZE = 12 * 1024;
uint8_t tensor_arena[TENSOR_ARENA_SIZE];

static tflite::MicroMutableOpResolver<NUM_TINYML_OPS> resolver;
const tflite::Model*     tf_model    = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
bool tinyml_ready = false;

void setupTinyML() {
  // esp-tflite-micro ไม่มี ErrorReporter object แยกแล้ว ใช้ MicroPrintf/MicroLog ภายในแทน
  tf_model = tflite::GetModel(rain_model);
  if (tf_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[TinyML] Schema version mismatch!");
    return;
  }

  // ลงทะเบียนเฉพาะ ops ที่โมเดล Dense นี้ใช้จริง (ดูได้จากชื่อ tensor ใน rain_model.h)
  if (resolver.AddFullyConnected() != kTfLiteOk) return;
  if (resolver.AddRelu() != kTfLiteOk) return;
  if (resolver.AddLogistic() != kTfLiteOk) return;

  static tflite::MicroInterpreter static_interpreter(
      tf_model, resolver, tensor_arena, TENSOR_ARENA_SIZE);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[TinyML] AllocateTensors failed!");
    return;
  }
  tinyml_ready = true;
  Serial.printf("[TinyML] พร้อม! ใช้ RAM %d bytes\n",
                interpreter->arena_used_bytes());
}

// ─────────────────────────────────────────────────────────
//  PREDICT RAIN
// ─────────────────────────────────────────────────────────
float predictRain(const WeatherData& wd) {
  if (!wd.valid || !tinyml_ready) return -1.0f;

  float raw[6] = {
    (float)wd.humidity,
    (float)wd.pressure,
    (float)wd.cloudiness,
    wd.windSpeed,
    wd.tempC,
    wd.windSpeed * 1.5f  // ประมาณ wind gust
  };

  TfLiteTensor* input = interpreter->input(0);
  for (int i = 0; i < 6; i++) {
    input->data.f[i] = (raw[i] - RAIN_SCALER_MEAN[i]) / RAIN_SCALER_SCALE[i];
  }

  if (interpreter->Invoke() != kTfLiteOk) return -1.0f;

  float prob = interpreter->output(0)->data.f[0];
  Serial.printf("[TinyML] โอกาสฝนตก: %.1f%%\n", prob * 100.0f);
  return prob;
}

const char* rainLabel(float prob) {
  if (prob < 0)    return "N/A";
  if (prob > 0.75) return "RAIN HIGH";
  if (prob > 0.50) return "RAIN MED";
  if (prob > 0.25) return "RAIN LOW";
  return "CLEAR";
}

// ─────────────────────────────────────────────────────────
//  FONT / HELPERS
// ─────────────────────────────────────────────────────────
#define FONT_SMALL u8g2_font_5x8_tr
#define LINE_H     8

void oledLine(uint8_t row, const char* txt) {
  u8g2.drawStr(0, (row + 1) * LINE_H, txt);
}

void beep(uint8_t ms) {
  digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
}

String windDir(int deg) {
  const char* d[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                     "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return String(d[(int)((deg + 11.25f) / 22.5f) % 16]);
}

String unixToTime(long ts) {
  long local = ts + 7L * 3600L;
  char buf[6];
  sprintf(buf, "%02d:%02d", (int)((local % 86400L) / 3600L),
                             (int)((local % 3600L) / 60L));
  return String(buf);
}

const char* aqiLabel(uint16_t pm25) {
  if (pm25 <= 12)  return "GOOD";
  if (pm25 <= 35)  return "MODERATE";
  if (pm25 <= 55)  return "SENSITIVE";
  if (pm25 <= 150) return "UNHEALTHY";
  if (pm25 <= 250) return "V.UNHLTHY";
  return                  "HAZARDOUS";
}

// ─────────────────────────────────────────────────────────
//  DRAW: AIR QUALITY (หน้า 1)
// ─────────────────────────────────────────────────────────
void drawAirPage(const pms5003data& d) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  char buf[32];

  oledLine(0, "2/3 AIR QUALITY");
  if (!pmsValid) { oledLine(2, "PMS5003 wait..."); u8g2.sendBuffer(); return; }

  snprintf(buf, sizeof(buf), "PM1.0:%d  PM2.5:%d", d.pm10_standard, d.pm25_standard);
  oledLine(1, buf);
  snprintf(buf, sizeof(buf), "PM10 :%d", d.pm100_standard);
  oledLine(2, buf);
  snprintf(buf, sizeof(buf), "PM2.5env:%d ug/m3", d.pm25_env);
  oledLine(3, buf);
  snprintf(buf, sizeof(buf), "PM10 env:%d ug/m3", d.pm100_env);
  oledLine(4, buf);
  snprintf(buf, sizeof(buf), ">0.3:%d >0.5:%d", d.particles_03um, d.particles_05um);
  oledLine(5, buf);
  snprintf(buf, sizeof(buf), "AQI:%-10s[2/3]", aqiLabel(d.pm25_env));
  oledLine(7, buf);
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  DRAW: WEATHER (หน้า 2)
// ─────────────────────────────────────────────────────────
void drawWeatherPage(const WeatherData& wd) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  char buf[32];

  oledLine(0, "3/3 WEATHER");
  if (!wd.valid) { oledLine(2, "Fetching..."); u8g2.sendBuffer(); return; }

  snprintf(buf, sizeof(buf), "Tmp:%.1fC Fl:%.1fC", wd.tempC, wd.feelsLikeC);
  oledLine(1, buf);
  snprintf(buf, sizeof(buf), "Hum:%d%% Prs:%dhPa", wd.humidity, wd.pressure);
  oledLine(2, buf);
  snprintf(buf, sizeof(buf), "Wind:%.1fm/s %s", wd.windSpeed, windDir(wd.windDeg).c_str());
  oledLine(3, buf);
  snprintf(buf, sizeof(buf), "Cld:%d%% Vis:%dm", wd.cloudiness, wd.visibility);
  oledLine(4, buf);
  snprintf(buf, sizeof(buf), "%-21.21s", wd.desc);
  oledLine(5, buf);
  snprintf(buf, sizeof(buf), "Rise:%s Set:%s",
           unixToTime(wd.sunrise).c_str(), unixToTime(wd.sunset).c_str());
  oledLine(6, buf);
  oledLine(7, "            [3/3]");
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  DRAW: AI RAIN PREDICTION (หน้า 3)
// ─────────────────────────────────────────────────────────
void drawRainPage(float prob) {
  u8g2.clearBuffer();

  // ── หัวข้อ ──
  u8g2.setFont(FONT_SMALL);
  oledLine(0, "1/3  AI RAIN FORECAST");
  u8g2.drawHLine(0, 9, 128);

  if (prob < 0) {
    u8g2.setFont(FONT_SMALL);
    oledLine(4, "      กำลังวิเคราะห์...");
    u8g2.sendBuffer();
    return;
  }

  int pct = (int)(prob * 100.0f + 0.5f);

  // ── เลขเปอร์เซ็นต์ตัวใหญ่ กึ่งกลางจอ ──
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  u8g2.setFont(u8g2_font_logisoso24_tr);
  int pctW = u8g2.getUTF8Width(pctBuf);
  int pctX = (128 - pctW) / 2;
  u8g2.drawStr(pctX, 38, pctBuf);

  // ── ไอคอนเล็ก ๆ ข้างเลข ตามระดับความเสี่ยง ──
  int iconX = pctX - 22;
  if (iconX < 2) iconX = 2;
  if (prob > 0.75) {
    // ก้อนเมฆ + เม็ดฝน
    u8g2.drawDisc(iconX + 6, 22, 5, U8G2_DRAW_ALL);
    u8g2.drawDisc(iconX + 12, 20, 6, U8G2_DRAW_ALL);
    u8g2.drawBox(iconX, 22, 16, 5);
    u8g2.drawLine(iconX + 3, 29, iconX + 1, 34);
    u8g2.drawLine(iconX + 9, 29, iconX + 7, 34);
    u8g2.drawLine(iconX + 15, 29, iconX + 13, 34);
  } else if (prob > 0.25) {
    // ก้อนเมฆเฉย ๆ
    u8g2.drawDisc(iconX + 6, 24, 5, U8G2_DRAW_ALL);
    u8g2.drawDisc(iconX + 12, 22, 6, U8G2_DRAW_ALL);
    u8g2.drawBox(iconX, 24, 16, 5);
  } else {
    // พระอาทิตย์
    u8g2.drawDisc(iconX + 8, 24, 5, U8G2_DRAW_ALL);
    u8g2.drawLine(iconX + 8, 14, iconX + 8, 17);
    u8g2.drawLine(iconX + 8, 31, iconX + 8, 34);
    u8g2.drawLine(iconX - 1, 24, iconX + 2, 24);
    u8g2.drawLine(iconX + 14, 24, iconX + 17, 24);
    u8g2.drawLine(iconX + 2, 18, iconX + 4, 20);
    u8g2.drawLine(iconX + 12, 28, iconX + 14, 30);
    u8g2.drawLine(iconX + 14, 18, iconX + 12, 20);
    u8g2.drawLine(iconX + 4, 28, iconX + 2, 30);
  }

  // ── Progress bar ──
  u8g2.drawRFrame(4, 46, 120, 8, 2);
  int barW = (int)((120 - 4) * prob);
  if (barW > 0) u8g2.drawRBox(6, 48, barW, 4, 1);

  // ── ข้อความสถานะ (Thai) กึ่งกลาง ──
  const char* msg;
  if      (prob > 0.75) msg = "โอกาสฝนตกสูง พกร่มด้วย";
  else if (prob > 0.50) msg = "โอกาสฝนตกปานกลาง";
  else if (prob > 0.25) msg = "โอกาสฝนตกน้อย";
  else                  msg = "ฟ้าใส โอกาสฝนตกน้อยมาก";

  u8g2.setFont(FONT_SMALL);
  int msgW = u8g2.getUTF8Width(msg);
  u8g2.drawUTF8((128 - msgW) / 2, 62, msg);

  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  DRAW: STATUS
// ─────────────────────────────────────────────────────────
void drawStatusPage(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  oledLine(0, "OPSSS Weather Station");
  oledLine(2, msg);
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  FETCH WEATHER
// ─────────────────────────────────────────────────────────
bool fetchWeather(WeatherData& wd) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String url = String("https://api.openweathermap.org/data/2.5/weather")
             + "?lat=" + String(LATITUDE, 4)
             + "&lon=" + String(LONGITUDE, 4)
             + "&appid=" + String(API_KEY)
             + "&units=metric";

  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, payload)) return false;

  strlcpy(wd.desc, doc["weather"][0]["main"] | "N/A", sizeof(wd.desc));
  wd.tempC      = doc["main"]["temp"]       | 0.0f;
  wd.feelsLikeC = doc["main"]["feels_like"] | 0.0f;
  wd.humidity   = doc["main"]["humidity"]   | 0;
  wd.pressure   = doc["main"]["pressure"]   | 0;
  wd.windSpeed  = doc["wind"]["speed"]      | 0.0f;
  wd.windDeg    = doc["wind"]["deg"]        | 0;
  wd.cloudiness = doc["clouds"]["all"]      | 0;
  wd.visibility = doc["visibility"]         | 0;
  wd.sunrise    = doc["sys"]["sunrise"]     | 0L;
  wd.sunset     = doc["sys"]["sunset"]      | 0L;
  wd.valid      = true;
  return true;
}

// ─────────────────────────────────────────────────────────
//  READ PMS5003
// ─────────────────────────────────────────────────────────
boolean readPMSdata(Stream* s) {
  if (!s->available()) return false;
  if (s->peek() != 0x42) { s->read(); return false; }
  if (s->available() < 32) return false;

  uint8_t  buffer[32];
  uint16_t sum = 0;
  s->readBytes(buffer, 32);
  for (uint8_t i = 0; i < 30; i++) sum += buffer[i];

  uint16_t buf16[15];
  for (uint8_t i = 0; i < 15; i++) {
    buf16[i]  = buffer[2 + i*2 + 1];
    buf16[i] += (buffer[2 + i*2] << 8);
  }
  memcpy((void*)&pmsData, (void*)buf16, 30);
  return sum == pmsData.checksum;
}

// ─────────────────────────────────────────────────────────
//  LINE: ส่งข้อความ
// ─────────────────────────────────────────────────────────
void sendLineMessage(String message) {
  if (WiFi.status() != WL_CONNECTED) return;
  message.replace("\\", "\\\\");
  message.replace("\"", "\\\"");
  message.replace("\n", "\\n");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (https.begin(client, "https://api.line.me/v2/bot/message/push")) {
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer " + CHANNEL_ACCESS_TOKEN);
    String payload = "{\"to\":\"" + LINE_USER_ID +
                     "\",\"messages\":[{\"type\":\"text\",\"text\":\"" +
                     message + "\"}]}";
    int code = https.POST(payload);
    Serial.printf("[LINE] HTTP %d\n", code);
    https.end();
  }
}

// ─────────────────────────────────────────────────────────
//  LINE: สร้างรายงาน (รวม AI ทำนายฝน)
// ─────────────────────────────────────────────────────────
String buildLineReport() {
  String msg = "📊 รายงานสภาพอากาศ ชลบุรี\n";
  msg += "━━━━━━━━━━━━━━\n";

  if (weather.valid) {
    msg += "🌤 สภาพอากาศ: " + String(weather.desc) + "\n";
    msg += "🌡 อุณหภูมิ: " + String(weather.tempC, 1) + "°C\n";
    msg += "💧 ความชื้น: " + String(weather.humidity) + "%\n";
    msg += "💨 ลม: " + String(weather.windSpeed, 1) + " m/s ทิศ" + windDir(weather.windDeg) + "\n";
  } else {
    msg += "⚠️ ไม่มีข้อมูลสภาพอากาศ\n";
  }

  msg += "━━━━━━━━━━━━━━\n";
  if (pmsValid) {
    msg += "😷 PM2.5: " + String(pmsData.pm25_env) + " µg/m³  ";
    msg += "PM10: " + String(pmsData.pm100_env) + " µg/m³\n";
    msg += "📈 AQI: " + String(aqiLabel(pmsData.pm25_env)) + "\n";
  }

  // AI ทำนายฝน (on-device)
  msg += "━━━━━━━━━━━━━━\n";
  msg += "🤖 AI ทำนาย (on-device):\n";
  if (rainProb >= 0) {
    int pct = (int)(rainProb * 100.0f);
    if      (rainProb > 0.75) msg += "🌧 โอกาสฝนตก " + String(pct) + "% (สูง) — พกร่มด้วย!\n";
    else if (rainProb > 0.50) msg += "🌦 โอกาสฝนตก " + String(pct) + "% (ปานกลาง)\n";
    else if (rainProb > 0.25) msg += "⛅ โอกาสฝนตก " + String(pct) + "% (น้อย)\n";
    else                      msg += "☀️ โอกาสฝนตก " + String(pct) + "% (ฟ้าใส)\n";
  } else {
    msg += "⚠️ โมเดลยังไม่พร้อม\n";
  }

  return msg;
}

// ─────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.setI2CAddress(OLED_ADDR * 2);
  u8g2.begin();
  u8g2.setContrast(200);

  drawStatusPage("Connecting WiFi..");
  beep(50); delay(100); beep(50);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    if (++attempts > 40) { drawStatusPage("WiFi failed!"); delay(2000); ESP.restart(); }
  }

  drawStatusPage("Loading AI model..");
  setupTinyML();

  drawStatusPage("Fetching weather..");
  fetchWeather(weather);

  rainProb = predictRain(weather);

  drawStatusPage("รอเซ็นเซอร์ฝุ่น..");
  unsigned long pmsWait = millis();
  while (!pmsValid && millis() - pmsWait < 10000) {
    if (readPMSdata(&pmsSerial)) pmsValid = true;
    delay(10);
  }

  lastWeatherFetch = millis();
  lastLineReport   = 0;

  delay(500);
  sendLineMessage(buildLineReport());

  currentPage = 0;
  drawRainPage(rainProb);
}

// ─────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── ปุ่ม: สลับหน้า 3 หน้า ──────────────────────────────
  bool buttonState = digitalRead(BUTTON_PIN);
  if (lastButtonState == HIGH && buttonState == LOW) {
    currentPage = (currentPage + 1) % 3;
    if      (currentPage == 0) drawRainPage(rainProb);
    else if (currentPage == 1) drawAirPage(pmsData);
    else                       drawWeatherPage(weather);
    delay(200);
  }
  lastButtonState = buttonState;

  // ── อ่าน PMS5003 ─────────────────────────────────────
  if (readPMSdata(&pmsSerial)) {
    pmsValid = true;
    if (currentPage == 1) drawAirPage(pmsData);
    if (pmsData.particles_25um > 55) beep(min((int)pmsData.particles_25um, 255));
  }

  // ── ดึง Weather + ทำนายฝนใหม่ทุก 60 วิ ──────────────
  if (now - lastWeatherFetch >= WEATHER_FETCH_MS) {
    lastWeatherFetch = now;
    if (fetchWeather(weather)) {
      rainProb = predictRain(weather);
      if (currentPage == 2) drawWeatherPage(weather);
      if (currentPage == 0) drawRainPage(rainProb);
    }
  }

  // ── LINE รายงานทุกวัน ─────────────────────────────────
  if (now - lastLineReport >= LINE_REPORT_MS) {
    lastLineReport = now;
    sendLineMessage(buildLineReport());
  }

  // ── แจ้งเตือน PM2.5 ──────────────────────────────────
  if (pmsValid && pmsData.particles_25um > 55) {
    if (lastPmAlert == 0 || now - lastPmAlert >= 600000UL) {
      lastPmAlert = now;
      sendLineMessage(
        "🚨 แจ้งเตือน PM2.5 สูง!\n"
        "━━━━━━━━━━━━━━\n"
        "PM2.5 = " + String(pmsData.particles_25um) + " หน่วย\n"
        "ระดับ: " + String(aqiLabel(pmsData.particles_25um)) + "\n"
        "⚠️ ควรสวมหน้ากาก N95"
      );
    }
  }

  // ── แจ้งเตือนฝนตก ────────────────────────────────────
  if (rainProb > 0.75) {
    static unsigned long lastRainAlert = 0;
    if (lastRainAlert == 0 || now - lastRainAlert >= 3600000UL) {
      lastRainAlert = now;
      sendLineMessage(
        "🌧 แจ้งเตือน: ฝนกำลังจะตก!\n"
        "━━━━━━━━━━━━━━\n"
        "AI ทำนายโอกาสฝนตก " + String((int)(rainProb*100)) + "%\n"
        "☂️ ควรพกร่มออกนอกบ้าน"
      );
    }
  }

  delay(10);
}
