#!/usr/bin/env python3
"""Real user-session launchd callbacks with simulated effects. No sudo or PF changes."""
import argparse
import datetime as dt
import os
import pathlib
import plistlib
import shutil
import subprocess
import tempfile
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('binary', type=pathlib.Path)
parser.add_argument('--launchd', action='store_true', help='Exercise real calendar deadlines (~4 minutes)')
parser.add_argument('--dispatch-only', action='store_true', help='Explicitly kickstart real jobs at deadlines; does not validate calendar delivery')
args = parser.parse_args()

def run(argv, success=True):
    result = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=20)
    if success and result.returncode:
        raise AssertionError(f'{argv}: {result.stderr}')
    if not success and result.returncode == 0:
        raise AssertionError(f'Expected failure: {argv}')
    return result

with tempfile.TemporaryDirectory(prefix='blocker-integration-') as directory:
    root = pathlib.Path(directory)
    # Keep launchd away from Desktop privacy controls; use a disposable executable copy.
    binary = root / 'blockerctl'
    shutil.copy2(args.binary.resolve(), binary)
    state = root / 'state with spaces & quotes'
    base = [binary, '--simulate', '--state', state]
    resource = 'test' + uuid.uuid4().hex
    run(base + ['website', resource, 'example.com'])
    run(base + ['set', 'focus', resource])
    for arguments in [['block', 'missing'], ['cancel', '999'], ['block', 'focus', '2030-02-30T12:00Z'],
                      ['website', '../escape', 'example.org'], ['website', 'url', 'https://example.org']]:
        before = (state / 'schedule.txt').read_bytes()
        run(base + arguments, success=False)
        assert (state / 'schedule.txt').read_bytes() == before, arguments
    print('PASS CLI rejects invalid requests without modifying schedule', flush=True)
    if not (args.launchd or args.dispatch_only):
        run(base + ['block', 'focus'])
        assert (state / f'{resource}.blocked').exists()
        run(base + ['unblock', 'focus'])
        assert not (state / f'{resource}.blocked').exists()
        print('PASS CLI immediate block/unblock', flush=True)
    else:
        start = (int(time.time()) // 60 + 1) * 60
        if start - time.time() < 10:
            start += 60
        end = start + (60 if args.dispatch_only else 180)
        stamp = lambda value: dt.datetime.fromtimestamp(value, dt.timezone.utc).strftime('%Y-%m-%dT%H:%MZ')
        run(base + ['block', 'focus', stamp(start)])
        run(base + ['unblock', 'focus', stamp(end)])
        domain = f'gui/{os.getuid()}'
        loaded = []
        try:
            for path in sorted((state / 'jobs').glob('*.plist')):
                payload = plistlib.loads(path.read_bytes())
                payload['StandardErrorPath'] = str(root / 'job-errors.txt')
                payload['StandardOutPath'] = str(root / 'job-output.txt')
                path.write_bytes(plistlib.dumps(payload))
                run(['/usr/bin/plutil', '-lint', path])
                run(['/bin/launchctl', 'bootstrap', domain, path])
                loaded.append(domain + '/' + payload['Label'])
            assert len(loaded) == 2
            # RunAtLoad must not execute the future block early.
            time.sleep(2)
            assert not (state / f'{resource}.blocked').exists()
            for label in loaded:
                info = run(['/bin/launchctl', 'print', label]).stdout
                print('Initial launchd status:', '\n'.join(line.strip() for line in info.splitlines() if 'exit code' in line or 'runs =' in line or 'state =' in line), flush=True)
            if (root / 'job-errors.txt').exists():
                print('Job stderr:', (root / 'job-errors.txt').read_text(), flush=True)
            print(f'PASS early RunAtLoad ignored; waiting for calendar start in {max(0, int(start-time.time()))}s', flush=True)
            if args.dispatch_only:
                while time.time() < start:
                    time.sleep(0.25)
                run(['/bin/launchctl', 'kickstart', next(label for label in loaded if label.endswith('.block'))])
            while time.time() < start + 90 and not (state / f'{resource}.blocked').exists():
                time.sleep(0.25)
            assert (state / f'{resource}.blocked').exists(), 'calendar block never ran'
            print('PASS real launchd ' + ('kickstart' if args.dispatch_only else 'calendar') + ' block after submitter exited', flush=True)
            # Deliberately deliver the block again; retry must preserve desired state.
            run(base + ['execute', f'{resource}.{start}.block'])
            if args.dispatch_only:
                while time.time() < end:
                    time.sleep(0.25)
                run(['/bin/launchctl', 'kickstart', next(label for label in loaded if label.endswith('.unblock'))])
            while time.time() < end + 90 and (state / f'{resource}.blocked').exists():
                time.sleep(0.25)
            assert not (state / f'{resource}.blocked').exists(), 'calendar unblock never ran'
            run(base + ['execute', f'{resource}.{start}.block'])
            assert not (state / f'{resource}.blocked').exists(), 'late block reblocked resource'
            print('PASS real launchd ' + ('kickstart' if args.dispatch_only else 'calendar') + ' unblock, duplicate and stale callback handling', flush=True)
        finally:
            for name in ['job-errors.txt', 'job-output.txt']:
                if (root / name).exists():
                    print(name + ': ' + (root / name).read_text(), flush=True)
            for label in loaded:
                info = subprocess.run(['/bin/launchctl', 'print', label], capture_output=True, text=True)
                print('Final launchd status:', '\n'.join(line.strip() for line in info.stdout.splitlines() if 'exit code' in line or 'runs =' in line or 'state =' in line), flush=True)
            for label in loaded:
                result = subprocess.run(['/bin/launchctl', 'bootout', label], capture_output=True, text=True)
                if result.returncode:
                    print(f'Cleanup needs attention for {label}: {result.stderr}', flush=True)
            for label in loaded:
                result = subprocess.run(['/bin/launchctl', 'print', label], capture_output=True)
                assert result.returncode != 0, f'Job left installed: {label}'
            print('PASS temporary launchd jobs removed', flush=True)
