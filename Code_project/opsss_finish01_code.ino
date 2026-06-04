//อ่านก่อนกดอัพโหลด
//ลงไลบารตามนี้ U8g2 ArduinoJson 
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>

// ─────────────────────────────────────────────────────────
//  USER CONFIG
// ─────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Tanadon";
const char* WIFI_PASSWORD = "llllllll";
const char* API_KEY       = "45f616977bf863aa69b532f02b2def30";

const float LATITUDE      = 13.7563;
const float LONGITUDE     = 100.5018;
const char* CITY_NAME     = "Bangkok";

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
const unsigned long PAGE_SWITCH_MS   = 20000UL;
const unsigned long WEATHER_FETCH_MS = 60000UL;

// ─────────────────────────────────────────────────────────
//  OLED — SH1106 128x64 I2C 
// ─────────────────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

HardwareSerial pmsSerial(2);

// ─────────────────────────────────────────────────────────
//  STRUCTS
// ─────────────────────────────────────────────────────────
struct WeatherData {
  char   desc[20];
  float  tempC;
  float  feelsLikeC;
  int    humidity;
  float  windSpeed;
  int    windDeg;
  int    pressure;
  int    visibility;
  int    cloudiness;
  long   sunrise;
  long   sunset;
  bool   valid = false;
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
unsigned long lastPageSwitch   = 0;
unsigned long lastWeatherFetch = 0;
bool          pmsValid         = false;
bool          lastButtonState  = HIGH;  

// ─────────────────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────────────────
String  buildURL();
String  windDir(int deg);
String  unixToTime(long ts);
bool    fetchWeather(WeatherData& wd);
void    drawWeatherPage(const WeatherData& wd);
void    drawAirPage(const pms5003data& d);
void    drawStatusPage(const char* msg);
const char* aqiLabel(uint16_t pm25);
boolean readPMSdata(Stream *s);
void    beep(uint8_t ms);

// ─────────────────────────────────────────────────────────
//  FONT ที่ใช้
// ─────────────────────────────────────────────────────────
#define FONT_SMALL  u8g2_font_5x8_tr
#define LINE_H      8
#define COL_MAX     21
#define ROWS        8

// ─────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────
void beep(uint8_t ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

String buildURL() {
  return String("https://api.openweathermap.org/data/2.5/weather")
       + "?lat=" + String(LATITUDE, 4)
       + "&lon=" + String(LONGITUDE, 4)
       + "&appid=" + String(API_KEY)
       + "&units=metric";
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
                             (int)((local % 3600L)  / 60L));
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

void oledLine(uint8_t row, const char* txt) {
  u8g2.drawStr(0, (row + 1) * LINE_H, txt);
}

void oledLinef(uint8_t row, const char* fmt, ...) {
  char buf[32];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  u8g2.drawStr(0, (row + 1) * LINE_H, buf);
}

void oledHLine(uint8_t row) {
  u8g2.drawHLine(0, row * LINE_H + 1, 128);
}

// ─────────────────────────────────────────────────────────
//  DRAW: Air qc
// ─────────────────────────────────────────────────────────
void drawAirPage(const pms5003data& d) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  char buf[32];

  oledLine(0, "1/2 AIR QUALITY");

  if (!pmsValid) {
    oledLine(2, "PMS5003 wait...");
    u8g2.sendBuffer();
    return;
  }

  snprintf(buf, sizeof(buf), "PM1.0:%d  PM2.5:%d", d.pm10_standard, d.pm25_standard);
  oledLine(1, buf);

  snprintf(buf, sizeof(buf), "PM10 :%d  [std ug/m3]", d.pm100_standard);
  oledLine(2, buf);

  snprintf(buf, sizeof(buf), "PM2.5env:%d ug/m3", d.pm25_env);
  oledLine(3, buf);

  snprintf(buf, sizeof(buf), "PM10 env:%d ug/m3", d.pm100_env);
  oledLine(4, buf);

  snprintf(buf, sizeof(buf), ">0.3:%d  >0.5:%d", d.particles_03um, d.particles_05um);
  oledLine(5, buf);

  snprintf(buf, sizeof(buf), ">2.5:%d  >10 :%d", d.particles_25um, d.particles_100um);
  oledLine(6, buf);

  snprintf(buf, sizeof(buf), "AQI: %-9s [2/2]", aqiLabel(d.pm25_env));
  oledLine(7, buf);

  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  DRAW: WEATHER PAGE
// ─────────────────────────────────────────────────────────
void drawWeatherPage(const WeatherData& wd) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  char buf[32];

  oledLine(0, "2/2 WEATHER");

  if (!wd.valid) {
    oledLine(2, "Fetching data...");
    u8g2.sendBuffer();
    return;
  }

  snprintf(buf, sizeof(buf), "Tmp:%.1fC Fl:%.1fC", wd.tempC, wd.feelsLikeC);
  oledLine(1, buf);

  snprintf(buf, sizeof(buf), "Hum:%d%%  Prs:%dhPa", wd.humidity, wd.pressure);
  oledLine(2, buf);

  snprintf(buf, sizeof(buf), "Wind:%.1fm/s %s", wd.windSpeed, windDir(wd.windDeg).c_str());
  oledLine(3, buf);

  snprintf(buf, sizeof(buf), "Cld:%d%%  Vis:%dm", wd.cloudiness, wd.visibility);
  oledLine(4, buf);

  snprintf(buf, sizeof(buf), "%-21.21s", wd.desc);
  oledLine(5, buf);

  snprintf(buf, sizeof(buf), "Rise:%s Set:%s",
           unixToTime(wd.sunrise).c_str(),
           unixToTime(wd.sunset).c_str());
  oledLine(6, buf);

  oledLine(7, "Next:AirQ   [1/2]");
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  DRAW: STATUS / BOOT PAGE
// ─────────────────────────────────────────────────────────
void drawStatusPage(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_SMALL);
  oledLine(0, "BKK Weather Station");
  oledLine(2, msg);
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────
//  FETCH WEATHER
// ─────────────────────────────────────────────────────────
bool fetchWeather(WeatherData& wd) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(buildURL());
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, payload)) return false;

  const char* main_w = doc["weather"][0]["main"] | "N/A";
  strlcpy(wd.desc, main_w, sizeof(wd.desc));

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

  Serial.printf("[Weather] %.1fC  %s\n", wd.tempC, wd.desc);
  return true;
}

// ─────────────────────────────────────────────────────────
//  READ PMS5003
// ─────────────────────────────────────────────────────────
boolean readPMSdata(Stream *s) {
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

  if (sum != pmsData.checksum) {
    Serial.println("[PMS] Checksum fail");
    return false;
  }
  return true;
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
  Serial.printf("WiFi: %s\n", WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempts > 40) {
      drawStatusPage("WiFi failed!");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  drawStatusPage("Fetching weather..");

  fetchWeather(weather);
  lastWeatherFetch = millis();
  lastPageSwitch   = millis();

  currentPage = 1;
drawAirPage(pmsData);
}

// ─────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── อ่านปุ่ม ──────────────────────────────────────────
  bool buttonState = digitalRead(BUTTON_PIN);   

  if (lastButtonState == HIGH && buttonState == LOW) {
    currentPage = 1 - currentPage;
    if (currentPage == 0)
      drawWeatherPage(weather);
    else
      drawAirPage(pmsData);
    delay(200);  // debounce
  }
  lastButtonState = buttonState;

  // ── อ่าน PMS5003 ─────────────────────────────────────
  if (readPMSdata(&pmsSerial)) {
    pmsValid = true;
    if (currentPage == 1) drawAirPage(pmsData); 
    if (pmsData.particles_25um > 50) {
        uint8_t dur = (pmsData.particles_25um < 255)
                      ? (uint8_t)pmsData.particles_25um : 255;
        beep(dur);
    }
}

  // ── ดึง Weather API ทุก 60 วิ ────────────────────────
  if (now - lastWeatherFetch >= WEATHER_FETCH_MS) {
    lastWeatherFetch = now;
    fetchWeather(weather);
    if (currentPage == 0) drawWeatherPage(weather);
  }

  delay(10);
}
