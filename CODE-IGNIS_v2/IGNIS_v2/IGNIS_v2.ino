#include <Wire.h>

#define PIN_SDA 21
#define PIN_SCL 38

void setup() {
  Serial.begin(115200);
  Serial.println("I2C Scanner — IGNIS v2");
  delay(500);

  // Bus pre-conditioning — recovers stuck I2C lines
  pinMode(PIN_SDA, OUTPUT); digitalWrite(PIN_SDA, HIGH);
  pinMode(PIN_SCL, OUTPUT); digitalWrite(PIN_SCL, HIGH);
  delay(100);
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(100);

  Wire.begin(PIN_SDA, PIN_SCL);
  delay(500);

  Serial.println("I2C Scanner — IGNIS v2");
  Serial.println("Scanning...");

  byte found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("Device at 0x");
      Serial.print(addr, HEX);
      if (addr == 0x77) Serial.print("  <- BMP180");
      if (addr == 0x68) Serial.print("  <- MPU6050");
      if (addr == 0x44) Serial.print("  <- SHT40");
      Serial.println();
      found++;
    }
  }

  if (found == 0) Serial.println("Nothing found.");
  Serial.print("Total: ");
  Serial.print(found);
  Serial.println(" device(s)");
}

void loop() {
  Serial.println("I2C Scanner — IGNIS v2");
  delay(500);
}