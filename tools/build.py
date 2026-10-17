#!/usr/bin/env python3
"""Build local experimental bundles using Xcode and a user-supplied driver.

Never install, load, modify EFI, change security settings or rebuild caches.
"""
from pathlib import Path
import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
import tempfile
from patch_driver import make_candidate

ROOT = Path(__file__).resolve().parent.parent

def run(command, log):
    result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace') or result.stdout.decode(errors='replace'))
    return result.stdout.decode(errors='replace')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', required=True, type=Path, help='Separately obtained original RtWlanU.kext')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    parser.add_argument('--enable-alpha', action='store_true', help='Enable ASUS matching in the local experimental build')
    args = parser.parse_args()
    output = args.output.absolute()
    if output.exists():
        parser.error('Output exists; choose a new directory')
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    headers = sdk / 'System/Library/Frameworks/Kernel.framework/Headers'
    libkmod = sdk / 'usr/lib/libkmod.a'
    if not libkmod.is_file() or not (headers / 'IOKit/usb/IOUSBHostInterface.h').is_file():
        parser.error('The selected Xcode SDK must provide Intel kernel USB headers and libkmod.a')
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.rtw-build-', dir=output.parent))
    try:
        logs = stage / 'logs'; logs.mkdir()
        candidate = make_candidate(args.driver, stage / 'RtWlanU_HostCandidate.kext')
        run(['python3', ROOT / 'bridge/generate_abi.py'], logs / 'generate.log')
        common = ['-arch', 'x86_64', '-mkernel', '-fapple-kext', '-fno-exceptions', '-fno-rtti',
                  '-fno-stack-protector', '-DKERNEL', '-DKERNEL_PRIVATE', '-Wno-deprecated-declarations',
                  '-mmacosx-version-min=26.0', '-isystem', headers]
        objects = []
        for name, relative in [('HostTransport', 'transport/HostTransport.cpp'),
                               ('HostSession', 'transport/HostSession.cpp'), ('LegacyABI', 'bridge/LegacyABI.cpp')]:
            obj = stage / (name + '.o'); objects.append(obj)
            command = ['clang++', *common, '-std=c++14']
            if name == 'LegacyABI': command += ['-Xclang', '-fdump-vtable-layouts']
            text = run([*command, '-c', ROOT / relative, '-o', obj], logs / (name + '.log'))
            if name == 'LegacyABI': (ROOT / 'bridge/compiler_abi.txt').write_text(text)
        run(['python3', ROOT / 'bridge/validate_abi.py'], logs / 'abi.log')
        module = stage / 'Module.c'
        module.write_text('#include <mach/mach_types.h>\n'
            'extern kern_return_t _start(kmod_info_t*,void*);\nextern kern_return_t _stop(kmod_info_t*,void*);\n'
            'KMOD_EXPLICIT_DECL(nl.easytosh.RTWUSBHostBridge,"0.1.0",_start,_stop)\n'
            '__private_extern__ kmod_start_func_t* _realmain=0;\n'
            '__private_extern__ kmod_stop_func_t* _antimain=0;\n'
            '__private_extern__ int _kext_apple_cc=__APPLE_CC__;\n')
        module_obj = stage / 'Module.o'; objects.append(module_obj)
        run(['clang', *common, '-std=gnu11', '-c', module, '-o', module_obj], logs / 'module.log')
        bridge = stage / 'RTWUSBHostBridge.kext'; binary = bridge / 'Contents/MacOS/RTWUSBHostBridge'
        binary.parent.mkdir(parents=True)
        run(['clang++', '-arch', 'x86_64', '-nostdlib', '-mmacosx-version-min=26.0', '-Xlinker', '-kext',
             '-Xlinker', '-undefined', '-Xlinker', 'dynamic_lookup', '-Xlinker', '-no_source_version',
             *objects, libkmod, '-o', binary], logs / 'link.log')
        info = dict(CFBundleIdentifier='nl.easytosh.RTWUSBHostBridge', CFBundleName='RTWUSBHostBridge',
            CFBundleExecutable='RTWUSBHostBridge', CFBundlePackageType='KEXT', CFBundleVersion='0.1.0',
            CFBundleShortVersionString='0.1.0', OSBundleCompatibleVersion='0.1.0', OSBundleRequired='Root',
            OSBundleLibraries={'com.apple.iokit.IOUSBHostFamily':'1.2','com.apple.kpi.iokit':'8.0.0',
                'com.apple.kpi.libkern':'8.0.0','com.apple.kpi.mach':'8.0.0','com.apple.kpi.bsd':'8.0.0'},
            IOKitPersonalities={'ASUS USB-AC68 development bridge':{
                'CFBundleIdentifier':'nl.easytosh.RTWUSBHostBridge','IOClass':'RTUSBInterface',
                'IOProviderClass':'IOUSBHostInterface','idVendor':0x0b05,'idProduct':0x1817,
                'bConfigurationValue':1,'bInterfaceNumber':0,'RTWDevelopmentEnable':args.enable_alpha}})
        (bridge / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', bridge], logs / 'bridge-sign.log')
        run(['codesign', '--verify', '--strict', bridge], logs / 'bridge-verify.log')
        imports = run(['nm', '-u', binary], logs / 'bridge-imports.log')
        if 'IOUSBInterface' in imports or 'IOUSBFamily' in imports:
            raise RuntimeError('Unexpected legacy USB import in bridge')
        # Build intermediates do not form part of the usable local output.
        for obj in objects: obj.unlink()
        module.unlink()
        result = dict(version='0.1.0-alpha.1', architecture='x86_64',
            alpha_matching_enabled=args.enable_alpha,
            compiler_abi_slots=json.loads((ROOT / 'bridge/ABI_VALIDATED.json').read_text())['implemented_slot_count'],
            bridge_executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            candidate=candidate, installed=False, kernel_loaded=False,
            production_signing=False, rebuilt_binary_hardware_qualified=False,
            note='Component tests and ABI checks do not establish kernel lifetime safety or hardware qualification.')
        (stage / 'BUILD_RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        stage.rename(output)
        print(json.dumps(result, indent=2))
    finally:
        if stage.exists(): shutil.rmtree(stage)

if __name__ == '__main__':
    main()
