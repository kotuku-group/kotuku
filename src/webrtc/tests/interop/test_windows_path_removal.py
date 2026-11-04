#!/usr/bin/env python3
"""Exercise same-host IPv6 recovery through two temporary Windows virtual tunnel interfaces.

Requires elevation. Only fixture-owned interfaces are removed; physical adapters are never changed.
The tunnel remote endpoints are documentation addresses. All test traffic targets local IPv6 addresses.
"""
import argparse
import json
from pathlib import Path
import queue
import secrets
import socket
import subprocess
import tempfile
import threading

from test_windows_notifications import powershell


def netsh(*arguments):
    result = subprocess.run(['netsh', 'interface', 'ipv6', *arguments],
                            capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    token = secrets.token_hex(3)
    devices = {f'fd42:203:{token[:4]}::1': f'Kotuku-{token}-a',
               f'fd42:204:{token[:4]}::1': f'Kotuku-{token}-b'}
    local = powershell("Get-NetIPAddress -AddressFamily IPv4 | Where-Object { "
                       "$_.AddressState -eq 'Preferred' -and $_.IPAddress -notlike '127.*' -and "
                       "$_.IPAddress -notlike '169.254.*' } | Select-Object -First 1 -ExpandProperty IPAddress").strip()
    socket.inet_pton(socket.AF_INET, local)
    created = []
    process = None
    output = []
    try:
        for index, (address, device) in enumerate(devices.items()):
            netsh('add', 'v6v4tunnel', device, local, f'192.0.2.{index + 1}')
            created.append(device)
            netsh('set', 'interface', device, 'metric=1', 'dadtransmits=0')
            netsh('add', 'address', device, address, 'store=active')
        with tempfile.TemporaryDirectory(prefix='rtc-topology-') as directory:
            config = Path(directory) / 'empty.json'
            config.write_text('[]')
            process = subprocess.Popen([str(args.probe.resolve()), str(config), 'topology', '--log-warning'],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            lines = queue.Queue()
            def read_output():
                for line in process.stdout:
                    lines.put(line)
                lines.put(None)
            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            initial = None
            routes = []
            while True:
                line = lines.get(timeout=40)
                if line is None:
                    break
                output.append(line)
                if line.startswith('TOPOLOGY_READY '):
                    initial = json.loads(line.split(' ', 1)[1])['selectedRoute']
                    address = initial['localEndpoint']['address']
                    if address not in devices or initial['remoteEndpoint']['address'] not in devices:
                        raise RuntimeError('Selected path is not a fixture-owned IPv6 interface')
                    netsh('delete', 'interface', devices[address])
                    created.remove(devices[address])
                elif line.startswith('Route evidence: '):
                    route = json.loads(line.split(': ', 1)[1])['selectedRoute']
                    for endpoint in ('localEndpoint', 'remoteEndpoint'):
                        address = route[endpoint]['address']
                        socket.inet_pton(socket.AF_INET6, address)
                        if address not in devices or devices[address] not in created:
                            raise RuntimeError('Recovery did not select the remaining fixture IPv6 path')
                    routes.append(route)
            result = process.wait(timeout=5)
            reader.join(timeout=2)
            if result or not initial or len(routes) != 2:
                raise RuntimeError(f'Virtual IPv6 recovery probe failed: {result}')
            args.evidence.mkdir(parents=True, exist_ok=True)
            (args.evidence / 'topology.json').write_text(json.dumps({
                'scope': 'same-host Windows virtual IPv6 tunnel interfaces; physical adapters unchanged',
                'initial': initial, 'recovered': routes}, indent=2))
    finally:
        if process:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            process.stdout.close()
        cleanup_errors = []
        for device in reversed(created):
            try:
                netsh('delete', 'interface', device)
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                cleanup_errors.append(f'{device}: {error}')
        print(''.join(output), end='')
        args.evidence.mkdir(parents=True, exist_ok=True)
        (args.evidence / 'public.log').write_text(''.join(output))
        if cleanup_errors:
            raise RuntimeError('Virtual interface cleanup failed: ' + '; '.join(cleanup_errors))
    print('PASS virtual IPv6 selected-path removal and recovery; temporary interfaces removed')


if __name__ == '__main__':
    main()
