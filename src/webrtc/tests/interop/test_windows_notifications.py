#!/usr/bin/env python3
"""Real Windows address notifications using one temporary /32 on the loopback interface.

Requires elevation. Never disables adapters or changes DNS/default routes. The address is
removed in finally; an existing address causes failure before any mutation.
"""
import argparse
from pathlib import Path
import queue
import subprocess
import threading

ADDRESS = '192.0.2.254'


def powershell(script):
    result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                             "$ErrorActionPreference='Stop'; " + script],
                            capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def exercise(command, marker, native=False):
    lines = queue.Queue()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1)
    def read_output():
        for line in process.stdout:
            lines.put(line)
        lines.put(None)
    worker = threading.Thread(target=read_output, daemon=True)
    worker.start()
    added = False
    output = []
    def wait_line(expected):
        while True:
            line = lines.get(timeout=45)
            if line is None:
                raise RuntimeError(f'Probe ended before {expected}: {"".join(output)}')
            output.append(line)
            if expected in line:
                return
    try:
        wait_line(marker)
        # Record ownership before invoking the mutating command so cleanup also runs on partial failure.
        added = True
        powershell(f"New-NetIPAddress -InterfaceIndex 1 -IPAddress {ADDRESS} -PrefixLength 32 "
                   "-SkipAsSource $true -PolicyStore ActiveStore | Out-Null")
        if native:
            wait_line('CHANGED')
            powershell(f"Remove-NetIPAddress -InterfaceIndex 1 -IPAddress {ADDRESS} -Confirm:$false")
            added = False
        result = process.wait(timeout=45)
        worker.join(timeout=2)
        while not lines.empty():
            line = lines.get_nowait()
            if line:
                output.append(line)
        print(''.join(output), end='')
        if result or any('FAIL' in line for line in output):
            raise RuntimeError(f'Probe failed with exit {result}')
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        if added:
            powershell(f"Get-NetIPAddress -InterfaceIndex 1 -IPAddress {ADDRESS} "
                       "-ErrorAction SilentlyContinue | Remove-NetIPAddress -Confirm:$false")
        process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--monitor', type=Path, required=True)
    parser.add_argument('--origo', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    powershell(f"if (Get-NetIPAddress -IPAddress {ADDRESS} -ErrorAction SilentlyContinue) "
               "{ throw 'Fixture address already exists' }; "
               "if ((Get-NetIPInterface -InterfaceIndex 1 -AddressFamily IPv4).Dhcp -ne 'Disabled') "
               "{ throw 'Loopback DHCP must already be disabled' }")
    exercise([str(args.monitor.resolve()), '--watch', ADDRESS], 'READY', native=True)
    for case in ['DeferredRecover', 'DeferredFree', 'DeferredFail']:
        exercise([str(args.origo.resolve()), str(root / 'tools/flute.tiri'),
                  f'file={root / "src/webrtc/tests/test_peer_network_change.tiri"}',
                  f'test={case}', 'windowsTopology=true', '--log-warning', '--gfx-driver=headless'],
                 'WINDOWS_TOPOLOGY_READY')
        print(f'PASS real Windows address notification: {case}')
    powershell(f"if (Get-NetIPAddress -IPAddress {ADDRESS} -ErrorAction SilentlyContinue) "
               "{ throw 'Fixture address was not removed' }")
    print('PASS real Windows address addition/removal and public recovery/free/failure; address cleaned up')


if __name__ == '__main__':
    main()
