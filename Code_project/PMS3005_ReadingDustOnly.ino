#include <U8g2lib.h>
#include <Wire.h>

// -------------------- PINS --------------------
#define PMS_RX 16
#define PMS_TX 17
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

// -------------------- OLED --------------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// -------------------- UART --------------------
HardwareSerial pmsSerial(2);

// -------------------- DATA STRUCT --------------------
struct PMS {
  uint16_t pm1_0;
  uint16_t pm2_5;
  uint16_t pm10;
} pms;

// -------------------- READ PMS5003 --------------------
bool readPMS() {
  static uint8_t buf[32];

  if (pmsSerial.available() < 32) return false;

  if (pmsSerial.peek() != 0x42) {
    pmsSerial.read();
    return false;
  }

  pmsSerial.readBytes(buf, 32);

  if (buf[0] != 0x42 || buf[1] != 0x4D) return false;

  uint16_t sum = 0;
  for (int i = 0; i < 30; i++) sum += buf[i];

  uint16_t frame_sum = (buf[30] << 8) | buf[31];
  if (sum != frame_sum) return false;

  // Atmospheric values
  pms.pm1_0 = (buf[10] << 8) | buf[11];
  pms.pm2_5 = (buf[12] << 8) | buf[13];
  pms.pm10  = (buf[14] << 8) | buf[15];

  return true;
}

// -------------------- DISPLAY --------------------
void draw() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  u8g2.drawStr(0, 12, "PM AIR QUALITY");

  char buf[20];

  sprintf(buf, "PM2.5: %d", pms.pm2_5);
  u8g2.drawStr(0, 32, buf);

  sprintf(buf, "ug/m3");
  u8g2.drawStr(80, 32, buf);

  sprintf(buf, "PM10 : %d", pms.pm10);
  u8g2.drawStr(0, 48, buf);

  u8g2.sendBuffer();
}

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(115200);

  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX, PMS_TX);

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 30, "Starting...");
  u8g2.sendBuffer();
}

// -------------------- LOOP --------------------
void loop() {

  if (readPMS()) {
    Serial.print("PM2.5: ");
    Serial.println(pms.pm2_5);

    draw();
  }

  delay(500);
}
