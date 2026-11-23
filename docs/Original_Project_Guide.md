# BLE Car Key Finder & Range Alarm – Project Guide

Nov 17, 2026 · @Pruthvi M

## Overview

This project is a two-board Bluetooth Low Energy (BLE) key finder and range alarm, calibrated to a 1.5 m limit. ESP32 #1 (the key unit) sits on the car key; ESP32 #2 (the pocket unit) travels with you. If you move farther than 1.5 m from the key, the key unit beeps and flashes its LEDs. Tapping the BOOT button on the pocket unit makes the key unit play a unique melody so you can find it. The pocket unit shows status on a 16x2 I2C LCD.

## Components used

The project needs two ESP32 boards and a handful of small parts; the BOOT button and the blue on-board LED on each board are used as they are.

| Qty | Part | Used on | Purpose |
| --- | --- | --- | --- |
| 2 | ESP32 development board | Both units | Bluetooth Low Energy link and control logic |
| 1 | Buzzer (passive recommended) | Key unit | Find-my-key melody, far warning and link-lost beeps |
| 1 | External LED | Key unit | Backup indicator beside the on-board LED |
| 1 | 220 ohm resistor | Key unit | Limits current through the external LED |
| 1 | 16x2 LCD with I2C backpack | Pocket unit | Shows key status, signal level and limit |
| 2 | Battery or USB power source | Both units | Power for each board |
| 1 | Set of jumper wires | Both units | Connections listed in the connection guide |
| 2 | BOOT button (on the board) | Both units | Pocket unit: ring the key and set the limit. Key unit: silence the sound |

## How it works

The pocket unit judges distance from Bluetooth signal strength (RSSI): the weaker the signal, the farther the key. The limit is stored as a signal level, so it is calibrated once at 1.5 m rather than typed in as meters.

1. **Connect.** The key unit advertises as "CarKey". The pocket unit scans, finds it and connects by itself. Both on-board LEDs blink slowly while searching and stay solid once connected.
2. **Measure.** The pocket unit reads the signal about every 0.4 s and averages the last 4 readings, so one noisy reading does not trigger an alarm.
3. **Far warning.** If the average is weaker than the stored limit, the LCD shows FAR and the pocket unit tells the key unit, which plays two warning beeps and blinks fast. The alarm clears only when the signal recovers by 3 dB, which stops it flickering at the edge.
4. **Link lost.** If the connection drops, the key unit plays three fast beeps (for up to 60 s) and the pocket unit shows LOST while it searches again.
5. **Find my key.** A short tap on the pocket unit's BOOT button plays a rising four-note chime on the key unit for up to 30 s. Tap again to stop it, or press BOOT on the key unit.
6. **Set the limit.** Stand 1.5 m from the key and hold BOOT on the pocket unit for about 1 s. The current signal level is saved in memory and survives power-off.

| Situation | Key unit sound | LEDs | Pocket LCD |
| --- | --- | --- | --- |
| Searching | Silent | Slow blink | Searching key... |
| Connected, within limit | Silent | Solid | Key:OK with signal level |
| Farther than limit | Two beeps, repeating | Fast blink | Key:FAR! |
| Find my key (tap BOOT) | Rising chime, repeating | Fast blink | Ringing key... |
| Link lost | Three fast beeps, repeating | Fast blink | Key:LOST |

## Circuit diagram

&#91;embedded content: circuit diagram · 2 boards, 1 wireless link\]

The two boards share no wires and talk only over Bluetooth. Each wire runs from a labelled ESP32 pin to the part beside it. Power each board from USB or a battery on VIN and GND; on the pocket unit the LCD uses those same two pins.

## Connection guide

Wire each unit exactly as in the tables below, with the power disconnected. Pin names are the labels printed on the ESP32 board (GPIO numbers, not pin positions).

**Key unit (ESP32 #1)**

| From | To (ESP32 pin) | Note |
| --- | --- | --- |
| Buzzer + (long leg or marked +) | GPIO25 | Passive buzzer plays the melodies |
| Buzzer - | GND |  |
| 220 ohm resistor, one end | GPIO4 |  |
| 220 ohm resistor, other end | External LED anode (long leg) |  |
| External LED cathode (short leg) | GND |  |
| Battery + | VIN (5 V) | Or power through USB |
| Battery - | GND |  |
| On-board LED | GPIO2 | Already on the board, no wire |
| BOOT button | GPIO0 | Already on the board, no wire |

**Pocket unit (ESP32 #2)**

| From | To (ESP32 pin) | Note |
| --- | --- | --- |
| LCD GND | GND |  |
| LCD VCC | VIN (5 V) | The LCD backpack needs 5 V, not 3.3 V |
| LCD SDA | GPIO21 | I2C data |
| LCD SCL | GPIO22 | I2C clock |
| Battery + | VIN (5 V) | Or power through USB |
| Battery - | GND |  |
| On-board LED | GPIO2 | Already on the board, no wire |
| BOOT button | GPIO0 | Already on the board, no wire |

Do not hold BOOT while powering on or resetting a board: that puts it into flash-programming mode. Press it only after the board has started.

## Operation and calibration

The limit is set to 1.5 m for the exhibition hall. Repeat these steps if the key unit or its mounting changes.

1. Power the key unit first, then the pocket unit. Wait for both LEDs to turn solid, which means they are connected.
2. Hold the key unit and the pocket unit exactly as they will be used on the day.
3. Measure 1.5 m between them with the tape.
4. Hold BOOT on the pocket unit for about 1 s. The LCD shows "Limit set" with the stored signal level.
5. Walk inside and outside the 1.5 m mark to confirm that FAR appears and the key unit beeps only outside it.
6. Tap BOOT on the pocket unit to test the find-my-key melody, and tap again to stop it.

Until the first calibration, the pocket unit uses a built-in default limit, so always calibrate before a demonstration.

**Tuning notes**

- Bluetooth signal strength is approximate: expect the alarm point to vary by roughly half a meter either way, more in a crowded hall.
- A body, a bag or a hand over the antenna weakens the signal. Calibrate with the units placed as they will be carried.
- If it alarms too early, set the limit again at a slightly larger distance; if too late, set it a little closer.
- Both units use the same transmit power setting in the code, so keep the same code on both boards after any change.
- A weak battery on the key unit lowers the signal and can cause early alarms; swap or recharge it first.
