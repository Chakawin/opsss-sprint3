//อ่านก่อนกดอัพโหลดไม่งั้นมันจะ Error
//ลงไลบารีตามนี้ ArduinoJson U8g2
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>

// ─────────────────────────────────────────────────────────
//  USER CONFIG
// ─────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Tanadon"; //เปลี่ยนตาม wifi ที่เพื่อนใช้
const char* WIFI_PASSWORD = "llllllll"; //รหัส wifi
const char* API_KEY       = "45f616977bf863aa69b532f02b2def30"; //api ที่เราสมัครมาห้ามเปลี่ยน

const float LATITUDE  = 13.7563;
const float LONGITUDE = 100.5018;

// ─────────────────────────────────────────────────────────
//  PIN CONFIG
// ─────────────────────────────────────────────────────────
#define PMS_RX     16
#define PMS_TX     17
#define BUZZER_PIN 25
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_ADDR  0x3C   // ลอง 0x3D ถ้าจอไม่ติด

// ─────────────────────────────────────────────────────────
//  TIMING
// ─────────────────────────────────────────────────────────
const unsigned long PAGE_SWITCH_MS   = 20000UL;
const unsigned long WEATHER_FETCH_MS = 60000UL;

// ─────────────────────────────────────────────────────────
//  OLED
//  SH1106 → U8G2_SH1106_128X64_NONAME_F_HW_I2C
//  SSD1306 → U8G2_SSD1306_128X64_NONAME_F_HW_I2C
// ─────────────────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C
  u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

HardwareSerial pmsSerial(2);

// ─────────────────────────────────────────────────────────
//  FONT LAYOUT
//  unifont_t_thai = 16x16px
#define FONT_THAI  u8g2_font_unifont_t_symbols
#define LH         16  
#define ROWS       4   

// ─────────────────────────────────────────────────────────
//  STRUCTS
// ─────────────────────────────────────────────────────────
struct WeatherData {
  char  desc[32];      
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
unsigned long lastPageSwitch   = 0;
unsigned long lastWeatherFetch = 0;
bool          pmsValid         = false;

// ─────────────────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────────────────
String  buildURL();
String  windDir(int deg);
String  unixToTime(long ts);
bool    fetchWeather(WeatherData& wd);
void    drawPage(int page);
void    drawWeather1(const WeatherData& wd);
void    drawWeather2(const WeatherData& wd);
void    drawAir1(const pms5003data& d);
void    drawAir2(const pms5003data& d);
void    drawBoot(const char* msg);
const char* aqiThai(uint16_t pm25);
boolean readPMSdata(Stream *s);
void    beep(uint8_t ms);

// ─────────────────────────────────────────────────────────
//  OLED HELPERS
// ─────────────────────────────────────────────────────────

void oledPrint(uint8_t row, const char* txt) {
  u8g2.drawUTF8(0, (row + 1) * LH, txt);
}


void oledPrintf(uint8_t row, const char* fmt, ...) {
  char buf[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  u8g2.drawUTF8(0, (row + 1) * LH, buf);
}


void oledLine(uint8_t afterRow) {
  u8g2.drawHLine(0, afterRow * LH + LH/2, 128);
}

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
       + "&units=metric&lang=th";
}

String windDir(int deg) {
  const char* d[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                     "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return String(d[(int)((deg + 11.25f) / 22.5f) % 16]);
}

String unixToTime(long ts) {
  long local = ts + 7L * 3600L;
  char buf[6];
  sprintf(buf, "%02d:%02d",
          (int)((local % 86400L) / 3600L),
          (int)((local % 3600L)  / 60L));
  return String(buf);
}

const char* aqiThai(uint16_t pm25) {
  if (pm25 <= 12)  return "ดีมาก";
  if (pm25 <= 35)  return "ดี";
  if (pm25 <= 55)  return "ปานกลาง";
  if (pm25 <= 150) return "ไม่ดีต่อสุขภาพ";
  if (pm25 <= 250) return "แย่มาก";
  return                  "อันตราย";
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

  // description ภาษาไทย (lang=th)
  const char* raw = doc["weather"][0]["description"] | "N/A";
  strlcpy(wd.desc, raw, sizeof(wd.desc));

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
//  DRAW PAGES
// ── WEATHER หน้า 1 ──────────────────────────────────────
// บรรทัด 0: "สภาพอากาศ กทม. [1/4]"
// บรรทัด 1: "อุณหภูมิ 33.4°C"
// บรรทัด 2: "รู้สึก 38.1°C ชื้น 71%"
// บรรทัด 3: "ลม 3.1 m/s SSE"
void drawWeather1(const WeatherData& wd) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_THAI);

  oledPrint(0, "สภาพอากาศ กทม. [1/4]");

  if (!wd.valid) {
    oledPrint(2, "กำลังโหลด...");
    u8g2.sendBuffer();
    return;
  }

  char buf[48];
  snprintf(buf, sizeof(buf), "อุณหภูมิ %.1f\xc2\xb0""C", wd.tempC);
  oledPrint(1, buf);

  snprintf(buf, sizeof(buf), "รู้สึก %.1f\xc2\xb0""C ชื้น %d%%", wd.feelsLikeC, wd.humidity);
  oledPrint(2, buf);

  snprintf(buf, sizeof(buf), "ลม %.1f m/s %s", wd.windSpeed, windDir(wd.windDeg).c_str());
  oledPrint(3, buf);

  u8g2.sendBuffer();
}

// ── WEATHER หน้า 2 ──────────────────────────────────────
// บรรทัด 0: "สภาพอากาศ กทม. [2/4]"
// บรรทัด 1: "มีเมฆมาก  ความดัน 1007"
// บรรทัด 2: "เมฆ 80%  ทัศน์ 9000m"
// บรรทัด 3: "ขึ้น 05:58  ตก 18:42"
void drawWeather2(const WeatherData& wd) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_THAI);

  oledPrint(0, "สภาพอากาศ กทม. [2/4]");

  if (!wd.valid) {
    oledPrint(2, "กำลังโหลด...");
    u8g2.sendBuffer();
    return;
  }

  char buf[48];

  snprintf(buf, sizeof(buf), "%s", wd.desc);
  oledPrint(1, buf);

  snprintf(buf, sizeof(buf), "เมฆ %d%%  ทัศน์ %dm", wd.cloudiness, wd.visibility);
  oledPrint(2, buf);

  snprintf(buf, sizeof(buf), "ขึ้น %s  ตก %s",
           unixToTime(wd.sunrise).c_str(),
           unixToTime(wd.sunset).c_str());
  oledPrint(3, buf);

  u8g2.sendBuffer();
}

// ── AIR QUALITY หน้า 1 ──────────────────────────────────
// บรรทัด 0: "คุณภาพอากาศ [3/4]"
// บรรทัด 1: "PM1.0: 12  PM2.5: 35"
// บรรทัด 2: "PM10:  48  ug/m3"
// บรรทัด 3: "สถานะ: ดี"
void drawAir1(const pms5003data& d) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_THAI);

  oledPrint(0, "คุณภาพอากาศ [3/4]");

  if (!pmsValid) {
    oledPrint(2, "รอข้อมูล PMS5003...");
    u8g2.sendBuffer();
    return;
  }

  char buf[48];
  snprintf(buf, sizeof(buf), "PM1.0:%d  PM2.5:%d", d.pm10_standard, d.pm25_standard);
  oledPrint(1, buf);

  snprintf(buf, sizeof(buf), "PM10: %d  ug/m3", d.pm100_standard);
  oledPrint(2, buf);

  snprintf(buf, sizeof(buf), "สถานะ: %s", aqiThai(d.pm25_env));
  oledPrint(3, buf);

  u8g2.sendBuffer();
}

// ── AIR QUALITY หน้า 2 ──────────────────────────────────
// บรรทัด 0: "อนุภาค (ต่อ 0.1L) [4/4]"
// บรรทัด 1: ">0.3um: 1842  >0.5um: 524"
// บรรทัด 2: ">2.5um: 58   >10um:  4"
// บรรทัด 3: "PM2.5env: 35  PM10: 48"
void drawAir2(const pms5003data& d) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_THAI);

  oledPrint(0, "อนุภาค/0.1L [4/4]");

  if (!pmsValid) {
    oledPrint(2, "รอข้อมูล PMS5003...");
    u8g2.sendBuffer();
    return;
  }

  char buf[48];
  snprintf(buf, sizeof(buf), ">0.3:%d >0.5:%d", d.particles_03um, d.particles_05um);
  oledPrint(1, buf);

  snprintf(buf, sizeof(buf), ">2.5:%d  >10:%d", d.particles_25um, d.particles_100um);
  oledPrint(2, buf);

  snprintf(buf, sizeof(buf), "PM2.5:%d PM10:%d ug", d.pm25_env, d.pm100_env);
  oledPrint(3, buf);

  u8g2.sendBuffer();
}

// ── BOOT / STATUS PAGE ──────────────────────────────────
void drawBoot(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_THAI);
  oledPrint(0, "สถานีอากาศ กทม.");
  oledPrint(2, msg);
  u8g2.sendBuffer();
}

// ── ROUTER ──────────────────────────────────────────────
void drawPage(int page) {
  switch (page) {
    case 0: drawWeather1(weather); break;
    case 1: drawWeather2(weather); break;
    case 2: drawAir1(pmsData);     break;
    case 3: drawAir2(pmsData);     break;
  }
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

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.setI2CAddress(OLED_ADDR * 2);
  u8g2.begin();
  u8g2.enableUTF8Print();   // เปิด UTF-8 สำหรับภาษาไทย
  u8g2.setContrast(200);

  drawBoot("กำลังเชื่อมต่อ WiFi");

  beep(50); delay(100); beep(50);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi: %s\n", WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempts > 40) {
      drawBoot("WiFi ล้มเหลว!");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  drawBoot("กำลังดึงข้อมูลอากาศ");
  fetchWeather(weather);
  lastWeatherFetch = millis();
  lastPageSwitch   = millis();

  drawPage(0);
}

// ─────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // อ่าน PMS5003 ตลอดเวลา (non-blocking)
  if (readPMSdata(&pmsSerial)) {
    pmsValid = true;
    if (pmsData.particles_25um > 50) { //buzzer ร้องเตืนอ
      uint8_t dur = (pmsData.particles_25um < 255)
                    ? (uint8_t)pmsData.particles_25um : 255;
      beep(dur);
    }
  }

  // ดึง Weather API ทุก 60 วิ
  if (now - lastWeatherFetch >= WEATHER_FETCH_MS) {
    lastWeatherFetch = now;
    if (fetchWeather(weather)) {
      // อัปเดตจอถ้าอยู่หน้า weather
      if (currentPage == 0 || currentPage == 1)
        drawPage(currentPage);
    }
  }

  // สลับหน้าทุก 20 วิ  (4 หน้าวนซ้ำ: 0→1→2→3→0)
  if (now - lastPageSwitch >= PAGE_SWITCH_MS) {
    lastPageSwitch = now;
    currentPage    = (currentPage + 1) % 4;
    drawPage(currentPage);
  }

  delay(10);
}
