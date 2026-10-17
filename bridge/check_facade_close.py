"""Execute unchanged production close/didTerminate bodies with fake IOKit.

The fake IOService::close intentionally models the matching kernel's lack of
a post-handleClose isOpen check. Full framework behavior is not simulated.
"""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'DevelopmentOnly/FacadeFixture'
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'bridge/LegacyABI.cpp').read_text()
parts = [source[source.index('void RTUSBInterface::close('):source.index('bool RTUSBInterface::willTerminate(')],
         source[source.index('bool RTUSBInterface::didTerminate('):source.index('IOReturn RTUSBInterface::message(')]]
body = '// Extracted unchanged production definitions for the fake-framework integration test.\n' + '\n'.join(parts)
(OUT / 'ActualCloseMethods.inc').write_text(body)
results = {}
for sanitizer, name in [('address,undefined', 'asan_ubsan'), ('thread', 'tsan')]:
    binary = OUT / ('facade-' + name)
    command = ['clang++', '-std=c++14', '-DRTW_TEST_HOST', '-fsanitize=' + sanitizer,
               '-fno-omit-frame-pointer', '-g', str(ROOT / 'transport/HostSession.cpp'),
               str(ROOT / 'transport/HostTransport.cpp'), str(ROOT / 'transport/tests/FacadeCloseTests.cpp'),
               '-o', str(binary)]
    compile_result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (name + '.compile.log')).write_bytes(compile_result.stdout)
    assert compile_result.returncode == 0, compile_result.stdout.decode()
    run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    (OUT / (name + '.log')).write_bytes(run.stdout)
    assert run.returncode == 0, run.stdout.decode()
    results[name] = dict(passed=True, returncode=run.returncode)
    print(name + ': ' + run.stdout.decode().strip())
needle = '    unlockForArbitration();\n    const auto result=closingSession->closeAndReleaseClient'
assert body.count(needle) == 1
bad_body = body.replace(needle, '    const auto result=closingSession->closeAndReleaseClient')
bad_body = bad_body.replace('    closingSession->release();', '    unlockForArbitration();\n    closingSession->release();')
bad_path = OUT / 'NEGATIVE_arbitration_held_during_drain.inc'
bad_path.write_text(bad_body)
binary = OUT / 'negative-arbitration'
command = ['clang++', '-std=c++14', '-DRTW_TEST_HOST', '-fsanitize=address,undefined',
           '-DRTW_FACADE_METHODS="' + str(bad_path) + '"', '-fno-omit-frame-pointer', '-g',
           str(ROOT / 'transport/HostSession.cpp'), str(ROOT / 'transport/HostTransport.cpp'),
           str(ROOT / 'transport/tests/FacadeCloseTests.cpp'), '-o', str(binary)]
compile_result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
assert compile_result.returncode == 0, compile_result.stdout.decode()
run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
(OUT / 'negative_arbitration_expected_failure.log').write_bytes(run.stdout)
assert run.returncode != 0 and b'callback.wait_for' in run.stdout, run.stdout.decode()
print('Expected negative regression: holding arbitration across native drain blocks the callback reclose.')
report = dict(results=results, negative_held_arbitration_regression_reproduced=True,
              extracted_production_bodies_sha256=hashlib.sha256(body.encode()).hexdigest(),
              source_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in [
                  'bridge/LegacyABI.cpp', 'transport/HostSession.cpp', 'transport/HostSession.hpp',
                  'transport/tests/FakeHost.hpp', 'transport/tests/FacadeCloseTests.cpp']},
              full_native_framework_tested=False, hardware_qualified=False, installed=False)
(OUT / 'RESULT.json').write_text(json.dumps(report, indent=2) + '\n')
