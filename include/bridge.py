#!/usr/bin/env python3
"""Unfreeze laptop bridge.

Receives events from the ankle unit, logs them to CSV, plays the spoken
cue, and runs a Thompson sampling bandit that picks the next cue.

Wi-Fi:   python3 bridge.py
USB:     python3 bridge.py --serial /dev/cu.usbserial-120
"""
import csv
import json
import os
import random
import select
import socket
import subprocess
import sys
import time

EVENT_PORT = 4210
CMD_PORT = 4211
VOICE_FILE = "step.mp3"        # ElevenLabs clip saying "step, step, step"
VOICE_REPEAT_S = 3.0
SUCCESS_S = 5.0                # unfreezing within this counts as a success
ARMS = {0: "beat", 1: "vibration", 2: "voice"}
STATE_FILE = "bandit_state.json"


class UdpLink:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", EVENT_PORT))
        self.sock.settimeout(0.2)
        self.device = None

    def recv(self):
        try:
            data, addr = self.sock.recvfrom(512)
        except socket.timeout:
            return None
        self.device = addr[0]
        return data.decode(errors="ignore").strip()

    def send(self, text):
        if self.device:
            self.sock.sendto(text.encode(), (self.device, CMD_PORT))


class SerialLink:
    def __init__(self, port):
        import serial  # pip install pyserial
        self.ser = serial.Serial(port, 115200, timeout=0.2)

    def recv(self):
        line = self.ser.readline().decode(errors="ignore").strip()
        return line or None

    def send(self, text):
        self.ser.write((text + "\n").encode())


def typed_command():
    """Return a line typed into the terminal, or None."""
    if not sys.stdin.isatty():
        return None
    ready, _, _ = select.select([sys.stdin], [], [], 0)
    if not ready:
        return None
    line = sys.stdin.readline().strip()
    return line or None


def load_state():
    if os.path.exists(STATE_FILE):
        with open(STATE_FILE) as f:
            return {int(k): v for k, v in json.load(f).items()}
    return {arm: [1, 1] for arm in ARMS}  # Beta(1, 1) prior per cue


def save_state(state):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f)


def choose(state):
    draws = {arm: random.betavariate(a, b) for arm, (a, b) in state.items()}
    return max(draws, key=draws.get)


def play_voice():
    if not os.path.exists(VOICE_FILE):
        return
    if sys.platform == "darwin":
        subprocess.Popen(["afplay", VOICE_FILE])
    elif sys.platform.startswith("linux"):
        subprocess.Popen(["mpg123", "-q", VOICE_FILE])


def append_csv(path, header, row):
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(header)
        w.writerow(row)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--serial":
        link = SerialLink(sys.argv[2])
    else:
        link = UdpLink()
    print("listening... walk, then tremble in place to trigger a freeze")

    state = load_state()
    next_arm = choose(state)
    cue_sent = False
    freezing = False
    freeze_cue = None
    last_voice = 0.0

    print("type: f = force freeze, s = stop, cue 0|1|2, fion X, pw X")

    while True:
        cmd = typed_command()
        if cmd:
            link.send(cmd)
            print(f"sent: {cmd}")

        line = link.recv()
        now = time.time()

        if freezing and freeze_cue == 2 and now - last_voice > VOICE_REPEAT_S:
            play_voice()
            last_voice = now

        if not line:
            continue
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            print(line)
            continue

        t = ev.get("t")
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        append_csv("events.csv", ["time", "type", "json"], [stamp, t, line])

        if t in ("hello", "boot"):
            cue_sent = False
        if not cue_sent:
            link.send(f"cue {next_arm}")
            cue_sent = True

        if t == "fi":
            state_txt = "FREEZE" if ev["st"] else "ok"
            print(f"\rFI {ev['fi']:6.2f}   power {ev['pw']:7.3f}   {state_txt:<7}",
                  end="", flush=True)

        elif t == "freeze_start":
            freezing = True
            freeze_cue = ev["cue"]
            print(f"\nfreeze detected, cue: {ARMS[freeze_cue]}")
            if freeze_cue == 2:
                play_voice()
                last_voice = now

        elif t == "freeze_end":
            freezing = False
            cue = ev["cue"]
            dur = ev["dur"] / 1000.0
            success = dur <= SUCCESS_S and ev.get("why") == "resolved"
            state[cue][0 if success else 1] += 1
            save_state(state)
            next_arm = choose(state)
            link.send(f"cue {next_arm}")
            append_csv("episodes.csv",
                       ["time", "duration_s", "cue", "success", "reason"],
                       [stamp, f"{dur:.1f}", ARMS[cue], success, ev.get("why")])
            rates = "  ".join(f"{ARMS[a]} {s[0]}/{s[0] + s[1]}" for a, s in state.items())
            print(f"\nunfroze in {dur:.1f} s with {ARMS[cue]} "
                  f"({'success' if success else 'slow'}) | {rates} | next: {ARMS[next_arm]}")

        elif t == "dose":
            append_csv("doses.csv", ["time"], [stamp])
            print("\ndose logged")

        elif t in ("err", "hello", "cfg"):
            print(f"\n{line}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")