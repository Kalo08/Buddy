# Buddy on Waveshare ESP32-P4

Everything is in **one sketch: [`buddy_p4.ino`](buddy_p4.ino)**. Open it in
the Arduino IDE and upload. It speaks the same WebSocket protocol as the Pi,
so you use the same hub and web UI: enter the `BDY-XXXXX` ID in the browser
and drive.

## Arduino IDE setup

1. **Board package:** File → Preferences → Additional boards manager URLs:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   Then in Boards Manager, install **esp32 by Espressif, version 3.3.0 or newer**.
   That version includes the P4 camera stack, the OV5640 driver, the JPEG
   encoder and the C6 WiFi bridge.
2. **Library:** Library Manager → **WebSockets** by Markus Sattler.
3. **Tools menu:**

   | Setting | Value |
   |---|---|
   | Board | ESP32P4 Dev Module |
   | PSRAM | Enabled |
   | Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
   | USB CDC On Boot | Disabled when plugged into the **USB to UART** port (recommended). Enabled only if you use the other (OTG) port. |
   | Chip Variant | match your chip. Boards made before late 2025 are often "before v3.00". If the upload complains about the chip revision, switch this. |

4. Edit the config block at the top of the sketch (WiFi, hub host, ID) and upload.

## Hardware

| Part | Connection |
|---|---|
| OV5640 camera | MIPI-CSI ribbon connector |
| Speaker | onboard ES8311 codec + amp → SPK header |
| Wheel servos | front-left GPIO20, front-right GPIO21, back GPIO22 (change `SERVO_PIN_*` if those aren't free on your header) |
| WiFi | onboard ESP32-C6, nothing to wire |

> The servos need their own 5 V supply. Tie its ground to the board's GND.

## What the sketch does

- **Camera:** the OV5640 captures RGB565 at 1280×720. The P4's PPA scales it
  down to fit 320×240 (it streams 320×176 to keep the aspect ratio), then
  the hardware JPEG encoder compresses it at quality 50 and 15 fps. Those are
  the same settings as the Pi. You can change them with the `CAM_*` defines.
  If colors look wrong, set `CAM_SWAP true`.
- **Speaker:** the P4 can't decode the browser's opus audio. When a browser
  pairs, the sketch sends `{"type":"hello","audio":"pcm16"}`, and `client.js`
  then sends raw 16 kHz PCM, which is played through the ES8311. Change the
  volume with `SPK_VOLUME`.
- **Wheels:** the drive logic is ported from `pi/buddy_pi.py`: same wheel
  geometry, steering softening, back-wheel correction and stall-protection
  rest cycling. Servos still get no signal at idle. Pulses come from LEDC
  hardware PWM.

## Serial monitor on a good boot (115200)

```
[buddy] booting (ESP32-P4) — id: BDY-00001
[srv] LEDC servos ready (GPIO 20/21/22) — outputs off until driven
[spk] ES8311 ready — PCM16 mono 16000 Hz
[cam] sensor: OV5640
[cam] ready — capture 1280x720 → stream 320x176 @ 15 fps, q50
[wifi] 192.168.x.x
[ws]  connected — id: BDY-00001
```

## Servo calibration

`NEUTRAL_US` in the sketch was measured on the Pi. LEDC pulses are exact,
whereas the Pi's software PWM is not, so a wheel may creep slightly. From
the browser console:

```js
ws.send(JSON.stringify({type:"servo", ch:0, angle:90}))   // sweep the angle
ws.send(JSON.stringify({type:"servo", ch:0, angle:-1}))   // -1 = output off
```

The serial monitor prints the µs value for each command. Put the one where
the wheel stops into `NEUTRAL_US[ch]`.

## Not included (yet)

- Mic → browser. The board has an ES8311 ADC and a mic input, but this
  isn't wired up.
