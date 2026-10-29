# Buddy on Waveshare ESP32-P4

Everything is in **one sketch: [`buddy_p4.ino`](buddy_p4.ino)**. Open it in
the Arduino IDE and upload. It speaks the same WebSocket protocol as the Pi,
so you use the same hub and web UI: enter the `BDY-XXXXX` ID in the browser
and drive.

## Arduino IDE setup

1. **Board package:** File → Preferences → Additional boards manager URLs:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   Then in Boards Manager, install **esp32 by Espressif, version 3.3.0 or newer**.
   That version includes the P4 camera stack, the OV5647/OV5640 drivers, the JPEG
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
| OV5647 (or OV5640) camera | MIPI-CSI ribbon connector (detected automatically) |
| Speaker | onboard ES8311 codec + amp → SPK header |
| Wheel servos | front-left GPIO20, front-right GPIO21, back GPIO22 (change `SERVO_PIN_*` if those aren't free on your header) |
| WiFi | onboard ESP32-C6, nothing to wire |

> The servos need their own 5 V supply. Tie its ground to the board's GND.

## USB webcam (instead of the ribbon camera)

`USE_USB_WEBCAM` near the top of the sketch picks the camera:
`false` = ribbon camera on MIPI-CSI (the default), `true` = USB webcam.

- Plug the webcam into the **USB OTG** port. Keep the board powered and
  connected to your PC through the **USB to UART** port.
- It needs a standard UVC webcam that can send **MJPEG**. Most cheap USB
  webcams can.
- The sketch picks the webcam's smallest MJPEG size that's at least 320×240.
  It forwards the camera's own JPEG frames without re-encoding, capped at
  `CAM_FPS`.
- Plugging and unplugging while it runs is fine. The stream restarts on its own.

Serial Monitor when it works:

```
[usb] waiting for a webcam on the USB OTG port...
[usb] webcam connected (addr 1) — formats:
[usb]   MJPEG 640x480
[usb]   MJPEG 320x240
[usb] webcam streaming MJPEG 320x240
```

If something goes wrong:

- **No `webcam connected` line:** the port may not be powering the camera.
  Check whether the webcam's LED lights up. If it doesn't, go through a
  powered USB hub.
- **`USB host init FAILED`:** set **Tools → USB Mode → Hardware CDC and
  JTAG** and upload again.
- **`no MJPEG mode found`:** that webcam only sends raw video. Use a
  different webcam.

## What the sketch does

- **Camera:** the ribbon camera is detected automatically. The OV5647
  captures a square 800×800 raw image. The P4's ISP converts it to color, the
  PPA scales it to 448×448, and the hardware JPEG encoder compresses it at
  quality 70 and 15 fps. If video lags on a weak connection, lower
  `CAM_FPS` or go back to `CAM_MAX_W/H 320` and `CAM_QUALITY 50`. An OV5640 captures 1280×720 instead and streams
  320×176. Change these with the `CAM_*` defines. If colors look wrong, set
  `CAM_SWAP true`.
- **Speaker:** the P4 can't decode the browser's opus audio. When a browser
  pairs, the sketch sends `{"type":"hello","audio":"pcm16"}`, and `client.js`
  then sends raw 16 kHz PCM, which is played through the ES8311. Change the
  volume with `SPK_VOLUME`: 1 dB per step, where 100 = 0 dB. Above 100 is
  extra boost, up to 132; the default is 118. Lower it if it distorts.
- **Wheels:** the drive logic is ported from `pi/buddy_pi.py`: same wheel
  geometry, steering softening, back-wheel correction and stall-protection
  rest cycling. Servos still get no signal at idle. Pulses come from LEDC
  hardware PWM.

## Serial monitor on a good boot (115200)

```
[buddy] booting (ESP32-P4) — id: BDY-00001
[srv] LEDC servos ready (GPIO 20/21/22) — outputs off until driven
[spk] ES8311 ready — PCM16 mono 16000 Hz
[cam] sensor: OV5647
[cam] ready — capture 800x800 → stream 448x448 @ 15 fps, q70
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

## Microphone

The onboard mic streams to the browser while a browser is paired, the same
way the Pi's mic did. Sound comes out of the page when the **SPEAKER** toggle
is on.

- **Full duplex (talk and listen at once):** `MIC_FULL_DUPLEX true`, the
  default. The ES8311 copies what the speaker plays into the mic stream as
  a reference, and Espressif's echo canceller (esp-sr AEC) subtracts it from
  the mic, so you don't hear your own voice come back. The boot log shows
  `[mic] echo canceller on`.
  - **Mic sounds like the speaker and not the room:** set
    `AEC_SWAP_SLOTS true`.
  - **Still some echo:** lower `SPK_VOLUME` or `MIC_GAIN`. Loud playback
    that clips the mic can't be cancelled. Keeping the speaker and mic apart
    on the robot also helps.
  - **Fallback:** `MIC_FULL_DUPLEX false` goes back to half duplex. The mic
    is then muted while the speaker plays, plus `MIC_ECHO_GUARD_MS` after.
- **Level:** `MIC_GAIN` is the codec's analog gain (0–7, 6 dB per step;
  default 5). `MIC_BOOST` is extra digital gain applied after echo
  cancelling (default 6×). The
  Serial Monitor prints a `[mic] peak` line every second. Speech should
  land around 3000–20000. If it's always near 32767, it's distorting:
  lower `MIC_BOOST`.
- **Choppy audio:** audio goes out in 100 ms packets (`MIC_CHUNK_MS`). If
  the `[mic]` line shows `dropped` above 0, the connection can't keep up.
  Lower `CAM_FPS` or `CAM_QUALITY` to free up bandwidth.
- **Turn it off:** set `MIC_ENABLED false`.
