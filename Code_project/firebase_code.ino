#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------- WIFI ----------------
const char* ssid = "B";
const char* password = "00001111";

// ---------------- FIREBASE ----------------
const char* firebaseURL =
"https://team19-pmreader-default-rtdb.asia-southeast1.firebasedatabase.app/sensor/latest.json";

void setup() {
  Serial.begin(115200);
  Serial.println("BOOT OK");

  // Connect WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
}

void loop() {

  // ---------------- FAKE SENSOR DATA ----------------
  float pm25 = random(10, 80);
  float temp = 30 + random(-5, 5);
  float humidity = 60 + random(-10, 10);

  // ---------------- BUILD JSON (RIGHT WAY) ----------------
  StaticJsonDocument<200> doc;

  doc["pm25"] = pm25;
  doc["temp"] = temp;
  doc["humidity"] = humidity;

  String json;
  serializeJson(doc, json);

  Serial.println("JSON:");
  Serial.println(json);

  // ---------------- SEND TO FIREBASE ----------------
  if (WiFi.status() == WL_CONNECTED) {

    WiFiClient client;
    HTTPClient http;

    http.begin(firebaseURL);
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.PUT(json);

    Serial.print("HTTP Code: ");
    Serial.println(httpCode);

    if (httpCode > 0) {
      Serial.println(http.getString());
    }

    http.end();
  }

  delay(5000);
}
