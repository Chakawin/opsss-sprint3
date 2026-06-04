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
  }

  // ── ดึง Weather API ทุก 60 วิ ────────────────────────
  if (now - lastWeatherFetch >= WEATHER_FETCH_MS) {
    lastWeatherFetch = now;
    fetchWeather(weather);
    if (currentPage == 0) drawWeatherPage(weather);
  }

  delay(10);
}
