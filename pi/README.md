# Buddy on Raspberry Pi 3

The device side of Buddy, replacing the fried ESP32-CAM. Speaks the same
WebSocket protocol to the hub server, so `server.js` / the web UI are
unchanged — enter the same `BDY-XXXXX` ID in the browser and drive.

## Hardware

| Part | Connection |
|---|---|
| USB camera | any USB port (`/dev/video0`) |
| Wheel servos ×3 | signal wires straight on the GPIO header: pin 11 (GPIO17) = front-left, pin 13 (GPIO27) = front-right, pin 15 (GPIO22) = back (two in front, one sideways in back) |
| Speaker (optional) | 3.5mm jack, HDMI, or USB audio |

> The servos still need their own 5V supply — don't power them from the Pi's
> 5V rail. Tie the supply's ground to a Pi GND pin so the signal has a
> common reference.

## Setup (Raspberry Pi OS)

```bash
# 1. System packages
sudo apt update
sudo apt install -y python3-opencv ffmpeg python3-rpi.gpio python3-pip

# 2. Python packages
pip3 install websockets

# 3. Sanity check
ls /dev/video*             # should show video0 (the USB camera)
```

## Run

```bash
python3 buddy_pi.py --id BDY-00001
```

Defaults match the old firmware config (Railway host, wss on 443). Options:

```
--host    hub server hostname          (default: buddy-production-948c.up.railway.app)
--port    hub port, 443 = wss          (default: 443)
--id      buddy ID                     (default: BDY-00001)
--camera  /dev/videoN index            (default: 0)
--width / --height / --fps / --quality video settings (default: 320x240 @ 15fps, q50 — low for latency)
-v        verbose logging (per-servo writes, etc.)
```

For a local hub: `python3 buddy_pi.py --host 192.168.1.50 --port 3000`

## How the servos are driven

These are pot-decoupled continuous-rotation conversions: the servo board
still thinks it's positional, but its pot is frozen, so ANY pulse drives the
motor toward wherever the frozen pot happens to sit. There is no universally
safe "stop pulse" — so at idle the code sends **no signal at all** (GPIO PWM
stopped, pins low; wheels limp and silent). Pulses only flow while a
button is held, and releasing the button / losing the connection cuts them.

Pulses come from RPi.GPIO software PWM on pins 11/13/15, which carries a bit
of timing jitter — harmless for continuous-rotation wheels.

## Servo calibration (finding each wheel's neutral)

`NEUTRAL_US` in `buddy_pi.py` is each servo's frozen-pot pulse — it sets the
midpoint that drive commands offset from. To find it, run the script, connect
a browser, open its console (F12), and per wheel:

```js
ws.send(JSON.stringify({type:"servo", ch:0, angle:90}))   // sweep the angle
ws.send(JSON.stringify({type:"servo", ch:0, angle:-1}))   // -1 = output off
```

Sweep the angle (0–180, fractions allowed) until the wheel stops moving —
the Pi's log prints the pulse in µs for every command. Put that µs value in
`NEUTRAL_US[ch]`. If a wheel never stops at any angle, its pot froze outside
the pulse range: it will still drive, but only one direction will be strong —
consider re-gluing that pot near center.

If the robot is too fast/jumpy, lower `DRIVE_US` (200 = full speed; try 100).

## Start on boot (systemd)

```bash
sudo tee /etc/systemd/system/buddy.service > /dev/null <<'EOF'
[Unit]
Description=Buddy device client
After=network-online.target
Wants=network-online.target

[Service]
User=pi
ExecStart=/usr/bin/python3 /home/pi/Buddy2/pi/buddy_pi.py --id BDY-00001
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now buddy
journalctl -u buddy -f      # watch logs
```

(Adjust the path/User if you cloned the repo elsewhere.)

## What changed vs the ESP32

- **Camera**: onboard OV2640 → USB UVC camera via OpenCV. Default is
  320×240 @ q50 to keep latency down; raise with `--width 640 --height 480
  --quality 70` if you want a sharper picture.
- **Speaker**: the hold-to-talk audio path is now actually enabled — incoming
  browser audio plays through the Pi's audio output via `ffplay`. (It was
  disabled on the ESP32-CAM.)
- **Servos**: no PCA9685 anymore — signal wires go straight to the Pi's GPIO
  header (pins 11/13/15, software PWM). Same channels, same drive math.
- The `firmware/` directory and `flash.html` flashing tool are now legacy.
