// ============================================================
//  IGNIS v2 — Flight Computer Firmware
//  Ignitia Rocket Lab | Tecnológico de Monterrey GDL
//  LASC 2026 — Rocket Challenge
//
//  Author:  Emilio Guadarrama Gutiérrez
//  Board:   ESP32-S3-WROOM-1-N16R8
//  IDE:     Arduino IDE 2.x
//  Target:  1,000 m apogee — dual-event pyrotechnic recovery
// ============================================================

// ============================================================
//  REQUIRED LIBRARIES
//  Install via Arduino Library Manager:
//    - RH_RF95 (RadioHead)         → LoRa RFM95W
//    - Adafruit BMP085 Unified     → BMP180 barometer
//    - Adafruit MPU6050            → IMU
//    - Adafruit SHT4x              → Temp/Humidity
//    - TinyGPSPlus                 → GPS parsing
//    - SD (built-in)               → MicroSD
//    - Adafruit NeoPixel           → WS2812B LEDs
//    - Wire, SPI (built-in)
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

// LoRa
#include <RH_RF95.h>

// Sensors
#include <Adafruit_BMP085.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_SHT4x.h>

// GPS
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>

// Storage
#include <SD.h>

// LEDs
#include <Adafruit_NeoPixel.h>

// Sleep
#include <esp_sleep.h>

// ============================================================
//  PIN DEFINITIONS — match MCU.SchDoc net labels exactly
// ============================================================

// Pyro channels
#define PIN_PYRO_CH1_CONT   1   // IO1 — Channel 1 continuity sense
#define PIN_PYRO_CH1_GATE   4   // IO4 — Channel 1 fire gate
#define PIN_PYRO_CH2_CONT   2   // IO2 — Channel 2 continuity sense
#define PIN_PYRO_CH2_GATE   5   // IO5 — Channel 2 fire gate

// Battery monitoring
#define PIN_VBAT_MON        3   // IO3 — ADC (100k+47k divider)

// LoRa RFM95W (SPI)
#define PIN_RFM95W_CS       10  // IO10
#define PIN_RFM95W_INT      7   // IO7  — DIO0
#define PIN_RFM95W_RESET    8   // IO8

// SPI bus (shared: LoRa + MicroSD)
#define PIN_SPI_MOSI        11  // IO11
#define PIN_SPI_MISO        13  // IO13
#define PIN_SPI_SCK         12  // IO12

// MicroSD
#define PIN_SD_CS           14  // IO14
#define PIN_SD_DETECT       15  // IO15

// I2C bus (BMP180 + MPU6050 + SHT40)
#define PIN_I2C_SDA         8   // IO8  — shared with RFM95W_RESET via net SNS_I2C_SDA
#define PIN_I2C_SCL         9   // IO9

// GPS UART
#define PIN_GPS_TX          17  // IO17 — FC TX → GPS RX
#define PIN_GPS_RX          18  // IO18 — FC RX ← GPS TX

// Indicators
#define PIN_WS2812B         6   // IO6
#define PIN_BUZZER          16  // IO16

// Buttons (input only — handled by boot sequence)
#define PIN_BOOT            0   // IO0 — wake from sleep
#define PIN_MPU6050_INT     4   // IO4 — reused carefully

// USB
#define PIN_USB_DP          20  // IO20
#define PIN_USB_DN          19  // IO19

// ============================================================
//  CONFIGURATION
// ============================================================

// LoRa
#define LORA_FREQ           915.0
#define LORA_SF             9
#define LORA_BW             125000
#define LORA_CR             5       // 4/5
#define LORA_SYNC           0x12
#define LORA_TX_POWER       17      // PA_BOOST pin

// Flight parameters
#define LAUNCH_DETECT_ACCEL     2.0f    // g — min accel to detect launch
#define APOGEE_CONFIRM_COUNT    10      // consecutive descending readings
#define MAIN_DEPLOY_ALT         450.0f  // m AGL — main chute altitude
#define PYRO_FIRE_DURATION      1000    // ms — how long to hold gate HIGH
#define CONTINUITY_THRESHOLD    512     // ADC value to confirm e-match continuity

// Telemetry
#define TELEM_INTERVAL_MS       500     // ms between LoRa packets in flight
#define TELEM_INTERVAL_IDLE_MS  2000    // ms between packets on ground

// SD logging
#define LOG_INTERVAL_MS         100     // ms between SD writes in flight

// Battery
#define VBAT_DIVIDER_RATIO      (147.0f / 47.0f)  // (R5+R6)/R5 = 147k/47k
#define VBAT_ADC_REF            3.3f
#define VBAT_ADC_MAX            4095.0f
#define VBAT_LOW_THRESHOLD      6.8f    // V — warn below this

// LEDs
#define NUM_LEDS                3
#define LED_BRIGHTNESS          50

// ============================================================
//  FLIGHT STATE MACHINE
// ============================================================

enum FlightState {
  STATE_STANDBY,    // On rail — light sleep, waiting for ARM button
  STATE_ARMED,      // Sensors active, logging ready, awaiting launch
  STATE_FLIGHT,     // Powered ascent / coasting — detecting apogee
  STATE_APOGEE,     // Apogee detected — firing drogue (CH1)
  STATE_DESCENT,    // Descending — monitoring for main deploy altitude
  STATE_MAIN,       // Main deployed (CH2 fired)
  STATE_LANDED      // Below threshold velocity — beeping for recovery
};

FlightState flightState = STATE_STANDBY;

// ============================================================
//  GLOBAL OBJECTS
// ============================================================

RH_RF95          lora(PIN_RFM95W_CS, PIN_RFM95W_INT);
Adafruit_BMP085  bmp;
Adafruit_MPU6050 mpu;
Adafruit_SHT4x   sht;
TinyGPSPlus      gps;
HardwareSerial   gpsSerial(1);
Adafruit_NeoPixel leds(NUM_LEDS, PIN_WS2812B, NEO_GRB + NEO_KHZ800);

File logFile;
String logFileName = "/flight_000.csv";

// ============================================================
//  FLIGHT DATA STRUCT
// ============================================================

struct FlightData {
  // Time
  unsigned long timestamp_ms;

  // Barometer
  float altitude_m;
  float pressure_hpa;
  float temp_bmp_c;

  // IMU
  float accel_x, accel_y, accel_z;   // g
  float gyro_x,  gyro_y,  gyro_z;    // deg/s
  float total_accel_g;

  // Temp/Humidity
  float temp_sht_c;
  float humidity_pct;

  // GPS
  float lat, lon;
  float gps_alt_m;
  float gps_speed_ms;
  uint8_t gps_sats;
  bool  gps_valid;

  // Power
  float vbat_v;

  // Recovery
  bool ch1_continuity;
  bool ch2_continuity;

  // State
  FlightState state;
};

FlightData current;

// ============================================================
//  APOGEE DETECTION
// ============================================================

float groundAltitude_m    = 0.0f;
float peakAltitude_m      = 0.0f;
int   apogeeConfirmCount  = 0;
float lastAltitude_m      = 0.0f;
bool  launchDetected      = false;

// ============================================================
//  TIMING
// ============================================================

unsigned long lastTelemMs   = 0;
unsigned long lastLogMs     = 0;
unsigned long lastSensorMs  = 0;
unsigned long pyro1FireTime = 0;
unsigned long pyro2FireTime = 0;
bool pyro1Active = false;
bool pyro2Active = false;

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[IGNIS v2] Boot");

  // Pin modes
  pinMode(PIN_PYRO_CH1_GATE, OUTPUT);
  pinMode(PIN_PYRO_CH2_GATE, OUTPUT);
  pinMode(PIN_PYRO_CH1_CONT, INPUT);
  pinMode(PIN_PYRO_CH2_CONT, INPUT);
  digitalWrite(PIN_PYRO_CH1_GATE, LOW);
  digitalWrite(PIN_PYRO_CH2_GATE, LOW);

  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_SD_DETECT, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  // LEDs
  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  setLED(0, 50, 50, 0);   // Yellow = booting

  // I2C
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  // SPI
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);

  // BMP180
  if (!bmp.begin()) {
    Serial.println("[ERROR] BMP180 not found");
    errorHalt();
  }
  Serial.println("[OK] BMP180");

  // MPU6050
  if (!mpu.begin()) {
    Serial.println("[ERROR] MPU6050 not found");
    errorHalt();
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("[OK] MPU6050");

  // SHT40
  if (!sht.begin()) {
    Serial.println("[ERROR] SHT40 not found");
    // Non-fatal — continue without it
  } else {
    sht.setPrecision(SHT4X_HIGH_PRECISION);
    Serial.println("[OK] SHT40");
  }

  // GPS — low power until needed
  gpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  Serial.println("[OK] GPS UART");

  // RFM95W
  initLoRa();

  // MicroSD
  initSD();

  // Calibrate ground altitude
  calibrateGroundAltitude();

  // Battery check
  float vbat = readVbat();
  Serial.printf("[VBAT] %.2f V\n", vbat);
  if (vbat < VBAT_LOW_THRESHOLD) {
    Serial.println("[WARN] Battery low!");
    buzz(100); delay(100); buzz(100);
  }

  // Boot complete
  setLED(0, 0, 0, 50);  // Blue = standby
  buzz(200);
  Serial.println("[IGNIS v2] Ready — entering STANDBY");
  flightState = STATE_STANDBY;
}

// ============================================================
//  MAIN LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  // Read sensors every 50ms
  if (now - lastSensorMs >= 50) {
    readAllSensors();
    lastSensorMs = now;
  }

  // State machine
  switch (flightState) {

    case STATE_STANDBY:
      // Light sleep — wake on BOOT button press to arm
      handleStandby();
      break;

    case STATE_ARMED:
      handleArmed(now);
      break;

    case STATE_FLIGHT:
      handleFlight(now);
      break;

    case STATE_APOGEE:
      handleApogee(now);
      break;

    case STATE_DESCENT:
      handleDescent(now);
      break;

    case STATE_MAIN:
      handleMain(now);
      break;

    case STATE_LANDED:
      handleLanded(now);
      break;
  }

  // Manage active pyro channels
  managePyroChannels(now);

  // Log to SD
  if (flightState >= STATE_ARMED && now - lastLogMs >= LOG_INTERVAL_MS) {
    logToSD();
    lastLogMs = now;
  }

  // Telemetry
  unsigned long telemInterval = (flightState >= STATE_FLIGHT)
                                ? TELEM_INTERVAL_MS
                                : TELEM_INTERVAL_IDLE_MS;
  if (now - lastTelemMs >= telemInterval) {
    sendTelemetry();
    lastTelemMs = now;
  }
}

// ============================================================
//  STATE HANDLERS
// ============================================================

void handleStandby() {
  // ⚠️ FIRMWARE REMINDER: Rail standby — 4 hour requirement
  // ESP32-S3 must be in light sleep here
  // GPS must stay in backup mode — not actively acquiring
  // Wake only on BOOT button press

  Serial.println("[STANDBY] Sleeping... press BOOT to arm");

  // Configure wake-up source
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT, 0);  // Wake on LOW

  // GPS to sleep
  gpsSerial.println("$PMTK161,0*28");  // GPS standby command

  // LED off to save power
  setLED(0, 0, 0, 0);

  // Enter light sleep
  esp_light_sleep_start();

  // Woke up — check if BOOT button caused wake
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[STANDBY] BOOT pressed — ARMING");
    transitionTo(STATE_ARMED);
  }
}

void handleArmed(unsigned long now) {
  setLED(0, 50, 0, 0);   // Red = armed

  // Check continuity
  current.ch1_continuity = (analogRead(PIN_PYRO_CH1_CONT) > CONTINUITY_THRESHOLD);
  current.ch2_continuity = (analogRead(PIN_PYRO_CH2_CONT) > CONTINUITY_THRESHOLD);

  if (!current.ch1_continuity) Serial.println("[WARN] CH1 continuity FAIL");
  if (!current.ch2_continuity) Serial.println("[WARN] CH2 continuity FAIL");

  // Detect launch: sustained high acceleration
  if (current.total_accel_g >= LAUNCH_DETECT_ACCEL) {
    if (!launchDetected) {
      launchDetected = true;
      Serial.printf("[LAUNCH] Detected! Accel=%.2fg\n", current.total_accel_g);
      transitionTo(STATE_FLIGHT);
    }
  }
}

void handleFlight(unsigned long now) {
  setLED(0, 0, 50, 0);  // Green = flight

  float alt = current.altitude_m - groundAltitude_m;

  // Track peak altitude
  if (alt > peakAltitude_m) {
    peakAltitude_m = alt;
    apogeeConfirmCount = 0;
  }

  // Apogee detection: altitude consistently decreasing
  if (alt < lastAltitude_m) {
    apogeeConfirmCount++;
  } else {
    apogeeConfirmCount = 0;
  }
  lastAltitude_m = alt;

  if (apogeeConfirmCount >= APOGEE_CONFIRM_COUNT) {
    Serial.printf("[APOGEE] Detected at %.1f m AGL\n", peakAltitude_m);
    transitionTo(STATE_APOGEE);
  }
}

void handleApogee(unsigned long now) {
  setLED(50, 0, 50, 0);  // Magenta = apogee

  // Fire drogue — Channel 1
  if (!pyro1Active) {
    Serial.println("[PYRO] Firing CH1 — DROGUE");
    firePyro(1);
  }

  // Once pyro fires, transition to descent
  if (!pyro1Active && now - pyro1FireTime > PYRO_FIRE_DURATION + 100) {
    transitionTo(STATE_DESCENT);
  }
}

void handleDescent(unsigned long now) {
  setLED(0, 50, 50, 0);  // Cyan = descent

  float agl = current.altitude_m - groundAltitude_m;

  Serial.printf("[DESCENT] AGL=%.1f m\n", agl);

  // Main deployment at 450m AGL
  if (agl <= MAIN_DEPLOY_ALT && !pyro2Active) {
    Serial.printf("[PYRO] Firing CH2 — MAIN at %.1f m AGL\n", agl);
    firePyro(2);
    transitionTo(STATE_MAIN);
  }
}

void handleMain(unsigned long now) {
  setLED(0, 0, 50, 50);  // White-ish = main deployed

  // Wait for landing: low altitude + near-zero vertical speed
  float agl = current.altitude_m - groundAltitude_m;
  if (agl < 20.0f && current.total_accel_g < 1.2f) {
    Serial.println("[LANDED] Touchdown detected");
    transitionTo(STATE_LANDED);
  }
}

void handleLanded(unsigned long now) {
  // Beep for recovery every 2 seconds
  static unsigned long lastBeep = 0;
  if (now - lastBeep >= 2000) {
    buzz(100);
    setLED(50, 50, 50, 0);
    delay(50);
    setLED(0, 0, 0, 0);
    lastBeep = now;
  }

  // Close SD log file
  if (logFile) {
    logFile.close();
    Serial.println("[SD] Log file closed");
  }
}

// ============================================================
//  PYRO CONTROL
// ============================================================

void firePyro(int channel) {
  if (channel == 1) {
    digitalWrite(PIN_PYRO_CH1_GATE, HIGH);
    pyro1FireTime = millis();
    pyro1Active = true;
    Serial.println("[PYRO] CH1 gate OPEN");
    buzz(500);
  } else if (channel == 2) {
    digitalWrite(PIN_PYRO_CH2_GATE, HIGH);
    pyro2FireTime = millis();
    pyro2Active = true;
    Serial.println("[PYRO] CH2 gate OPEN");
    buzz(500);
  }
}

void managePyroChannels(unsigned long now) {
  if (pyro1Active && now - pyro1FireTime >= PYRO_FIRE_DURATION) {
    digitalWrite(PIN_PYRO_CH1_GATE, LOW);
    pyro1Active = false;
    Serial.println("[PYRO] CH1 gate CLOSED");
  }
  if (pyro2Active && now - pyro2FireTime >= PYRO_FIRE_DURATION) {
    digitalWrite(PIN_PYRO_CH2_GATE, LOW);
    pyro2Active = false;
    Serial.println("[PYRO] CH2 gate CLOSED");
  }
}

// ============================================================
//  SENSOR READING
// ============================================================

void readAllSensors() {
  current.timestamp_ms = millis();

  // BMP180
  current.pressure_hpa = bmp.readPressure() / 100.0f;
  current.temp_bmp_c   = bmp.readTemperature();
  current.altitude_m   = bmp.readAltitude(1013.25f);  // Update with QNH if available

  // MPU6050
  sensors_event_t accel_evt, gyro_evt, temp_evt;
  mpu.getEvent(&accel_evt, &gyro_evt, &temp_evt);
  current.accel_x = accel_evt.acceleration.x / 9.81f;
  current.accel_y = accel_evt.acceleration.y / 9.81f;
  current.accel_z = accel_evt.acceleration.z / 9.81f;
  current.gyro_x  = gyro_evt.gyro.x * RAD_TO_DEG;
  current.gyro_y  = gyro_evt.gyro.y * RAD_TO_DEG;
  current.gyro_z  = gyro_evt.gyro.z * RAD_TO_DEG;
  current.total_accel_g = sqrt(
    current.accel_x * current.accel_x +
    current.accel_y * current.accel_y +
    current.accel_z * current.accel_z
  );

  // SHT40
  sensors_event_t hum_evt, temp_sht_evt;
  sht.getEvent(&hum_evt, &temp_sht_evt);
  current.temp_sht_c  = temp_sht_evt.temperature;
  current.humidity_pct = hum_evt.relative_humidity;

  // GPS
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
  if (gps.location.isValid()) {
    current.lat       = gps.location.lat();
    current.lon       = gps.location.lng();
    current.gps_alt_m = gps.altitude.meters();
    current.gps_speed_ms = gps.speed.mps();
    current.gps_sats  = gps.satellites.value();
    current.gps_valid = true;
  } else {
    current.gps_valid = false;
  }

  // Battery
  current.vbat_v = readVbat();
}

float readVbat() {
  int raw = analogRead(PIN_VBAT_MON);
  float vPin = (raw / VBAT_ADC_MAX) * VBAT_ADC_REF;
  return vPin * VBAT_DIVIDER_RATIO;
}

void calibrateGroundAltitude() {
  Serial.println("[CAL] Calibrating ground altitude...");
  float sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += bmp.readAltitude(1013.25f);
    delay(50);
  }
  groundAltitude_m = sum / 20.0f;
  Serial.printf("[CAL] Ground = %.1f m\n", groundAltitude_m);
}

// ============================================================
//  LORA TELEMETRY
// ============================================================

void initLoRa() {
  pinMode(PIN_RFM95W_RESET, OUTPUT);
  digitalWrite(PIN_RFM95W_RESET, LOW);
  delay(10);
  digitalWrite(PIN_RFM95W_RESET, HIGH);
  delay(10);

  if (!lora.init()) {
    Serial.println("[ERROR] RFM95W init failed");
    errorHalt();
  }

  lora.setFrequency(LORA_FREQ);
  lora.setTxPower(LORA_TX_POWER, false);  // false = PA_BOOST pin
  lora.setSpreadingFactor(LORA_SF);
  lora.setSignalBandwidth(LORA_BW);
  lora.setCodingRate4(LORA_CR);
  lora.setSyncWord(LORA_SYNC);

  Serial.printf("[OK] RFM95W @ %.1f MHz SF%d\n", LORA_FREQ, LORA_SF);
}

void sendTelemetry() {
  // Build compact CSV packet
  char packet[120];
  snprintf(packet, sizeof(packet),
    "%lu,%d,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.6f,%.6f,%d,%.2f,%d,%d",
    current.timestamp_ms,
    (int)flightState,
    current.altitude_m - groundAltitude_m,  // AGL
    peakAltitude_m,
    current.total_accel_g,
    current.pressure_hpa,
    current.temp_bmp_c,
    current.vbat_v,
    current.lat,
    current.lon,
    current.gps_sats,
    current.gps_alt_m,
    current.ch1_continuity ? 1 : 0,
    current.ch2_continuity ? 1 : 0
  );

  lora.send((uint8_t*)packet, strlen(packet));
  lora.waitPacketSent();

  // Read RSSI accurately — SX1276 HF port offset = -157
  // Do NOT use lora.lastRssi() — it uses wrong -137 offset
  // Requires direct SPI register read if using RadioHead
  // For RadioLib: use spiRead(0x1A) - 157
}

// ============================================================
//  SD CARD LOGGING
// ============================================================

void initSD() {
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[WARN] SD card not found — logging disabled");
    return;
  }

  // Find available filename
  for (int i = 0; i < 999; i++) {
    logFileName = "/flight_" + String(i, DEC) + ".csv";
    if (!SD.exists(logFileName)) break;
  }

  logFile = SD.open(logFileName, FILE_WRITE);
  if (!logFile) {
    Serial.println("[WARN] Could not open log file");
    return;
  }

  // Write CSV header
  logFile.println(
    "timestamp_ms,state,alt_agl_m,peak_alt_m,"
    "accel_total_g,accel_x,accel_y,accel_z,"
    "gyro_x,gyro_y,gyro_z,"
    "pressure_hpa,temp_bmp_c,temp_sht_c,humidity_pct,"
    "vbat_v,lat,lon,gps_alt_m,gps_sats,"
    "ch1_cont,ch2_cont"
  );
  logFile.flush();

  Serial.printf("[SD] Logging to %s\n", logFileName.c_str());
}

void logToSD() {
  if (!logFile) return;

  logFile.printf(
    "%lu,%d,%.2f,%.2f,"
    "%.3f,%.3f,%.3f,%.3f,"
    "%.2f,%.2f,%.2f,"
    "%.2f,%.2f,%.2f,%.1f,"
    "%.3f,%.6f,%.6f,%.1f,%d,"
    "%d,%d\n",
    current.timestamp_ms,
    (int)flightState,
    current.altitude_m - groundAltitude_m,
    peakAltitude_m,
    current.total_accel_g,
    current.accel_x, current.accel_y, current.accel_z,
    current.gyro_x,  current.gyro_y,  current.gyro_z,
    current.pressure_hpa,
    current.temp_bmp_c,
    current.temp_sht_c,
    current.humidity_pct,
    current.vbat_v,
    current.lat, current.lon,
    current.gps_alt_m,
    current.gps_sats,
    current.ch1_continuity ? 1 : 0,
    current.ch2_continuity ? 1 : 0
  );

  // Flush every 10 writes to reduce SD wear
  static int writeCount = 0;
  if (++writeCount >= 10) {
    logFile.flush();
    writeCount = 0;
  }
}

// ============================================================
//  LED CONTROL
// ============================================================

void setLED(int r, int g, int b, int white) {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds.setPixelColor(i, leds.Color(r, g, b));
  }
  leds.show();
}

// ============================================================
//  BUZZER
// ============================================================

void buzz(int duration_ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(duration_ms);
  digitalWrite(PIN_BUZZER, LOW);
}

// ============================================================
//  STATE TRANSITION
// ============================================================

void transitionTo(FlightState newState) {
  Serial.printf("[STATE] %d → %d\n", (int)flightState, (int)newState);
  flightState = newState;
  buzz(100);
}

// ============================================================
//  ERROR HALT
// ============================================================

void errorHalt() {
  Serial.println("[FATAL] System halted");
  while (true) {
    setLED(50, 0, 0, 0);
    buzz(200);
    delay(200);
    setLED(0, 0, 0, 0);
    delay(200);
  }
}
