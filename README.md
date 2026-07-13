# REPUSH Dongle

This repository contains the implementation of the arduino side of the dongle.

## Repository Layout

```
REPUSH-Dongle/
├── src/
│   ├── main.cpp            Arduino firmware (enrollment, crypto, FPGA RPC)
│   ├── puf_functions.cpp   Arduino-side (legacy) PUF helpers + RNG
│   └── touch_keyboard.cpp  ILI9341 touch-screen UI
├── include/
│   ├── puf_functions.h
│   └── touch_keyboard.h
├── python_scripts/
│   ├── analyze_perf.py   Aggregates the *_perf.csv timing logs into summary stats
│   ├── test_bch.py       BCH fuzzy-commitment enroll/query test over the FPGA debug UART
│   └── *_perf.csv        Raw timing logs (enroll, sign, sensitive sign, reconfigure)
└── README.md                   This file

```

---

## How to run everything:
Read the readme file and follow the instructions for each of the repos in this order:
1. REPUSH-FPGA
2. REPUSH-Dongle
3. REPUSH-DOMAN
4. REPUSH-Server
  

## Building and Flashing
- Install PlatformIO CLI: https://docs.platformio.org/en/latest/core/installation/index.html

```bash
# Make sure to select the correct device for the upload port:
pio run -e nano_33_iot -t upload --upload-port /dev/cu.usbmodem11201
```

## Evaluation

Set up a virtual environment from the repo root and install the Python
dependencies used by the `python_scripts/` tools:

```shell
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

`analyze_perf.py` aggregates the `*_perf.csv` timing logs (enrollment, signing,
sensitive signing, reconfiguration) and prints min/max/mean/median/stdev for
each phase. The actual readings were gathered from the user interface by simply copy and pasting.

```shell
python python_scripts/analyze_perf.py
```