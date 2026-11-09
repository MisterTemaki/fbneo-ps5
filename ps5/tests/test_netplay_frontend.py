#!/usr/bin/env python3
"""Drive two full host frontends over real TCP using the project's homebrew Z80 test ROM."""
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[1]
binary = (root / "build/host/fbneo-ps5-host").resolve()
headless = (root / "build/host/fbneo_headless").resolve()
work = Path(tempfile.mkdtemp(prefix="fbneo-netplay-frontend-"))
environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
print(f"Validation output: {work}", flush=True)
subprocess.run([
    sys.executable, str(root / "tests/make_fake_set.py"), str(headless), "ponpoko", str(work / "roms"),
    "--put", f"ppokoj1.bin={root / 'tests/data/pacman-test.6e'}",
    "--put", f"82s123.7f={root / 'tests/data/pacman-test.7f'}",
    "--put", f"82s126.4a={root / 'tests/data/pacman-test.4a'}",
    "--put", "ppoko9.bin=", "--put", "ppoko10.bin=",
], cwd=root, env=environment, check=True, stdout=subprocess.DEVNULL)
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]


def pad_script(guest):
    events = {}

    def tap(frame, bits):
        events[frame] = bits
        events[frame + 2] = 0

    tap(20, 0x6)  # L3+R3: pause
    for index in range(8):
        tap(30 + index * 6, 0x40)
    tap(80, 0x4000)  # open Netplay
    count = 4 if guest else 3
    for index in range(count):
        tap(92 + index * 6, 0x40)
    tap(120, 0x4000)  # Host/Join
    tap(140, 0x2000)  # back from Netplay
    tap(155, 0x2000)  # resume
    tap(200, 0x100000)  # coin
    events[230] = 0x4000 if guest else 0x20
    events[280] = 0
    if not guest:
        tap(320, 0x6)
        tap(330, 0x40)  # Save state is disabled while online
        tap(342, 0x4000)
        tap(390, 0x2000)  # pause the remote peer, then resume
        events[500] = 0x20  # change P1 input after both peers are advancing; correct guest prediction
        events[540] = 0
        tap(660, 0x6)
        for index in range(8):
            tap(670 + index * 6, 0x40)
        tap(720, 0x4000)
        for index in range(5):
            tap(732 + index * 6, 0x40)
        tap(770, 0x4000)  # Disconnect
        tap(790, 0x2000)
        tap(810, 0x2000)
    tap(900, 0x6)
    tap(910, 0x10)  # Up wraps to Quit
    tap(920, 0x4000)
    return ";".join(f"{frame}:{bits:x}" for frame, bits in sorted(events.items()))


processes = []
rolled_back = False
try:
    for role in ("host", "guest"):
        folder = work / role
        folder.mkdir()
        shutil.copytree(work / "roms", folder / "roms")
        (folder / "fbneo-ps5.ini").write_text(
            f"shader=0\ndebug_logs=1\nnetplay_address=127.0.0.1\nnetplay_port={port}\n", encoding="utf-8")
        env = {
            **environment, "FBNEO_PS5_ROOT": str(folder),
            "FBNEO_PS5_HOMEBREW": str(folder / "homebrew"), "FBNEO_HOST_OFFLINE": "1",
            "FBNEO_HELPER_PORT": str(port + (1 if role == "host" else 2)),
            "FBNEO_ELFLDR_PORT": str(port + (3 if role == "host" else 4)),
            "FBNEO_JB_NO_OTHERS": "1", "FBNEO_LOGS_DEFAULT": "1",
            "FBNEO_HOST_MAX_FLIPS": "1100", "FBNEO_HOST_REALTIME": "1",
            "FBNEO_HOST_PAD": pad_script(role == "guest"),
        }
        output = (folder / "output.log").open("w")
        processes.append((role, subprocess.Popen([str(binary), str(folder / "roms/ponpoko.zip")],
                         cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT), output))
        if role == "host":
            # A fixed launch delay races with native linking/CPU contention: wait for the actual listener.
            deadline = time.monotonic() + 30
            boot = folder / "logs/boot.log"
            while not boot.exists() or "[netplay] hosting on TCP" not in boot.read_text(errors="replace"):
                assert processes[-1][1].poll() is None, "host exited before opening its listener"
                assert time.monotonic() < deadline, "host did not open its listener"
                time.sleep(0.05)
    for role, process, output in processes:
        code = process.wait(timeout=90)
        output.close()
        text = (work / role / "output.log").read_text(errors="replace")
        logs = "\n".join(p.read_text(errors="replace") for p in (work / role / "logs").glob("*.log"))
        print(f"{role}: exit {code}", flush=True)
        for line in logs.splitlines():
            if "[netplay]" in line:
                print(line, flush=True)
        assert code == 0, text[-3000:]
        assert "AddressSanitizer" not in text and "runtime error:" not in text, text[-3000:]
        assert "state synchronized" in logs, logs[-4000:]
        assert "[netplay] frame 120" in logs, logs[-4000:]
        assert "offline state restored" in logs, logs[-4000:]
        rolled_back |= any(int(n) > 0 for n in re.findall(r"(\d+) rollback\(s\)", logs))
        assert "desynchronized" not in logs and "restore failed" not in logs, logs[-4000:]
        assert not list((work / role / "states").glob("*.state*")), "online state-save action was not blocked"
    assert rolled_back, "neither real core executed rollback; the delayed-input scenario was not exercised"
    print("PASS two complete frontends: menu Host/Join, real core rollback, pause, disconnect and offline restore", flush=True)
finally:
    for _, process, output in processes:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        output.close()
