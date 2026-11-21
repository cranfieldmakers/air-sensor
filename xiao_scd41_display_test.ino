#include <Wire.h>

// XIAO ESP32-S3 buses:
//   OLED:   GPIO1 SDA, GPIO2 SCL, address 0x3C
//   SCD41:  GPIO3 SDA, GPIO4 SCL ("SDC" in the wiring note assumed to mean SCL)
TwoWire SensorBus(1);

constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t SCD41_ADDR = 0x62;
constexpr int OLED_SDA_PIN = 1;
constexpr int OLED_SCL_PIN = 2;
constexpr int SENSOR_SDA_PIN = 3;
constexpr int SENSOR_SCL_PIN = 4;
constexpr int WIDTH = 128;
constexpr int HEIGHT = 64;

constexpr uint16_t CMD_START_PERIODIC = 0x21B1;
constexpr uint16_t CMD_STOP_PERIODIC = 0x3F86;
constexpr uint16_t CMD_GET_VARIANT = 0x202F;
constexpr uint16_t CMD_GET_READY = 0xE4B8;
constexpr uint16_t CMD_READ_MEASUREMENT = 0xEC05;

uint8_t frameBuffer[WIDTH * HEIGHT / 8];
bool oledAvailable = false;
bool sensorAvailable = false;
bool sensorIsSCD41 = false;
bool measurementRunning = false;
bool haveReading = false;
uint16_t co2ppm = 0;
float temperatureC = 0.0f;
float humidityPct = 0.0f;
uint32_t lastReadyPoll = 0;
uint32_t lastScreenUpdate = 0;

uint8_t sendDisplayCommands(const uint8_t *commands, size_t count) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);
  Wire.write(commands, count);
  return Wire.endTransmission();
}

bool probe(TwoWire &bus, uint8_t address) {
  bus.beginTransmission(address);
  return bus.endTransmission() == 0;
}

void setPixel(int x, int y) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  frameBuffer[x + (y / 8) * WIDTH] |= static_cast<uint8_t>(1U << (y & 7));
}

void drawHLine(int x, int y, int width) {
  for (int i = 0; i < width; ++i) setPixel(x + i, y);
}

// 5x7 uppercase font, stored as five vertical columns.
bool glyph(char c, uint8_t out[5]) {
  static const uint8_t font[36][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x62,0x51,0x49,0x49,0x46}, // 2
    {0x22,0x41,0x49,0x49,0x36}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x2F,0x49,0x49,0x49,0x31}, // 5
    {0x3E,0x49,0x49,0x49,0x32}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x26,0x49,0x49,0x49,0x3E}  // 9
  };
  if (c >= 'A' && c <= 'Z') {
    memcpy(out, font[c - 'A'], 5);
    return true;
  }
  if (c >= '0' && c <= '9') {
    memcpy(out, font[26 + c - '0'], 5);
    return true;
  }
  static const uint8_t dot[5] = {0x00,0x60,0x60,0x00,0x00};
  static const uint8_t percent[5] = {0x63,0x13,0x08,0x64,0x63};
  static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
  if (c == '.') memcpy(out, dot, 5);
  else if (c == '%') memcpy(out, percent, 5);
  else if (c == '-') memcpy(out, dash, 5);
  else if (c == ' ') memset(out, 0, 5);
  else return false;
  return true;
}

void drawText(const char *text, int x, int y, int scale = 1) {
  uint8_t columns[5];
  while (*text) {
    if (glyph(*text, columns)) {
      for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
          if (columns[col] & (1U << row)) {
            for (int dx = 0; dx < scale; ++dx) {
              for (int dy = 0; dy < scale; ++dy) {
                setPixel(x + col * scale + dx, y + row * scale + dy);
              }
            }
          }
        }
      }
    }
    x += 6 * scale;
    ++text;
  }
}

uint16_t flushDisplay() {
  uint16_t errors = 0;
  for (uint8_t page = 0; page < 8; ++page) {
    // The tested 128x64 SH1106-style panel starts at RAM column 2.
    const uint8_t column = 2;
    const uint8_t pageAndColumn[] = {
      static_cast<uint8_t>(0xB0 | page),
      static_cast<uint8_t>(column & 0x0F),
      static_cast<uint8_t>(0x10 | ((column >> 4) & 0x0F))
    };
    if (sendDisplayCommands(pageAndColumn, sizeof(pageAndColumn)) != 0) ++errors;
    const size_t pageOffset = static_cast<size_t>(page) * WIDTH;
    for (size_t offset = 0; offset < WIDTH; offset += 16) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write(0x40);
      Wire.write(frameBuffer + pageOffset + offset, 16);
      if (Wire.endTransmission() != 0) ++errors;
    }
  }
  return errors;
}

uint16_t renderScreen() {
  if (!oledAvailable) return 0;
  memset(frameBuffer, 0, sizeof(frameBuffer));

  drawText(sensorIsSCD41 ? "SCD41 LIVE" : "SCD4X LIVE", 4, 2);
  drawHLine(2, 12, 124);

  if (!sensorAvailable) {
    drawText("SENSOR NOT FOUND", 4, 23);
    drawText("CHECK GPIO3 4", 4, 38);
  } else if (!haveReading) {
    drawText("WAITING FOR", 4, 22);
    drawText("FIRST SAMPLE", 4, 38);
  } else {
    char value[8];
    snprintf(value, sizeof(value), "%u", co2ppm);
    const int digits = strlen(value);
    const int scale = digits <= 4 ? 2 : 1;
    drawText("CO2", 4, 19);
    drawText(value, 27, 15, scale);
    drawText("PPM", 87, 21);
    drawHLine(2, 39, 124);

    char temperature[12];
    snprintf(temperature, sizeof(temperature), "T %.1f C", temperatureC);
    char humidity[12];
    snprintf(humidity, sizeof(humidity), "RH %.0f%%", humidityPct);
    drawText(temperature, 4, 49);
    drawText(humidity, 78, 49);
  }
  return flushDisplay();
}

uint8_t sensirionCrc8(const uint8_t *data, size_t count) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < count; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

uint8_t sendSensorCommand(uint16_t command) {
  SensorBus.beginTransmission(SCD41_ADDR);
  SensorBus.write(static_cast<uint8_t>(command >> 8));
  SensorBus.write(static_cast<uint8_t>(command & 0xFF));
  return SensorBus.endTransmission();
}

bool readSensorWord(uint16_t &word) {
  const uint8_t received = SensorBus.requestFrom(static_cast<uint8_t>(SCD41_ADDR),
                                                 static_cast<uint8_t>(3));
  if (received != 3) {
    while (SensorBus.available()) SensorBus.read();
    return false;
  }
  uint8_t bytes[3];
  for (uint8_t i = 0; i < 3; ++i) bytes[i] = SensorBus.read();
  if (sensirionCrc8(bytes, 2) != bytes[2]) return false;
  word = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
  return true;
}

bool readDataReady(bool &ready) {
  if (sendSensorCommand(CMD_GET_READY) != 0) return false;
  delay(2);
  uint16_t status = 0;
  if (!readSensorWord(status)) return false;
  ready = (status & 0x07FF) != 0;
  return true;
}

bool readMeasurement(uint16_t &co2, float &tempC, float &rh) {
  if (sendSensorCommand(CMD_READ_MEASUREMENT) != 0) return false;
  delay(2);
  const uint8_t received = SensorBus.requestFrom(static_cast<uint8_t>(SCD41_ADDR),
                                                 static_cast<uint8_t>(9));
  if (received != 9) {
    while (SensorBus.available()) SensorBus.read();
    return false;
  }
  uint8_t bytes[9];
  for (uint8_t i = 0; i < 9; ++i) bytes[i] = SensorBus.read();
  for (uint8_t offset = 0; offset < 9; offset += 3) {
    if (sensirionCrc8(bytes + offset, 2) != bytes[offset + 2]) return false;
  }

  const uint16_t rawCo2 = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
  const uint16_t rawTemp = (static_cast<uint16_t>(bytes[3]) << 8) | bytes[4];
  const uint16_t rawRh = (static_cast<uint16_t>(bytes[6]) << 8) | bytes[7];
  co2 = rawCo2;
  tempC = -45.0f + (175.0f * rawTemp / 65535.0f);
  rh = 100.0f * rawRh / 65535.0f;
  return true;
}

void showError(const char *message) {
  Serial.println(message);
  if (oledAvailable) {
    memset(frameBuffer, 0, sizeof(frameBuffer));
    drawText("SCD41 TEST", 4, 3);
    drawHLine(2, 13, 124);
    drawText(message, 4, 25);
    const uint16_t errors = flushDisplay();
    if (errors) Serial.printf("OLED transfer errors: %u\n", errors);
  }
}

void setupDisplay() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(100000);
  oledAvailable = probe(Wire, OLED_ADDR);
  Serial.printf("TEST OLED 0x%02X on GPIO%d/%d: %s\n", OLED_ADDR,
                OLED_SDA_PIN, OLED_SCL_PIN, oledAvailable ? "PASS" : "FAIL");
  if (!oledAvailable) return;

  const uint8_t init[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
    0xAD, 0x8B, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F,
    0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
  };
  const uint8_t error = sendDisplayCommands(init, sizeof(init));
  if (error != 0) {
    oledAvailable = false;
    Serial.printf("TEST OLED initialization: FAIL (I2C error %u)\n", error);
    return;
  }
  Serial.println("TEST OLED initialization: PASS");
  renderScreen();
}

void setupSensor() {
  SensorBus.begin(SENSOR_SDA_PIN, SENSOR_SCL_PIN);
  SensorBus.setClock(100000);
  delay(1100);  // SCD4x needs about 1 s after power-up before commands.

  sensorAvailable = probe(SensorBus, SCD41_ADDR);
  Serial.printf("TEST SCD4x 0x%02X on GPIO%d/%d: %s\n", SCD41_ADDR,
                SENSOR_SDA_PIN, SENSOR_SCL_PIN,
                sensorAvailable ? "PASS" : "FAIL");
  if (!sensorAvailable) {
    showError("SENSOR NOT FOUND");
    return;
  }

  // Stop any prior periodic run so the idle-only variant query is available.
  const uint8_t stopError = sendSensorCommand(CMD_STOP_PERIODIC);
  delay(500);
  if (stopError != 0) {
    Serial.printf("TEST sensor stop/idle: WARN (I2C error %u)\n", stopError);
  } else {
    Serial.println("TEST sensor stop/idle: PASS");
  }

  uint16_t variant = 0;
  const uint8_t variantCommandError = sendSensorCommand(CMD_GET_VARIANT);
  delay(2);
  const bool variantReadOk = variantCommandError == 0 && readSensorWord(variant);
  sensorIsSCD41 = variantReadOk && ((variant & 0xF000) == 0x1000);
  if (variantReadOk) {
    Serial.printf("TEST sensor variant: %s (ID 0x%04X)\n",
                  sensorIsSCD41 ? "PASS - SCD41" : "WARN - not SCD41",
                  variant);
  } else {
    Serial.println("TEST sensor variant/CRC: FAIL");
  }

  const uint8_t startError = sendSensorCommand(CMD_START_PERIODIC);
  if (startError != 0) {
    measurementRunning = false;
    Serial.printf("TEST periodic measurement start: FAIL (I2C error %u)\n",
                  startError);
    showError("START MEASURE ERR");
    return;
  }
  measurementRunning = true;
  Serial.println("TEST periodic measurement start: PASS; updates about every 5 s");
  renderScreen();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== XIAO ESP32-S3 / SCD41 display diagnostics ===");
  Serial.println("Sensor wiring assumption: GPIO3=SDA, GPIO4=SCL (typed as SDC).");
  setupDisplay();
  setupSensor();
}

void loop() {
  if (!measurementRunning) {
    delay(100);
    return;
  }

  const uint32_t now = millis();
  if (now - lastReadyPoll < 250) {
    delay(10);
    return;
  }
  lastReadyPoll = now;

  bool ready = false;
  if (!readDataReady(ready)) {
    Serial.println("TEST data-ready/status CRC: FAIL");
    return;
  }
  if (!ready) return;

  uint16_t newCo2 = 0;
  float newTemp = 0.0f;
  float newRh = 0.0f;
  if (!readMeasurement(newCo2, newTemp, newRh)) {
    Serial.println("TEST measurement read/CRC: FAIL");
    return;
  }

  co2ppm = newCo2;
  temperatureC = newTemp;
  humidityPct = newRh;
  haveReading = true;
  Serial.printf("TEST measurement read/CRC: PASS | CO2=%u ppm | T=%.1f C | RH=%.1f %%\n",
                co2ppm, temperatureC, humidityPct);
  const uint16_t displayErrors = renderScreen();
  if (displayErrors == 0) {
    Serial.println("TEST OLED update: PASS");
  } else {
    Serial.printf("TEST OLED update: FAIL (%u I2C transfer errors)\n",
                  displayErrors);
  }
}
