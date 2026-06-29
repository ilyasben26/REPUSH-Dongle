import csv
import statistics
import sys
from pathlib import Path

ENROLL_CSV = Path(__file__).parent / "enroll_perf.csv"
SIGN_CSV   = Path(__file__).parent / "sign_perf.csv"

ENROLL_COLUMNS = {
    "dongle_enroll_ms":        "Dongle enroll (ms)",
    "dongle_ack_ms":           "Dongle acknowledge (ms)",
    "server_verify_crypto_ms": "Server verify crypto (ms)",
}

SIGN_COLUMNS = {
    "roundtrip_s":      "Round trip (s)",
    "dongle_sign_ms":   "Dongle sign (ms)",
    "server_verify_ms": "Server verify (ms)",
}


def load(path: Path, columns: dict) -> dict[str, list[float]]:
    data: dict[str, list[float]] = {k: [] for k in columns}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            for col in columns:
                data[col].append(float(row[col]))
    return data


def stats(values: list[float]) -> dict:
    n = len(values)
    return {
        "n":      n,
        "min":    min(values),
        "max":    max(values),
        "mean":   statistics.mean(values),
        "median": statistics.median(values),
        "stdev":  statistics.stdev(values) if n > 1 else 0.0,
    }


def print_table(title: str, data: dict[str, list[float]], columns: dict) -> None:
    col_w = 28
    print(f"\n{title}")
    print(f"{'Metric':<{col_w}}  {'n':>4}  {'min':>8}  {'max':>8}  {'mean':>8}  {'median':>8}  {'stdev':>8}")
    print("-" * (col_w + 52))
    for col, label in columns.items():
        s = stats(data[col])
        print(
            f"{label:<{col_w}}  {s['n']:>4}  "
            f"{s['min']:>8.3f}  {s['max']:>8.3f}  "
            f"{s['mean']:>8.3f}  {s['median']:>8.3f}  {s['stdev']:>8.3f}"
        )
    print()


def main() -> None:
    enroll_data = load(ENROLL_CSV, ENROLL_COLUMNS)
    sign_data   = load(SIGN_CSV,   SIGN_COLUMNS)

    print_table("=== Phase 1: Enrollment ===", enroll_data, ENROLL_COLUMNS)
    print_table("=== Phase 2: Non-sensitive signing ===", sign_data, SIGN_COLUMNS)


if __name__ == "__main__":
    main()
