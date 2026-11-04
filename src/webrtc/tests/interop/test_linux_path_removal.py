#!/usr/bin/env python3
"""Public IPv6-only/multihomed transfer and signalled recovery after removing the selected Linux interface."""
import argparse
import json
from pathlib import Path
import queue
import socket
import subprocess
import sys
import tempfile
import threading


def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True, text=True, timeout=15).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--outer-net', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.inside:
        subprocess.run(['unshare', '-Urn', sys.executable, str(Path(__file__).resolve()), '--inside', *sys.argv[1:],
                        '--outer-net', str(Path('/proc/self/ns/net').readlink())], check=True, timeout=90)
        return
    if not args.outer_net or str(Path('/proc/self/ns/net').readlink()) == args.outer_net:
        raise RuntimeError('Refusing interface mutation in the outer namespace')
    run('ip', 'link', 'set', 'lo', 'up')
    devices = {'fd42:203::1': 'rtc_path_a', 'fd42:204::1': 'rtc_path_b'}
    for address, device in devices.items():
        run('ip', 'link', 'add', device, 'type', 'dummy')
        run('ip', '-6', 'addr', 'add', address + '/64', 'dev', device, 'nodad')
        run('ip', 'link', 'set', device, 'up')
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
        output = []
        initial = None
        routes = []
        try:
            while True:
                line = lines.get(timeout=30)
                if line is None:
                    break
                output.append(line)
                if line.startswith('TOPOLOGY_READY '):
                    initial = json.loads(line.split(' ', 1)[1])['selectedRoute']
                    address = initial['localEndpoint']['address']
                    if address not in devices:
                        raise RuntimeError('Selected path is not one of the private IPv6 interfaces')
                    # Removing the selected interface requires the existing connection to recover through the other.
                    run('ip', 'link', 'del', devices[address])
                elif line.startswith('Route evidence: '):
                    route = json.loads(line.split(': ', 1)[1])['selectedRoute']
                    for endpoint in ('localEndpoint', 'remoteEndpoint'):
                        socket.inet_pton(socket.AF_INET6, route[endpoint]['address'])
                    routes.append(route)
            result = process.wait(timeout=5)
            print(''.join(output), end='')
            if result or not initial or len(routes) != 2:
                raise RuntimeError(f'IPv6-only recovery probe failed: {result}')
            if routes[0]['localEndpoint']['address'] == initial['localEndpoint']['address']:
                raise RuntimeError('Selected path did not move away from the removed interface')
            args.evidence.mkdir(parents=True, exist_ok=True)
            (args.evidence / 'public.log').write_text(''.join(output))
            (args.evidence / 'topology.json').write_text(json.dumps({
                'scope': 'private Linux network namespace; two dummy IPv6 interfaces; same-host transfer',
                'initial': initial, 'recovered': routes, 'remaining': json.loads(run('ip', '-j', 'addr'))}, indent=2))
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            reader.join(timeout=2)
            process.stdout.close()
    print('PASS IPv6-only multihomed exact transfers, selected interface removal, public signalled recovery')


if __name__ == '__main__':
    main()
