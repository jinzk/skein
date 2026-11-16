#!/usr/bin/env python3
"""Exercise --watch: restart a child when a .skein file changes."""

import os
import signal
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1])


def write_text(path, text):
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    os.replace(temporary, path)


def script(version):
    return (
        "import std;\n"
        f'std.fs.write_text("run.txt", "{version}", "utf-8");\n'
        "while (true) {\n"
        "  std.time.sleep(50);\n"
        "}\n"
    )


def wait_for(path, expected, timeout=15):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            with open(path, encoding="utf-8") as handle:
                last = handle.read().strip()
            if last == expected:
                return
        except FileNotFoundError:
            last = None
        time.sleep(0.05)
    raise AssertionError(
        f"{path}: expected {expected!r}, last value was {last!r}")


def check_usage_errors():
    proc = subprocess.run(
        [BIN, "--watch"],
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=10,
    )
    if proc.returncode != 2 or "--watch requires a script file" not in \
            proc.stderr:
        raise AssertionError(
            f"missing-script check failed: {proc.returncode}\n"
            f"{proc.stdout}\n{proc.stderr}")

    with tempfile.TemporaryDirectory(prefix="skein-watch-bad-") as tmp:
        script_path = os.path.join(tmp, "app.skein")
        write_text(script_path, "print(1);\n")
        proc = subprocess.run(
            [BIN, "--watch", "--watch-dir", os.path.join(tmp, "missing"),
             script_path],
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=10,
        )
        if proc.returncode != 2 or "not a directory" not in proc.stderr:
            raise AssertionError(
                f"bad-directory check failed: {proc.returncode}\n"
                f"{proc.stdout}\n{proc.stderr}")


def check_restart():
    with tempfile.TemporaryDirectory(prefix="skein-watch-") as tmp:
        script_path = os.path.join(tmp, "app.skein")
        output_path = os.path.join(tmp, "run.txt")
        write_text(script_path, script("v1"))

        proc = subprocess.Popen(
            [BIN, "--watch", "app.skein"],
            cwd=tmp,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=(
                subprocess.CREATE_NEW_PROCESS_GROUP
                if os.name == "nt" else 0
            ),
        )
        stdout = ""
        stderr = ""
        try:
            wait_for(output_path, "v1")
            write_text(script_path, script("v2"))
            wait_for(output_path, "v2")
        finally:
            if proc.poll() is None:
                if os.name == "nt":
                    proc.send_signal(
                        getattr(signal, "CTRL_BREAK_EVENT", signal.SIGTERM))
                else:
                    proc.send_signal(signal.SIGINT)
            try:
                stdout, stderr = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()

        if proc.returncode not in (0, 130):
            raise AssertionError(
                f"watcher exited with {proc.returncode}\n"
                f"{stdout}\n{stderr}")
        if "Traceback" in stderr:
            raise AssertionError(stderr)


def check_sigterm_stops_watcher():
    with tempfile.TemporaryDirectory(prefix="skein-watch-term-") as tmp:
        script_path = os.path.join(tmp, "app.skein")
        output_path = os.path.join(tmp, "run.txt")
        write_text(script_path, script("v1"))

        proc = subprocess.Popen(
            [BIN, "--watch", "app.skein"],
            cwd=tmp,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            wait_for(output_path, "v1")
            proc.send_signal(signal.SIGTERM)
            stdout, stderr = proc.communicate(timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill()
                stdout, stderr = proc.communicate()

        if proc.returncode != 0:
            raise AssertionError(
                f"watcher did not stop cleanly on SIGTERM: "
                f"{proc.returncode}\n{stdout}\n{stderr}")
        if "Traceback" in stderr:
            raise AssertionError(stderr)


def main():
    check_usage_errors()
    check_restart()
    if not sys.platform.startswith("win"):
        check_sigterm_stops_watcher()
    print("watch mode tests passed")


if __name__ == "__main__":
    main()
