#!/usr/bin/env python3
"""Create a private runtime overlay of one exact supported prebuilt Brave ELF."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import sys


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--native-install-shim', type=Path)
    args = parser.parse_args()
    source = args.package.resolve(strict=True)
    output = args.output.resolve()
    if output == source or source in output.parents:
        raise ValueError('Output must be outside the original package')
    pin = json.loads(args.manifest.read_text())
    if pin['schema'] != 1 or pin['architecture'] != 'x86_64-linux':
        raise ValueError('Unsupported binary manifest')
    relative = Path(pin['executable'])
    if relative.is_absolute() or '..' in relative.parts:
        raise ValueError('Executable must be within the package')
    binary = source / relative
    if digest(binary) != pin['input_sha256']:
        raise ValueError('Unsupported Brave binary: hash changed; re-identify and verify hooks')
    with binary.open('rb') as stream:
        if stream.read(6) != b'\x7fELF\x02\x01':
            raise ValueError('Expected a little-endian ELF64 executable')
        stream.seek(18)
        if stream.read(2) != b'\x3e\x00':
            raise ValueError('Expected an x86-64 executable')
        for patch in pin['patches']:
            expected = bytes.fromhex(patch['expected_prefix'])
            replacement = bytes.fromhex(patch['replacement'])
            if not expected or not replacement or len(replacement) > len(expected):
                raise ValueError('Invalid patch span')
            stream.seek(patch['file_offset'])
            if stream.read(len(expected)) != expected:
                raise ValueError('Native hook instructions changed: ' + patch['function'])
    # Never modify or overwrite a source package or an existing overlay.
    output.mkdir(parents=False, exist_ok=False)
    try:
        # Resources remain read-only links to the original package. Create real
        # directories so copying our ELF and launch scripts cannot write through
        # directory symlinks into the original Nix output.
        for directory, dirs, files in os.walk(source, followlinks=False):
            dest = output / Path(directory).relative_to(source)
            dest.mkdir(exist_ok=True)
            for name in dirs[:]:
                entry = Path(directory) / name
                if entry.is_symlink():
                    (dest / name).symlink_to(entry.resolve())
                    dirs.remove(name)
            for name in files:
                (dest / name).symlink_to((Path(directory) / name).resolve())
        target = output / relative
        target.unlink()
        shutil.copyfile(binary, target)
        with target.open('r+b') as stream:
            for patch in pin['patches']:
                stream.seek(patch['file_offset'])
                stream.write(bytes.fromhex(patch['replacement']))
        if digest(target) != pin['output_sha256']:
            raise ValueError('Patched output hash does not match the verified pin')
        target.chmod(0o755)
        # Both launchers must be real copies: brave-browser resolves its own
        # location to find the ELF/resources. A symlink would run stock Brave.
        for relative in ['bin/brave', 'opt/brave.com/brave/brave-browser']:
            target = output / relative
            target.unlink()
            script = (source / relative).read_text().replace(str(source), str(output))
            if args.native_install_shim and relative.endswith('brave-browser'):
                invocation = '\"$HERE/brave\" \"$@\" || true'
                if script.count(invocation) != 1:
                    raise ValueError('Brave ELF launcher changed; review preload boundary')
                # Only the actual browser ELF loads the shim. Preloading shell
                # utilities would mix their older glibc with the browser's SDK.
                script = script.replace(invocation,
                    'if [ "${GNOSIS_NATIVE_UI_FD-}" = 5 ]; then\n  LD_PRELOAD=' +
                    shlex.quote(str(args.native_install_shim.resolve(strict=True))) + ' exec \"$HERE/brave\" \"$@\"' +
                    '\nelse\n  ' + invocation + '\nfi')
            target.write_text(script)
            target.chmod(0o755)
        wrapped = output / 'bin/.brave-wrapped'
        wrapped.unlink()
        wrapped.symlink_to('../opt/brave.com/brave/brave-browser')
        print('Applied native UI hooks to private Brave overlay:', output)
    except BaseException:
        shutil.rmtree(output)
        raise


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print('Brave binary patch refused:', error, file=sys.stderr)
        sys.exit(1)
