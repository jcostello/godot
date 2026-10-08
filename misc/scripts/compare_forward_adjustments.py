#!/usr/bin/env python3
"""Compare Forward+ adjustment GPU timings without changing project resources or SSR."""

import argparse
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile


def summarize(log, warmup):
    text = log.read_text(errors="replace")
    marker = next((line for line in text.splitlines() if line.startswith("[Forward adjustments]")), None)
    if marker is None:
        print("  Missing adjustment diagnostics: verify the executable includes these changes.")
    else:
        print(f"  {marker}")
    for label, pattern in (
        ("GPU total (sampled frames)", r"GPU PROFILE \(total ([\d.eE+-]+)ms\)"),
        ("Tonemap GPU", r"-Tonemap: ([\d.eE+-]+)ms"),
    ):
        values = [float(value) for value in re.findall(pattern, text)][warmup:]
        if values:
            print(f"  {label}: median {statistics.median(values):.4f} ms; range {min(values):.4f}–{max(values):.4f}; {len(values)} samples")
        else:
            print(f"  {label}: insufficient samples after discarding {warmup}")


    fps = [float(value) for value in re.findall(r"Project FPS: ([\d.]+)", text)][warmup:]
    if fps:
        print(f"  Project FPS: median {statistics.median(fps):.1f}; range {min(fps):.1f}–{max(fps):.1f}; {len(fps)} samples")
    else:
        print("  Project FPS: not enough project FPS reports; GPU timings alone do not measure FPS")
    print("  GPU total uses individual frames; Tonemap uses window averages from the engine profiler.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[2] / "bin/godot.linuxbsd.editor.x86_64")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--modes", nargs="+", choices=("normal", "runtime_branches", "no_soft_contrast", "no_saturation", "no_transform", "off"), default=("normal", "off", "normal", "off", "normal"), help="Alternating repeats distinguish effect cost from run-to-run drift")
    parser.add_argument("--warmup", type=int, default=5, help="GPU reports to discard per run")
    parser.add_argument("--display-driver", default="x11")
    args = parser.parse_args()
    if args.seconds <= 0 or args.warmup < 0:
        parser.error("--seconds must be positive and --warmup nonnegative")
    binary = args.binary.resolve()
    project = args.project.resolve()
    if not binary.is_file() or not (project / "project.godot").is_file():
        parser.error("binary or project.godot not found")
    log_dir = Path(tempfile.mkdtemp(prefix="forward-adjustments-"))
    print(f"Logs: {log_dir}", flush=True)
    print("Keep the same camera, resolution and scene. SSR is unchanged.", flush=True)
    print("Compare repeated normal and off runs; wait for GPU/shader warmup before interpreting differences.", flush=True)
    modes = args.modes
    for index, mode in enumerate(modes, 1):
        env = os.environ.copy()
        env.pop("GODOT_FORK_ADJUSTMENTS_TEST", None)
        env["GODOT_FORK_ADJUSTMENTS_PROFILE"] = "1"
        if mode != "normal":
            env["GODOT_FORK_ADJUSTMENTS_TEST"] = mode
        command = [str(binary), "--path", str(project), "--gpu-profile", "--quit-after", "0", "--display-driver", args.display_driver]
        log = log_dir / f"{index:02d}-{mode}.log"
        print(f"\n{index:02d}-{mode}: {args.seconds:g} seconds", flush=True)
        with log.open("w") as output:
            process = subprocess.Popen(command, env=env, stdout=output, stderr=subprocess.STDOUT)
            timed_out = False
            try:
                process.wait(timeout=args.seconds)
            except subprocess.TimeoutExpired:
                timed_out = True
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            except BaseException:
                process.kill()
                process.wait()
                raise
        if not timed_out and process.returncode:
            print(f"  Game exited with error {process.returncode}; inspect {log}")
        summarize(log, args.warmup)
    print(f"\nLogs saved in {log_dir}")
    print("runtime_branches compares the previous shader branches; other modes isolate effect blocks.")
    print("Each block can interact with the others; do not add their timing differences together.")


if __name__ == "__main__":
    main()
