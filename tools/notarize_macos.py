#!/usr/bin/env python3
r"""Submit once, then resume notarization using a preserved signed ZIP and receipt.

Local use (credentials saved with `xcrun notarytool store-credentials`):
  python3 tools/notarize_macos.py submit --state-dir dist/notary --profile evnova \
      --archive dist/evnova-macos-arm64.zip
  python3 tools/notarize_macos.py finish --state-dir dist/notary --profile evnova \
      --output dist/evnova-macos-arm64.zip

Pending submissions exit successfully with ready=false in GITHUB_OUTPUT when
set. Only an Accepted submission can produce the final, stapled output ZIP.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run(*args, capture=False):
    return subprocess.run(
        [str(arg) for arg in args], check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
    ).stdout


def digest(path):
    with path.open('rb') as file:
        checksum = hashlib.sha256()
        for chunk in iter(lambda: file.read(1024 * 1024), b''):
            checksum.update(chunk)
        return checksum.hexdigest()


def write_receipt(directory, receipt):
    temporary = directory / 'submission.json.tmp'
    temporary.write_text(json.dumps(receipt, indent=2) + '\n')
    temporary.replace(directory / 'submission.json')


def load_receipt(directory, source_run, source_sha):
    receipt = json.loads((directory / 'submission.json').read_text())
    if receipt['source_run'] != source_run or receipt['source_sha'] != source_sha:
        raise RuntimeError('Submission belongs to a different CI run or commit')
    if digest(directory / 'signed.zip') != receipt['sha256']:
        raise RuntimeError('Signed archive differs from the submitted archive')
    return receipt


def submit(directory, archive, auth, source_run='', source_sha=''):
    directory.mkdir(parents=True, exist_ok=True)
    if (directory / 'submission.json').exists():
        receipt = load_receipt(directory, source_run, source_sha)
        print(f"Reusing Apple submission {receipt['id']}", flush=True)
        return receipt
    saved = directory / 'signed.zip'
    if archive.resolve() != saved.resolve():
        shutil.copyfile(archive, saved)
    print('Uploading signed archive to Apple (without waiting for analysis)...', flush=True)
    result = json.loads(run(
        'xcrun', 'notarytool', 'submit', saved, *auth,
        '--no-wait', '--output-format', 'json', capture=True,
    ))
    receipt = {
        'id': result['id'], 'sha256': digest(saved),
        'source_run': source_run, 'source_sha': source_sha,
    }
    write_receipt(directory, receipt)
    print(f"Apple submission ID: {receipt['id']}", flush=True)
    return receipt


def finish(directory, output, auth, wait_seconds=300, source_run='', source_sha=''):
    receipt = load_receipt(directory, source_run, source_sha)
    submission_id = receipt['id']
    deadline = time.monotonic() + wait_seconds
    while True:
        result = json.loads(run(
            'xcrun', 'notarytool', 'info', submission_id, *auth,
            '--output-format', 'json', capture=True,
        ))
        status = result['status']
        print(f'Apple submission {submission_id}: {status}', flush=True)
        if status == 'Accepted':
            break
        if status != 'In Progress':
            run('xcrun', 'notarytool', 'log', submission_id, *auth)
            raise RuntimeError(f'Notarization failed: {status}')
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            print('Still pending at Apple. Signed ZIP and submission ID are preserved; '
                  'run finish again later. No resubmission is needed.', flush=True)
            return False
        time.sleep(min(30, remaining))

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='evnova-staple-', dir=output.parent) as temporary:
        stage = Path(temporary)
        run('ditto', '-x', '-k', directory / 'signed.zip', stage)
        app = stage / 'EV Nova.app'
        run('codesign', '--verify', '--deep', '--strict', '--verbose=2', app)
        run('xcrun', 'stapler', 'staple', app)
        run('xcrun', 'stapler', 'validate', app)
        run('codesign', '--verify', '--deep', '--strict', '--verbose=2', app)
        run('spctl', '--assess', '--type', 'execute', '--verbose=2', app)
        archive = stage / 'notarized.zip'
        run('ditto', '-c', '-k', '--keepParent', '--sequesterRsrc', app, archive)
        archive.replace(output)
    print(f'Created notarized package: {output}', flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['submit', 'finish'])
    parser.add_argument('--state-dir', required=True, type=Path)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--keychain', type=Path)
    parser.add_argument('--archive', type=Path, help='signed ZIP to submit')
    parser.add_argument('--output', type=Path, help='final stapled ZIP (finish only)')
    parser.add_argument('--wait-seconds', type=int, default=300, help='0 for a single status check')
    parser.add_argument('--source-run', default='', help='CI run ID bound to the receipt')
    parser.add_argument('--source-sha', default='', help='CI source commit bound to the receipt')
    args = parser.parse_args()
    if args.wait_seconds < 0:
        parser.error('--wait-seconds must be nonnegative')
    auth = ['--keychain-profile', args.profile]
    if args.keychain:
        auth.extend(['--keychain', args.keychain])
    if args.command == 'submit':
        if not args.archive:
            parser.error('submit requires --archive')
        submit(args.state_dir, args.archive, auth, args.source_run, args.source_sha)
    else:
        if not args.output:
            parser.error('finish requires --output')
        if args.output.resolve() == (args.state_dir / 'signed.zip').resolve():
            parser.error('output must not replace the preserved signed ZIP')
        ready = finish(args.state_dir, args.output, auth, args.wait_seconds, args.source_run, args.source_sha)
        if os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a') as file:
                file.write(f'ready={str(ready).lower()}\n')


if __name__ == '__main__':
    try:
        main()
    except (subprocess.CalledProcessError, RuntimeError, ValueError, OSError, KeyError) as error:
        sys.exit(str(error))
