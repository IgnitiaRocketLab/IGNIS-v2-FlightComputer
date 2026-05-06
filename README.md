# IGNIS v2 — Flight Computer
### Ignitia Rocket Lab · Tecnológico de Monterrey GDL · LASC 2026

---

> *Designed to survive a rocket. Built by students who refused to buy one off the shelf.*

---

## What is this?

IGNIS v2 is the second generation SRAD (Student Researched and Developed) flight computer
for Ignitia Rocket Lab's entry in the **Latin American Space Challenge 2026**, held in Brazil.

It handles everything from liftoff to recovery — sensor fusion, apogee detection,
pyrotechnic deployment, real-time LoRa telemetry, and full flight data logging.
No commercial flight computer. No shortcuts.

---

## Hardware at a Glance

| Block | Component |
|---|---|
| MCU | ESP32-S3-WROOM-1-N8R8 |
| RF Telemetry | RFM95W — 915 MHz LoRa |
| Barometer | BMP180 |
| IMU | MPU-6050 (6-DOF) |
| Temp / Humidity | SHT40 |
| GPS | Adafruit GPS Breakout |
| Storage | MicroSD — SPI |
| Power | Dual AMS1117 series · VBAT → 5V → 3.3V |
| Pyro | 2-channel MOSFET deployment |
| PCB | 4-layer · Altium Designer · JLCPCB fab |
| Target apogee | 1,000 m — LASC Rocket Challenge |

---

## Project Structure

```
IGNIS-v2-FlightComputer/
├── Schematics/
│   ├── Main.SchDoc
│   ├── Power.SchDoc
│   ├── MCU.SchDoc
│   ├── RF_LoRa.SchDoc
│   ├── Sensors.SchDoc
│   ├── GPS.SchDoc
│   ├── Storage.SchDoc
│   ├── Indicators.SchDoc
│   └── Pyro.SchDoc
├── PCB/
│   └── IGNIS_v2.PcbDoc
├── Libraries/
└── Fabrication/
```

---

## Design & Development

**Lead Designer & Author** — [Emilio Guadarrama](https://github.com/Emilio-Guadarrama)  
Vice President, Ignitia Rocket Lab · Avionics & Electronics Lead

**Pyro Circuit** — Aristoteles Prieto  
**Team President** — Maximiliano Funoy Serrano  
**Faculty Advisor** — Dr. José Luis Henríquez Mercado

## Team

**Ignitia Rocket Lab**
Tecnológico de Monterrey — Campus Guadalajara

📧 ignitia.rocketlab@gmail.com

---

*IGNIS v2 is a fully student-designed system.
Every trace routed, every component chosen, every test run — by the team.*

