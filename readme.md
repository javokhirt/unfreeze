# Unfreeze

A wearable for Parkinson's patients that catches a freezing episode the moment it starts and cues the person back into their next step.

Won the Research Track at HackPSU Fall 2026.

## The problem

Freezing of gait is when the feet stick to the floor mid step while the body keeps moving. It affects up to 70% of Parkinson's patients after ten years and is a leading cause of falls.

A steady rhythm can break a freeze, but existing devices fire constantly. Unfreeze only fires when a freeze actually happens.

## How it works

1. A motion sensor on the shin samples at 64 Hz.
2. The chip splits the motion into walking (0.5 to 3 Hz) and trembling (3 to 8 Hz) and compares them. This ratio is the freeze index.
3. A freeze is flagged when the index is high and the leg is actually moving, so standing still never triggers it.
4. The chip fires the cue itself: vibration, a beat, or a voice prompt. No network needed.
5. A laptop script logs every episode and uses a bandit algorithm to learn which cue works best for that person.

## Hardware

ESP32-S3, MPU-6050, vibration motor, passive buzzer, two LEDs, one button for logging doses.

| Part | Pin |
| --- | --- |
| MPU-6050 SDA / SCL | GPIO8 / GPIO9 |
| Vibration motor | GPIO4 |
| Buzzer | GPIO5 |
| LEDs | GPIO6, GPIO10 |
| Dose button | GPIO7 |

## Run it

1. Put your Wi-Fi name, password, and laptop IP at the top of `src/main.cpp`.
2. Flash the board:
   ```
   pio run -t upload
   ```
3. Start the laptop script:
   ```
   python3 bridge.py
   ```
4. Strap it on, press RESET, and stand still for two seconds.

Type `f` in the script to force a freeze, `s` to stop it, and `cue 0`, `1`, or `2` to switch between beat, vibration, and voice.

Logs are saved as `episodes.csv`, `doses.csv`, and `events.csv`.

## Not a medical device

A research prototype, not tested on patients.