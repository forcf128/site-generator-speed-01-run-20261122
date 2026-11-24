#!/usr/bin/env python3
"""Benchmark: cold-process CPU (user+sys, via child rusage deltas) of running
an entry's main.ts on the practice input, median of N runs.
Usage: python3 tools/bench.py [runs=5] [entry...]
Entries: mine (this repo), baseline (kit house entry), v4 (apexsite-v4).
Prints median CPU in microseconds and the raw runs."""
import os
import resource
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT.parent
KIT = BASE / "kit" / "site-generator-speed-01-run-20261122-kit"
PRACTICE_IN = KIT / "practice" / "input"

ENTRIES = {
    "mine": ROOT,
    "baseline": KIT / "baselines" / "makefun-site",
    "v4": BASE / "apexsite-v4" / "v4-src" / "apexsite-v4",
}


def child_cpu():
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    return ru.ru_utime + ru.ru_stime


def one(entry_dir):
    with tempfile.TemporaryDirectory(prefix="zen-bench-") as out:
        env = dict(os.environ)
        env["MAKEFUN_INPUT_DIR"] = str(PRACTICE_IN)
        env["MAKEFUN_OUTPUT_DIR"] = out
        before = child_cpu()
        r = subprocess.run(["node", "main.ts"], cwd=str(entry_dir), env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        after = child_cpu()
        if r.returncode != 0:
            raise RuntimeError(f"run failed in {entry_dir}: {r.stderr.decode()[:500]}")
        return int(round((after - before) * 1e6))


def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    names = sys.argv[2:] or ["mine", "baseline"]
    for name in names:
        entry_dir = ENTRIES.get(name)
        if entry_dir is None:
            print(f"{name}: unknown entry")
            continue
        times = [one(entry_dir) for _ in range(runs)]
        print(f"{name}: median {int(statistics.median(times))} us  runs {times}")


if __name__ == "__main__":
    main()
