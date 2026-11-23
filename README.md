# BLE Car Key Finder & Range Alarm

An autonomous, two-board ESP32 Bluetooth Low Energy (BLE) key locator and proximity alarm.

## Overview
- **Key Tag Unit (ESP32 #1):** BLE Server attached to car keys. Equipped with buzzer, high-brightness 5mm LED, onboard LED, and silence button.
- **Handheld Unit (ESP32 #2):** BLE Client carried by user. Monitors RSSI, drives an external trigger button (GPIO 4), an active buzzer (GPIO 25), and a 16x2 I2C LCD.

## Features
- **Find-My-Key:** Press external button on GPIO 4 to trigger a melodic chime and bright LED flash on the key tag.
- **Dual-Unit Separation Alarm:** When out of range (>1.5 m), both devices sound alarms.
- **Universal Silence:** Press BOOT (GPIO 0) on either board to mute active alarms.
- **Reliable Filtering:** Median-of-7 + Exponential Moving Average to prevent false alarms when close.
- **Flash Storage:** Hold BOOT on handheld for 3.5s to recalibrate range threshold into NVS memory.

## Hardware Connections

### Handheld Unit (ESP32 #2)
| Component | ESP32 Pin |
| :--- | :--- |
| External Push Button | GPIO 4 |
| On-Board BOOT Button | GPIO 0 |
| Handheld Buzzer (+) | GPIO 25 |
| 16x2 LCD SDA | GPIO 21 |
| 16x2 LCD SCL | GPIO 22 |
| 16x2 LCD VCC / GND | VIN (5V) / GND |
| On-Board LED | GPIO 2 |

### Key Tag Unit (ESP32 #1)
| Component | ESP32 Pin |
| :--- | :--- |
| Buzzer (+) | GPIO 25 |
| External LED Anode | GPIO 4 (via 220 ohm resistor) |
| External LED Cathode | GND |
| On-Board LED | GPIO 2 |
| On-Board BOOT Button | GPIO 0 |

## License
Released under the MIT License. Copyright (c) 2026 Pruthvi M.
