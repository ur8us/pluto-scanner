#!/usr/bin/env python3
"""Check CIC frame accounting for clean and corrupted sample streams."""

import argparse
import json


class CicContinuityModel:
    def __init__(self, fft_size, decim, hop=None):
        self.fft_size = int(fft_size)
        self.decim = int(decim)
        self.hop = int(hop if hop is not None else fft_size)
        self.fill = 0
        self.phase = 0
        self.expected_sample = 0
        self.raw_total = 0
        self.decim_total = 0
        self.frames_total = 0
        self.account_raw = 0
        self.account_decim = 0
        self.account_frames = 0
        self.published = 0
        self.skipped = 0
        self.skip_frames = 0
        self.sequence_errors = 0
        self.accounting_errors = 0

    def reset_cic(self, skip_frames=1):
        self.fill = 0
        self.phase = 0
        self.account_raw = 0
        self.account_decim = 0
        self.account_frames = 0
        self.skip_frames = int(skip_frames)

    def process_sample(self, sample_id):
        if sample_id != self.expected_sample:
            self.sequence_errors += 1
            self.expected_sample = sample_id + 1
        else:
            self.expected_sample += 1

        self.raw_total += 1
        self.account_raw += 1
        self.phase += 1
        if self.phase < self.decim:
            return
        self.phase = 0
        self.decim_total += 1
        self.account_decim += 1
        self.fill += 1

        if self.fill < self.fft_size:
            return

        self.frames_total += 1
        self.account_frames += 1
        if self.skip_frames > 0:
            self.skipped += 1
            self.skip_frames -= 1
        else:
            self.published += 1

        if self.hop >= self.fft_size:
            self.fill = 0
        else:
            self.fill = self.fft_size - self.hop

    def process_range(self, start, count):
        for sample_id in range(start, start + count):
            self.process_sample(sample_id)

    def check_accounting(self):
        expected_decim = self.account_frames * self.hop + self.fill
        expected_raw = expected_decim * self.decim + self.phase
        ok = self.account_decim == expected_decim and self.account_raw == expected_raw
        if not ok:
            self.accounting_errors += 1
        return ok

    def summary(self):
        return {
            "fft": self.fft_size,
            "decim": self.decim,
            "hop": self.hop,
            "raw_total": self.raw_total,
            "decim_total": self.decim_total,
            "frames_total": self.frames_total,
            "published": self.published,
            "skipped": self.skipped,
            "sequence_errors": self.sequence_errors,
            "accounting_errors": self.accounting_errors,
            "fill": self.fill,
            "phase": self.phase,
        }


def run_clean_case(name, fft_size, decim, chunks, frames, hop=None):
    model = CicContinuityModel(fft_size, decim, hop)
    sample = 0
    decim_target = int(fft_size) if int(frames) > 0 else 0
    if int(frames) > 1:
        decim_target += (int(frames) - 1) * model.hop
    target = decim_target * int(decim)
    index = 0
    while sample < target:
        count = min(chunks[index % len(chunks)], target - sample)
        model.process_range(sample, count)
        sample += count
        if not model.check_accounting():
            break
        index += 1
    row = model.summary()
    row["name"] = name
    row["status"] = (
        "ok"
        if row["sequence_errors"] == 0
        and row["accounting_errors"] == 0
        and row["frames_total"] == frames
        else "error"
    )
    return row


def run_drop_duplicate_case(kind):
    model = CicContinuityModel(1024, 64)
    sample = 0
    for _ in range(5000):
        if kind == "drop" and sample == 2111:
            sample += 1
        model.process_sample(sample)
        if kind == "duplicate" and sample == 2111:
            model.process_sample(sample)
        sample += 1
    model.check_accounting()
    row = model.summary()
    row["name"] = kind
    row["status"] = "ok" if row["sequence_errors"] > 0 else "error"
    return row


def run_reset_case():
    model = CicContinuityModel(1024, 64)
    model.process_range(0, 1024 * 64 // 2)
    model.check_accounting()
    model.reset_cic(skip_frames=1)
    model.process_range(1024 * 64 // 2, 1024 * 64 * 3)
    model.check_accounting()
    row = model.summary()
    row["name"] = "reset_skip"
    row["status"] = (
        "ok"
        if row["sequence_errors"] == 0
        and row["accounting_errors"] == 0
        and row["skipped"] == 1
        and row["published"] >= 1
        else "error"
    )
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    rows = [
        run_clean_case("x2_line_sized", 65536, 2, [131072], 3),
        run_clean_case("x64_capped_chunks", 65536, 64, [262144], 2),
        run_clean_case("x64_prime_chunks", 4096, 64, [8191, 257, 16384], 4),
        run_clean_case("single_sample_chunks", 1024, 64, [1], 2),
        run_clean_case("overlap_accounting_guard", 4096, 64, [4096], 4, hop=511),
        run_drop_duplicate_case("drop"),
        run_drop_duplicate_case("duplicate"),
        run_reset_case(),
    ]
    ok = all(row["status"] == "ok" for row in rows)
    result = {"status": "ok" if ok else "error", "cases": rows}
    if not args.quiet or not ok:
        print(json.dumps(result, indent=2))
    if not ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
