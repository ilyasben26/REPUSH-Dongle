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

### BCH fuzzy-commitment test

`test_bch.py` connects to the FPGA's debug UART and drives a BCH
enroll-then-query loop to verify the PUF response can be reliably
error-corrected back to the same seed, make sure that the FPGA has been flashed accordingly first:

```shell
python python_scripts/test_bch.py --port /dev/tty.usbserial-XXXX
```

Before running it, initialize the FPGA over the same debug UART:

```
si          # init SD card
ps          # scan and save PUF challenges
rs 0 12345  # init LR-PUF state slot 0 with some seed
```

Optional flags: `--state` (LR-PUF state slot, default `0`), `--challenge`
(challenge ID, default `1`), `--queries` (number of query iterations, default
`20`), `--debug` (print raw UART traffic).

