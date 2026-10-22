# RC-80 – 80s Boombox Radio Cassette Player (Arduino firmware)

Firmware for the **RC-80**, a 3D-printed 80s-style boombox with an AM/FM stereo radio, an analog dial with a real moving needle, an auto-reverse cassette deck and an internal rechargeable battery, all run by an Arduino Nano.

**3D model, print files, assembly manual and shopping list:**
👉 **[RC-80 on MakerWorld](https://makerworld.com/en/models/3479422-rc-80-80s-boombox-radio-cassette-player)**

This repository only contains the firmware source code.

🇧🇷 [Leia em português](README.pt.md)

---

## Sketches

| Folder | Description |
|---|---|
| [`firmware/radio_k7_en`](firmware/radio_k7_en/radio_k7_en.ino) | English comments. Adds the regional settings `AM_STEP` and `FM_DEEMPH`. |
| [`firmware/radio_k7`](firmware/radio_k7/radio_k7.ino) | Portuguese (PT-BR) comments. Fixed to the Americas (AM 10 kHz, FM 75 µs). |

Both sketches have the same logic. Upload **one** of them.

## What the firmware does

- **Analog tuning:** the TUNING knob moves the dial needle with a dial cord. The same cord turns a 10-turn potentiometer (Bourns 3590S).
  - The Nano reads the needle position on A0, filters it, and tunes the Si4732 to the frequency printed under the needle.
  - FM is linear from 88 to 108 MHz. AM is logarithmic from 530 to 1600 kHz, like the printed scale.
- **BAND switch:** selects AM or FM. The **FM STEREO** LED shows the stereo pilot.
- **Cassette deck:** drives a **CRP42602Y** logic mechanism (12 V motor, 9 V solenoid).
  - Keys: PLAY, STOP, F.FWD, REW, PAUSE and A-B (change side).
  - **Auto-reverse:** at the end of side A it plays side B; at the end of side B it stops.
  - **End of tape:** detected with the reel rotation sensor.
- **Audio switching:** a relay switches radio ⇄ tape automatically. The amplifier is muted during the switch to avoid pops.
- **Door eject:** pressing STOP/EJ with the tape stopped makes an SG90 servo release the door latch.
- **Battery monitor:** a divider on A6 reads the 3S Li-ion pack.
  - Below 9.9 V the LED blinks: charge the battery.
  - Below 9.3 V the tape stops and the amplifier is muted.
- **Power saving:** the motor is switched off when there is no cassette, or when the deck has been idle for 30 s.

## Hardware (summary)

| Function | Part |
|---|---|
| MCU | Arduino Nano (ATmega328P, 5 V) |
| Radio | Si4732-A10 (I2C, 3.3 V, through a BSS138 level shifter) |
| Tape mechanism | CRP42602Y / M02753900D (logic, auto-reverse) |
| Head preamp | TA7668 |
| Amplifier | PAM8610 2 × 10 W |
| Volume + power | Dual 10k log pot with switch (Adafruit #3482) |
| Tuning sensor | Bourns 3590S-2-103L, 10 kΩ, 10 turns |
| Door latch | SG90 micro servo |
| Power | 3 × 18650 (3S) + BMS, 12.6 V charger jack, MP1584 5 V, XL6009 14 V, LM317 12 V / 9 V |

The full schematics, wiring table and shopping list are in the manual on MakerWorld.

## Pinout (Arduino Nano)

| Pin | Connection | Notes |
|---|---|---|
| D2 | CRP42602Y pin 2 – ROTATION_SENS | interrupt, 10k pull-up |
| D3 | SG90 servo signal | |
| D4 | CRP42602Y pin 3 – GEAR_STATUS_SW | LOW = gear in function |
| D5 | CRP42602Y pin 7 – CASSETTE_DETECT_SW | LOW = cassette present |
| D6 | Solenoid driver (IRLZ44N gate) | HIGH = pull |
| D7 | Motor 12 V switch (BC337 → IRF9540N) | HIGH = motor on |
| D8 | Audio relay (2N2222) | HIGH = tape, LOW = radio |
| D9 | PAM8610 MUTE | see `AMP_MUTE_LEVEL` |
| D10 | FM STEREO / low battery LED | 1 kΩ |
| D12 | Si4732 RESET | through level shifter |
| A0 | Tuning pot wiper | 100 nF to GND |
| A1 | BAND microswitch | closed = FM |
| A4 / A5 | I2C SDA / SCL → Si4732 | through level shifter |
| A6 | Battery divider 100k / 33k | |
| A7 | Keys (resistor ladder, 10k pull-up) | |

### Key ladder (A7)

| Key | Resistor to GND | ADC value |
|---|---|---|
| STOP/EJ | 0 Ω | 0 |
| PLAY | 1 kΩ | 93 |
| REW | 2.2 kΩ | 184 |
| F.FWD | 3.9 kΩ | 288 |
| PAUSE | 6.8 kΩ | 414 |
| A-B | 15 kΩ | 614 |

### CRP42602Y connector CN1

| Pin | Signal | Connection |
|---|---|---|
| 1 | REC_B_SW | not used |
| 2 | ROTATION_SENS | D2 |
| 3 | GEAR_STATUS_SW | D4 |
| 4 | SOLENOID_1 | IRLZ44N drain |
| 5 | SOLENOID_2 | +9 V |
| 6 | GND | GND |
| 7 | CASSETTE_DETECT_SW | D5 |
| 8 | P12V | switched +12 V (motor + sensor LED) |
| 9 | REC_A_SW | not used |

## Building and uploading

1. Install the **Arduino IDE** (or `arduino-cli`).
2. Install the libraries:
   - **PU2CLR SI4735** by Ricardo Lima Caratti (Library Manager);
   - **Servo** (Library Manager).
   - **EEPROM** and **Wire** come with the AVR core.
3. Open `firmware/radio_k7_en/radio_k7_en.ino`.
4. Select **Arduino Nano**. On older clones, choose the processor *ATmega328P (Old Bootloader)*.
5. Upload. The sketch uses about 11 KB of flash.

With `arduino-cli`:

```sh
arduino-cli lib install "PU2CLR SI4735" Servo
arduino-cli compile -b arduino:avr:nano firmware/radio_k7_en
arduino-cli upload  -b arduino:avr:nano -p COM3 firmware/radio_k7_en
```

## Settings

All settings are at the top of the sketch:

| Constant | Default | Meaning |
|---|---|---|
| `AMP_MUTE_LEVEL` | `HIGH` | Level that mutes your amplifier. Some PAM8610 boards are active LOW. |
| `SERVO_REST` / `SERVO_RELEASE` | 20° / 75° | Servo angles for "latched" and "release the door" |
| `VBAT_LOW` / `VBAT_EMPTY` | 9.9 V / 9.3 V | Low battery warning and cut-off |
| `MOTOR_IDLE_OFF_MS` | 30000 | Turns the motor off after this idle time |
| `AM_STEP`, `AM_MIN`, `AM_MAX` *(EN sketch)* | 10, 530, 1600 | AM band. Use 9, 531 and 1602 in Europe, Asia, Africa and Oceania. |
| `FM_DEEMPH` *(EN sketch)* | 2 (75 µs) | Use 1 (50 µs) outside the Americas |

## Dial calibration (once)

1. With the radio off, hold **STOP/EJ** and switch on with the VOLUME knob.
2. When the LED blinks slowly, move the needle exactly to **88** and press **PLAY**.
3. When the LED blinks fast, move the needle to **108** and press **PLAY**.

The two points are stored in EEPROM. The AM scale is derived from the same calibration.

## Credits

- **CRP42602Y solenoid timing and pinout:** based on [elehobica/pico_crp42602y_ctrl](https://github.com/elehobica/pico_crp42602y_ctrl) (BSD-2-Clause).
- **Logic cassette deck reference:** [Fagear/AVRTapeControl](https://github.com/Fagear/AVRTapeControl).
- **Radio library:** [PU2CLR SI4735](https://github.com/pu2clr/SI4735) by Ricardo Lima Caratti.
