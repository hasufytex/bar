"""Run against an isolated headless Sway; never touch the user's compositor."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def run():
    with tempfile.TemporaryDirectory(prefix="bar-wayland-test-") as tmp:
        root = Path(tmp)
        config = root / "sway.conf"
        config.write_text("output * mode 1280x720\n")
        env = os.environ.copy()
        env.update(
            XDG_RUNTIME_DIR=tmp,
            WLR_BACKENDS="headless",
            WLR_RENDERER="pixman",
            WLR_LIBINPUT_NO_DEVICES="1",
        )
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("SWAYSOCK", None)
        with (root / "sway.log").open("w") as log:
            sway = subprocess.Popen(
                ["sway", "-c", str(config)], env=env, stdout=log, stderr=log
            )
        bar = None
        try:
            for _ in range(100):
                sockets = list(root.glob("sway-ipc.*.sock"))
                displays = [p for p in root.glob("wayland-*") if p.is_socket()]
                if sockets and displays:
                    break
                if sway.poll() is not None:
                    raise RuntimeError((root / "sway.log").read_text())
                time.sleep(0.05)
            else:
                raise RuntimeError("Headless Sway did not start")
            env.update(
                SWAYSOCK=str(sockets[0]),
                WAYLAND_DISPLAY=displays[0].name,
                WAYLAND_DEBUG="1",
            )
            with (root / "bar.log").open("w") as log:
                bar = subprocess.Popen(
                    [os.environ.get("BAR_TEST_BINARY", "./bar")],
                    env=env, stdout=log, stderr=log,
                )
            time.sleep(1.25)
            for width in [1920, 800, 2560, 1280]:
                reply = subprocess.run(
                    ["swaymsg", "-r", "output", "HEADLESS-1", "mode", f"{width}x720"],
                    env=env, capture_output=True, text=True, check=True,
                )
                assert '"success": true' in reply.stdout, reply.stdout
                time.sleep(0.25)
                assert bar.poll() is None, (root / "bar.log").read_text()
            time.sleep(1.25)
            assert bar.poll() is None, (root / "bar.log").read_text()
            trace = (root / "bar.log").read_text()
            assert trace.count(".create_buffer(") >= 4
            assert ".release()" in trace
            # Every reused buffer must have been released since its last attach.
            busy = set()
            attached = 0
            for line in trace.splitlines():
                attach = re.search(r"\.attach\(wl_buffer[#@](\d+)", line)
                release = re.search(r"wl_buffer[#@](\d+)\.release\(\)", line)
                if attach:
                    attached += 1
                    ident = attach.group(1)
                    assert ident not in busy, f"Reused busy buffer: {line}"
                    busy.add(ident)
                if release:
                    busy.discard(release.group(1))
            assert attached > 0, "No buffer attachments were parsed"
            # Sway may close its socket before sending the exit command reply.
            subprocess.run(["swaymsg", "exit"], env=env, capture_output=True)
            sway.wait(timeout=3)
            bar.wait(timeout=3)
            assert bar.returncode in (0, 1), (root / "bar.log").read_text()
            trace = (root / "bar.log").read_text()
            assert "AddressSanitizer" not in trace and "runtime error:" not in trace, trace
            print("Headless startup, buffer release/reuse, four resizes, and disconnect passed.")
        finally:
            stop(bar)
            stop(sway)


if __name__ == "__main__":
    run()
