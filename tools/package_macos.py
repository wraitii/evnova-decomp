#!/usr/bin/env python3
"""Package an existing static macOS build; optionally sign and notarize it."""

import argparse
import json
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args, capture=False, check=True):
    return subprocess.run(
        [str(arg) for arg in args], check=check, text=True,
        stdout=subprocess.PIPE if capture else None,
    ).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/release/src/evnova')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/evnova-macos-arm64.zip')
    parser.add_argument('--bundle-id', default='io.github.wraitii.evnova-decomp')
    parser.add_argument('--version', default=None, help='defaults to the CMake project version')
    parser.add_argument('--build-number', default='1')
    parser.add_argument('--identity', default='-', help='Developer ID Application identity; default: ad hoc')
    parser.add_argument('--keychain', type=Path, help='keychain containing the signing identity/profile')
    parser.add_argument('--notary-profile', help='notarytool keychain profile; enables notarization')
    parser.add_argument('--icon', type=Path, help='optional .icns icon')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('packaging requires macOS and Xcode command-line tools')
    if args.notary_profile and args.identity == '-':
        parser.error('notarization requires a Developer ID Application --identity')
    if not args.binary.is_file():
        parser.error(f'build the executable first: {args.binary}')
    if args.output.suffix != '.zip':
        parser.error('--output must end in .zip')
    if args.icon and (not args.icon.is_file() or args.icon.suffix != '.icns'):
        parser.error('--icon must be an existing .icns file')
    version = args.version or re.search(
        r'\bVERSION\s+(\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text()
    ).group(1)
    for value in (version, args.build_number):
        if not re.fullmatch(r'\d+(?:\.\d+){0,2}', value):
            parser.error('version and build number must contain one to three numeric components')
    if not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', args.bundle_id):
        parser.error('invalid reverse-DNS bundle identifier')

    # The project uses static vcpkg libraries. Fail instead of shipping a bundle
    # that accidentally depends on a developer machine or a CI build directory.
    dependencies = run('otool', '-L', args.binary, capture=True)
    for line in dependencies.splitlines():
        if ' (compatibility version ' in line:
            dependency = line.strip().split(' (compatibility version ', 1)[0]
            if not dependency.startswith(('/usr/lib/', '/System/Library/')):
                parser.error(f'non-system dynamic dependency must be bundled first: {dependency}')
    load_commands = run('otool', '-l', args.binary, capture=True)
    minimum = re.search(r'\bminos\s+([\d.]+)', load_commands)
    if minimum is None:
        minimum = re.search(r'cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)', load_commands)
    if minimum is None:
        parser.error('cannot determine the executable minimum macOS version')

    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Stage beside the archive so replacement is atomic and failures cannot
    # leave a partially written distribution ZIP.
    with tempfile.TemporaryDirectory(prefix='evnova-package-', dir=args.output.parent) as temporary:
        stage = Path(temporary)
        app = stage / 'EV Nova.app'
        contents = app / 'Contents'
        (contents / 'MacOS').mkdir(parents=True)
        (contents / 'Resources').mkdir()
        executable = contents / 'MacOS/evnova'
        shutil.copyfile(args.binary, executable)
        executable.chmod(0o755)
        info = {
            'CFBundleDevelopmentRegion': 'en',
            'CFBundleExecutable': 'evnova',
            'CFBundleIdentifier': args.bundle_id,
            'CFBundleInfoDictionaryVersion': '6.0',
            'CFBundleName': 'EV Nova',
            'CFBundleDisplayName': 'EV Nova',
            'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': version,
            'CFBundleVersion': args.build_number,
            'LSMinimumSystemVersion': minimum.group(1),
            'LSApplicationCategoryType': 'public.app-category.games',
            'NSHighResolutionCapable': True,
        }
        if args.icon:
            shutil.copyfile(args.icon, contents / 'Resources/EVNova.icns')
            info['CFBundleIconFile'] = 'EVNova.icns'
        with (contents / 'Info.plist').open('wb') as file:
            plistlib.dump(info, file)
        run('plutil', '-lint', contents / 'Info.plist')
        signing = ['codesign', '--force', '--sign', args.identity, '--options', 'runtime']
        if args.identity != '-':
            signing.append('--timestamp')
        if args.keychain:
            signing.extend(['--keychain', args.keychain])
        run(*signing, app)
        run('codesign', '--verify', '--deep', '--strict', '--verbose=2', app)
        archive = stage / 'package.zip'
        run('ditto', '-c', '-k', '--keepParent', '--sequesterRsrc', app, archive)
        if args.notary_profile:
            auth = ['--keychain-profile', args.notary_profile]
            if args.keychain:
                auth.extend(['--keychain', args.keychain])
            result = json.loads(run(
                'xcrun', 'notarytool', 'submit', archive, *auth,
                '--wait', '--timeout', '30m', '--output-format', 'json', capture=True, check=False,
            ))
            print(json.dumps(result, indent=2))
            if result.get('status') != 'Accepted':
                if result.get('id'):
                    run('xcrun', 'notarytool', 'log', result['id'], *auth)
                raise RuntimeError('Apple did not accept the notarization submission')
            run('xcrun', 'stapler', 'staple', app)
            run('xcrun', 'stapler', 'validate', app)
            run('codesign', '--verify', '--deep', '--strict', '--verbose=2', app)
            run('spctl', '--assess', '--type', 'execute', '--verbose=2', app)
            archive.unlink()
            run('ditto', '-c', '-k', '--keepParent', '--sequesterRsrc', app, archive)
        archive.replace(args.output)
    print(f'Created {args.output}')


if __name__ == '__main__':
    try:
        main()
    except (subprocess.CalledProcessError, RuntimeError, json.JSONDecodeError) as error:
        sys.exit(str(error))
