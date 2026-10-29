# Buddy main board: engineering notebook

A dated log of the design decisions and progress for the Buddy robot's main
PCB. The KiCad project is in `../` (`buddy_main.kicad_pro`), and the images
are in `img/`.

---

## Entry 1 · 2026-10-29 · Requirements and architecture

### Requirements 

| # | Requirement | Notes |
|---|---|---|
| R1 | One PCB acts as Buddy's main computer | Replaces the Waveshare P4-Nano dev board |
| R2 | Plug in a **speaker** | |
| R3 | Plug in a **mic** | |
| R4 | Drive **3 brushed DC motors**, no encoders | Each needs an H-bridge |
| R5 | Plug in the **camera** | |
| R6 | Power from **USB-C or a 2S LiPo** | "Easy-to-use batteries" |
| R7 | **Can't be fried** by plugging in the battery wrong | Reverse polarity, wrong battery, short |
| R8 | Designed in **KiCad**, every part with a **3D model** | |
| R9 | Progress documented in this notebook with images | |

### Block diagram

![Block diagram](img/01_block_diagram.svg)

### Decision log

**D1. Processor: ESP32-S3-WROOM-1 (N16R8) module, not a bare ESP32-P4.**
- The module already contains the antenna, RF matching, 16 MB flash and
  8 MB PSRAM. The board only needs power, a reset circuit and connectors.
  It can be hand-soldered, since its pads are castellated at 1.27 mm pitch.
- A bare ESP32-P4 would need a QFN-104 package, an external core supply,
  an ESP32-C6 for WiFi, and MIPI CSI routing. That's far more risk for a
  first board.
- Trade-off: the ESP32-S3 has no MIPI CSI input, so the current OV5647
  camera can't be used. The board takes the standard **24-pin DVP
  camera** (OV2640) instead. That camera is well supported in Arduino by
  `esp32-camera`, which the original Buddy ESP32-CAM firmware already used.
- The S3 has **native USB**, so the board needs no separate USB-to-UART
  chip. Programming and serial both go over the USB-C port.

**D2. Battery input protection: LTC4365 controller + back-to-back N-MOSFETs.**
This covers R7. One small controller handles every common "plugged it in
wrong" case:

| Fault | What happens |
|---|---|
| Battery plugged in backwards | LTC4365 survives −40 V and keeps both MOSFETs off. No current flows. |
| Too-big battery (3S, 12 V supply) | Over-voltage trip at **9.7 V** turns the MOSFETs off before the motor drivers (max 10.8 V) see it |
| LiPo running flat | Under-voltage trip at **6.1 V** (≈3.05 V/cell) disconnects the load, which protects the battery |
| Short or stalled motors | 3 A PTC fuse (resettable) on the battery input |
| Voltage spikes | ±24 V bidirectional TVS. It's bidirectional so it **doesn't** conduct when the battery is reversed |

Divider for the UV/OV thresholds (both comparators trip at 0.5 V):

```
VBAT ─ R1 1 MΩ ─┬─ OV ─ R2 33 kΩ ─┬─ UV ─ R3 56 kΩ ─ GND

V_OV = 0.5 V × (R1+R2+R3)/R3        = 0.5 × 1089k/56k = 9.72 V
V_UV = 0.5 V × (R1+R2+R3)/(R2+R3)   = 0.5 × 1089k/89k = 6.12 V
Divider current at 8.4 V ≈ 7.7 µA (negligible battery drain)
```

The power switch pulls LTC4365 `~SHDN` low. It only carries microamps, so
a tiny slide switch can turn off a multi-amp battery path.

**D3. USB input protection: 1.1 A PTC + 5 V TVS + Schottky diode.**
- The PTC protects the laptop's USB port from a board fault.
- The Schottky diode stops the 7.4 V battery from pushing current back
  into the USB host when both are plugged in. When both are present, the
  higher voltage (the battery) powers the board automatically, with no
  switching logic needed.
- 5.1 kΩ resistors on CC1 and CC2 make the port identify as a 5 V sink
  (default USB-C current).

**D4. One system rail (VSYS, 4.6–9.7 V) feeds everything.**
- Motors run directly from VSYS (DRV8833 range 2.7–10.8 V, so both USB
  and 2S LiPo work).
- **3V3**: AP63203 buck converter, 2 A. A linear regulator from 8.4 V would
  burn about 2.5 W at 500 mA.
- **5V**: AP63205 buck converter, 2 A, for the speaker amp (≈3 W at 5 V vs ≈1 W
  at 3.3 V). On USB-only power it runs in dropout at about 4.3 V, which
  the amp accepts (2.5–5.5 V).
- **2V8 / 1V2**: XC6206 LDOs from 3V3 for the camera's analog and core
  supplies (milliamps, so linear regulators are fine).

**D5. Motor drivers: 2× DRV8833 dual H-bridge (3 of 4 bridges used).**
- 1.5 A RMS / 2 A peak per bridge, and both USB and 2S voltages are in range.
- 0.1 Ω sense resistors set a **2 A current limit per motor**
  (I = 0.2 V / 0.1 Ω). A stalled motor is limited, not cooked.
- `~SLEEP` is shared and pulled low, so the motors stay off until the
  firmware enables them. `~FAULT` is shared and reported to the ESP32.

**D6. Audio: MAX98357A I2S amp (speaker) + I2S MEMS mic header.**
- The amp and the mic share one I2S bus: shared BCLK and WS, data out to
  the amp, data in from the mic. That's full duplex on 4 GPIOs.
- The mic plugs in as a common INMP441 breakout module, as R3 asks.

**D7. Connectors are keyed and polarized so they can't go in backwards.**
- XT30 / JST-PH for the battery, JST-XH for the motors, JST-PH for the
  speaker, and a 24-pin FPC for the camera.

### ESP32-S3 pin map

The pins avoid the flash/PSRAM (GPIO26–37), USB (19/20), UART0 (43/44)
and the strapping pins. The few strapping pins used are only ones whose
boot level is safe.

| Function | GPIO | Notes |
|---|---|---|
| Camera Y2–Y9 | 11, 9, 8, 10, 12, 18, 17, 16 | Same as the ESP32-S3-EYE, so the stock `esp32-camera` config works |
| Camera XCLK / PCLK | 15 / 13 | |
| Camera VSYNC / HREF | 6 / 7 | |
| Camera SIOD / SIOC | 4 / 5 | 4.7 kΩ pull-ups |
| I2S BCLK / WS | 41 / 42 | Shared by amp + mic |
| I2S out (amp DIN) | 40 | |
| I2S in (mic SD) | 39 | |
| Motor 1 IN1 / IN2 | 2 / 14 | |
| Motor 2 IN1 / IN2 | 21 / 38 | |
| Motor 3 IN1 / IN2 | 47 / 48 | |
| Motor `~SLEEP` | 45 | Strapping pin. The 100 kΩ pull-down holds the required low at boot and keeps the motors asleep |
| Motor `~FAULT` | 3 | 10 kΩ pull-up (open drain) |
| Battery voltage | 1 | ADC1. 100k/33k divider: 8.4 V → 2.08 V |
| Status LED | 46 | Strapping pin. The LED to GND keeps it low at boot |
| BOOT button | 0 | |
| USB D− / D+ | 19 / 20 | Native USB-Serial/JTAG |

### Open questions / risks
- The 24-pin camera pinout varies between suppliers. I'm using the common
  OV2640 "ESP32-CAM" pinout. **Check it against your camera's datasheet
  before ordering.**
- No onboard LiPo charger. Charge the battery with a balance charger off
  the robot.
