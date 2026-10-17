#!/usr/bin/env python3
"""Create a local candidate from a separately supplied, exact Realtek binary.

No downloads, installation, EFI changes, catalogue changes or module loading.
"""
from pathlib import Path
import argparse
import hashlib
import json
import plistlib
import shutil
import struct
import subprocess
import tempfile

ORIGINAL_SHA256 = '7e8e7a4d6ced1b85b3854c896011feb6227a430c49fd738322f76b1b0d819ba3'
OLD_SYMBOL = b'__ZN14IOUSBInterface9metaClassE\0'
NEW_SYMBOL = b'__ZN14RTUSBInterface9metaClassE\0'

def executable_text(data):
    if data[:4] != bytes.fromhex('cffaedfe'):
        raise ValueError('Expected the supported thin x86_64 Mach-O executable')
    cursor = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or cursor + length > len(data):
            raise ValueError('Invalid Mach-O load command')
        if command == 0x19:
            count = struct.unpack_from('<I', data, cursor + 64)[0]
            for index in range(count):
                pos = cursor + 72 + index * 80
                name, segment, _, size, offset = struct.unpack_from('<16s16sQQI', data, pos)
                if name.rstrip(b'\0') == b'__text' and segment.rstrip(b'\0') == b'__TEXT':
                    if offset + size > len(data):
                        raise ValueError('Invalid executable text bounds')
                    return data[offset:offset + size]
        cursor += length
    raise ValueError('Executable text section missing')

def make_candidate(source, destination):
    source, destination = Path(source).resolve(), Path(destination).absolute()
    if destination.exists():
        raise ValueError('Output already exists; choose a new output path')
    if source == destination or source in destination.parents:
        raise ValueError('Output must be outside the original driver bundle')
    binary = source / 'Contents/MacOS/RtWlanU'
    original = binary.read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Unsupported Realtek executable: exact 1830.32.b27 SHA-256 is required')
    if original.count(OLD_SYMBOL) != 1 or len(OLD_SYMBOL) != len(NEW_SYMBOL):
        raise ValueError('Unexpected USB metaclass symbol layout')
    info = plistlib.loads((source / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != 'com.realtek.driver.RtWlanU' or info.get('CFBundleExecutable') != 'RtWlanU':
        raise ValueError('Unexpected original bundle identity')
    libraries = info.get('OSBundleLibraries', {})
    if 'com.apple.iokit.IOUSBFamily' not in libraries:
        raise ValueError('Expected an unmodified legacy USB bundle dependency')
    personalities = {}
    for name, personality in info.get('IOKitPersonalities', {}).items():
        if personality.get('idVendor') == 0x0b05 and personality.get('idProduct') == 0x1817:
            copied = dict(personality)
            copied.update(IOProviderClass='RTUSBInterface', bConfigurationValue=1, bInterfaceNumber=0,
                          CFBundleIdentifier='com.realtek.driver.RtWlanU')
            personalities[name] = copied
    if len(personalities) != 1:
        raise ValueError('Expected exactly one ASUS USB-AC68 personality')
    for p in source.rglob('*'):
        if p.is_symlink():
            raise ValueError('Use a plain extracted driver bundle without symlinks')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.rtw-patch-', dir=destination.parent))
    staged = temporary / destination.name
    try:
        shutil.copytree(source, staged, copy_function=shutil.copyfile)
        (staged / 'Contents/MacOS/RtWlanU').write_bytes(original.replace(OLD_SYMBOL, NEW_SYMBOL))
        info['CFBundleDisplayName'] = 'RtWlanU Host Candidate (experimental)'
        info['IOKitPersonalities'] = personalities
        libraries.pop('com.apple.iokit.IOUSBFamily')
        libraries['nl.easytosh.RTWUSBHostBridge'] = '0.1.0'
        info['OSBundleLibraries'] = libraries
        (staged / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        subprocess.run(['codesign', '--force', '--sign', '-', '--timestamp=none', str(staged)], check=True)
        subprocess.run(['codesign', '--verify', '--strict', str(staged)], check=True)
        patched = (staged / 'Contents/MacOS/RtWlanU').read_bytes()
        if executable_text(original) != executable_text(patched):
            raise ValueError('Executable instructions changed unexpectedly')
        imports = subprocess.check_output(['nm', '-u', str(staged / 'Contents/MacOS/RtWlanU')], text=True)
        if '__ZN14RTUSBInterface9metaClassE' not in imports or '__ZN14IOUSBInterface9metaClassE' in imports:
            raise ValueError('Unexpected patched imports')
        staged.rename(destination)
        return dict(original_executable_sha256=ORIGINAL_SHA256,
                    candidate_executable_sha256=hashlib.sha256(patched).hexdigest(),
                    original_executable_instructions_preserved=True,
                    hardware_id='0b05:1817', installed=False, kernel_loaded=False)
    finally:
        shutil.rmtree(temporary)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(make_candidate(args.original, args.output), indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')

if __name__ == '__main__':
    main()
