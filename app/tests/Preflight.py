#!/usr/bin/env python3
"""Fast auth, payment, display and localization release checks, without GTK/SDK."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]


def run(label, command):
    print(f'Linux preflight: {label}', flush=True)
    subprocess.run(command, check=True, timeout=90)


for control in (None, 'epoch', 'async-logout'):
    command = [sys.executable, '-B', str(root / 'tests/AuthSessionRegression.py')]
    if control:
        command += ['--negative-control', control]
    run('auth/session ' + (control or 'regression'), command)

with tempfile.TemporaryDirectory(prefix='urnetwork-linux-preflight-') as temp:
    executable = Path(temp) / 'ui-wiring'
    run('compile payment, display and localization checks', [
        *shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17',
        '-I', str(root / 'src'), f'-DUR_SRC_DIR="{root / "src"}"',
        str(root / 'tests/main.cpp'), str(root / 'tests/CryptoPayWiringTest.cpp'),
        str(root / 'tests/DisplayTextTest.cpp'), str(root / 'tests/CatalogLookupTest.cpp'),
        str(root / 'tests/ExtenderSharePresentationTest.cpp'),
        '-o', str(executable),
    ])
    run('payment, display and localization checks', [str(executable)])
