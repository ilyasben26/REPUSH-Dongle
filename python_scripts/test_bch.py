#!/usr/bin/env python3
"""
BCH fuzzy commitment test script for REPUSH FPGA.
Connects to the FPGA debug UART (USB-C port) and uses the text shell.

Commands sent to FPGA:
    be <state_hex> <challenge_hex>   -- enroll (10-vote majority, writes SD)
    bq <state_hex> <challenge_hex>   -- query  (single-shot, BCH-corrected)

FPGA responds with a line containing:
    BCH_SEED:<64 hex chars>          -- success
    BCH_ERR                          -- failure

Prerequisites on FPGA (via the same terminal before running this script):
    si          -- init SD card
    ps          -- scan and save PUF challenges
    rs 0 12345  -- init LR-PUF state slot 0 with some seed

Usage:
    python3 test_bch.py --port /dev/tty.usbserial-* [--state 0] [--challenge 1] [--queries 20]
"""

import time
import argparse
import sys

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial")
    sys.exit(1)


class FPGADebugLink:
    def __init__(self, port: str, baud: int = 115200, debug: bool = False):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.debug = debug
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def _send(self, cmd: str):
        self.ser.reset_input_buffer()
        if self.debug:
            print(f'  [TX] {cmd!r}')
        self.ser.write((cmd + '\r\n').encode('ascii'))

    def _read_result(self, timeout: float) -> str:
        """
        Read lines until one contains BCH_SEED: or BCH_ERR.
        Returns the matching line, or None on timeout.
        """
        deadline = time.time() + timeout
        buf = b''
        while time.time() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                if self.debug:
                    print(f'  [RX] {chunk!r}')
                buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                decoded = line.decode('ascii', errors='replace').strip()
                if 'illegal command' in decoded:
                    return '__ILLEGAL__'
                if 'BCH_SEED:' in decoded or 'BCH_ERR' in decoded:
                    return decoded
        return None

    def bch_enroll(self, state_idx: int, challenge_id: int) -> bytes:
        """Enroll: returns 32-byte seed."""
        self._send(f'be {state_idx:x} {challenge_id:x}')
        line = self._read_result(timeout=30.0)
        if line is None:
            raise TimeoutError('timeout — no response. Is firmware rebuilt+flashed? Is another terminal open on this port?')
        if line == '__ILLEGAL__':
            raise RuntimeError('"be" not recognized — rebuild and reflash firmware (make clean && make 20k)')
        if 'BCH_ERR' in line:
            raise RuntimeError('FPGA returned BCH_ERR — run: si, ps, rs <state> <seed> on debug UART first')
        idx = line.index('BCH_SEED:')
        hex_str = line[idx + 9:].strip()
        if len(hex_str) != 64:
            raise RuntimeError(f'bad seed length ({len(hex_str)} chars): {hex_str!r}')
        return bytes.fromhex(hex_str)

    def bch_query(self, state_idx: int, challenge_id: int) -> bytes:
        """Query: returns 32-byte seed."""
        self._send(f'bq {state_idx:x} {challenge_id:x}')
        line = self._read_result(timeout=15.0)
        if line is None:
            raise TimeoutError('timeout — no response from FPGA')
        if line == '__ILLEGAL__':
            raise RuntimeError('"bq" not recognized — rebuild and reflash firmware')
        if 'BCH_ERR' in line:
            raise RuntimeError('FPGA returned BCH_ERR — uncorrectable PUF errors or not enrolled')
        idx = line.index('BCH_SEED:')
        hex_str = line[idx + 9:].strip()
        if len(hex_str) != 64:
            raise RuntimeError(f'bad seed length ({len(hex_str)} chars): {hex_str!r}')
        return bytes.fromhex(hex_str)

    def close(self):
        self.ser.close()


def main():
    parser = argparse.ArgumentParser(description='BCH PUF test via FPGA debug UART')
    parser.add_argument('--port',      required=True, help='Serial port, e.g. /dev/tty.usbserial-*')
    parser.add_argument('--baud',      type=int, default=115200)
    parser.add_argument('--state',     type=int, default=0, help='LR-PUF state slot (must be initialized)')
    parser.add_argument('--challenge', type=int, default=1, help='Challenge ID (decimal)')
    parser.add_argument('--queries',   type=int, default=20, help='Number of query iterations')
    parser.add_argument('--debug',     action='store_true',  help='Print all raw UART traffic')
    args = parser.parse_args()

    print(f'Opening {args.port} at {args.baud} baud...')
    link = FPGADebugLink(args.port, args.baud, debug=args.debug)

    # BCH Enroll
    print(f'\n=== BCH Enroll: state={args.state}, challenge={args.challenge:#x} ===')
    try:
        enroll_seed = link.bch_enroll(args.state, args.challenge)
    except Exception as e:
        print(f'FAILED: {e}')
        link.close()
        sys.exit(1)
    print(f'Enroll seed: {enroll_seed.hex()}')

    # BCH Query loop
    print(f'\n=== BCH Query x{args.queries} ===')
    passed = failed = errors = 0

    for i in range(args.queries):
        try:
            seed = link.bch_query(args.state, args.challenge)
            match = (seed == enroll_seed)
            if match:
                passed += 1
            else:
                failed += 1
            status = 'PASS' if match else 'FAIL'
            print(f'  [{i+1:3d}] {status}  seed={seed.hex()[:16]}...')
        except Exception as e:
            errors += 1
            print(f'  [{i+1:3d}] ERROR: {e}')

    print(f'\n=== Results: {passed}/{args.queries} PASS, {failed} wrong seed, {errors} errors ===')
    if failed == 0 and errors == 0:
        print('BCH PUF PASSED: all queries match enrollment seed.')
    else:
        print('BCH PUF FAILED: some queries produced wrong seed or errored.')
        link.close()
        sys.exit(1)

    link.close()


if __name__ == '__main__':
    main()
