#!/usr/bin/env python3
"""Run with python3 tests/threshold_temp_layer/run.py; requires a host C compiler.

Compiles the real processor against a simulated clock, work queue and ZMK events.
This checks event ordering and timeout behavior; it does not replace a Zephyr build
or a hardware test. All generated headers and binaries stay in a temporary directory.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SOURCE = Path(os.environ.get(
    "THRESHOLD_TEMP_LAYER_SOURCE", ROOT / "src/input_processor_threshold_temp_layer.c"
)).resolve()


def main():
    with tempfile.TemporaryDirectory(prefix="torabo-aml-tests-") as temporary:
        directory = Path(temporary)
        # The production source's API dependencies are supplied by stubs.h.
        for include in re.findall(r"^#include <([^>]+)>", SOURCE.read_text(), re.MULTILINE):
            header = directory / include
            header.parent.mkdir(parents=True, exist_ok=True)
            header.touch()
        binary = directory / "test"
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-const-variable",
            "-I", str(directory), f'-DPROCESSOR_SOURCE="{SOURCE}"', str(HERE / "test.c"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary), *sys.argv[1:]], check=True)

    keymap = (ROOT / "config/keymap.keymap").read_text()
    movement = re.search(r"&pointing_listener\s*\{(.*?)scroll\s*\{", keymap, re.DOTALL).group(1)
    buttons = re.search(r"&mkp_input_listener\s*\{(.*?)\};", keymap, re.DOTALL).group(1)
    processor = r"<&zip_threshold_temp_layer\s+([^>]+)>"
    assert re.search(processor, movement).group(1).split() == re.search(processor, buttons).group(1).split()
    assert "&zip_temp_layer" not in movement + buttons
    print("PASS pointing and mouse-button listeners share the processor and timeout")


if __name__ == "__main__":
    main()
