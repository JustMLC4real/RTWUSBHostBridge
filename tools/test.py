#!/usr/bin/env python3
"""Run component tests without a Realtek binary, USB device or loaded kext."""
from pathlib import Path
import json
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/tests'
out.mkdir(parents=True, exist_ok=True)
results = []
for name, sources, fake_host in [
    ('transport', ['transport/HostTransport.cpp', 'transport/tests/TransportTests.cpp'], True),
    ('session', ['transport/HostSession.cpp', 'transport/HostTransport.cpp', 'transport/tests/SessionTests.cpp'], True),
    ('power', ['transport/tests/PowerEventsTests.cpp'], False),
]:
    binary = out / name
    command = ['clang++', '-std=c++14', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    if fake_host: command.append('-DRTW_TEST_HOST')
    subprocess.run([*command, *(str(root / p) for p in sources), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode: raise SystemExit(result.stdout + result.stderr)
    results.append(dict(name=name, passed=True))
    print(name + ': ' + result.stdout.strip())
subprocess.run(['python3', str(root / 'bridge/check_facade_close.py')], cwd=root, check=True)
results.append(dict(name='production-facade-bodies-asan-ubsan-tsan', passed=True))
(out / 'RESULT.json').write_text(json.dumps(dict(component_tests=results,
    kernel_module_loaded=False, full_native_framework_tested=False), indent=2) + '\n')
