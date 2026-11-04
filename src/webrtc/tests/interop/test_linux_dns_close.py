#!/usr/bin/env python3
"""Bound public DNS close/free against a delayed NSS query inside private user/network/mount namespaces."""
import argparse
import json
from pathlib import Path
import queue
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--outer-net', help=argparse.SUPPRESS)
    parser.add_argument('--outer-mnt', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.inside:
        subprocess.run(['unshare', '-Urnm', sys.executable, str(Path(__file__).resolve()), '--inside',
                        *sys.argv[1:], '--outer-net', str(Path('/proc/self/ns/net').readlink()),
                        '--outer-mnt', str(Path('/proc/self/ns/mnt').readlink())], check=True, timeout=45)
        return
    for kind in ('net', 'mnt'):
        if not getattr(args, 'outer_' + kind) or str(Path(f'/proc/self/ns/{kind}').readlink()) == getattr(args, 'outer_' + kind):
            raise RuntimeError('Refusing DNS changes outside private namespaces')
    subprocess.run(['mount', '--make-rprivate', '/'], check=True)
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
    with tempfile.TemporaryDirectory(prefix='rtc-dns-') as directory:
        resolver = Path(directory) / 'resolv.conf'
        resolver.write_text('nameserver 127.0.0.1\noptions attempts:1 timeout:5\n')
        subprocess.run(['mount', '--bind', str(resolver), '/etc/resolv.conf'], check=True)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as dns:
            dns.bind(('127.0.0.1', 53))
            dns.settimeout(10)
            for mode in ('dns-close', 'dns-free', 'dns-capacity'):
                host = f'kotuku-interop-{secrets.token_hex(8)}.invalid'
                config = Path(directory) / 'dns.json'
                config.write_text(json.dumps([{'url': f'turn:{host}:3478', 'username': 'test', 'credential': 'test'}]))
                process = subprocess.Popen([str(args.probe.resolve()), str(config), mode, '--log-warning'],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                lines = queue.Queue()
                def read_output():
                    for line in process.stdout:
                        lines.put(line)
                    lines.put(None)
                reader = threading.Thread(target=read_output, daemon=True)
                reader.start()
                output = []
                try:
                    request, sender = dns.recvfrom(4096)
                    labels = b''.join(bytes([len(label)]) + label.encode() for label in host.split('.')) + b'\0'
                    if request[12:12 + len(labels)] != labels:
                        raise RuntimeError('Unexpected DNS query')
                    while True:
                        line = lines.get(timeout=10)
                        if line is None:
                            raise RuntimeError('Probe exited before close')
                        output.append(line)
                        if 'DNS_CLOSE_READY' in line:
                            break
                    time.sleep(2)
                    # Deliver late replies while the probe pumps its destroyed object's message queue.
                    dns.settimeout(0.1)
                    deadline = time.monotonic() + 5
                    while process.poll() is None and time.monotonic() < deadline:
                        if request:
                            dns.sendto(request[:2] + struct.pack('!HHHHH', 0x8183, 1, 0, 0, 0)
                                       + request[12:12 + len(labels) + 4], sender)
                        try:
                            request, sender = dns.recvfrom(4096)
                        except socket.timeout:
                            request = None
                    result = process.wait(timeout=5)
                    reader.join(timeout=2)
                    while not lines.empty():
                        line = lines.get_nowait()
                        if line:
                            output.append(line)
                    print(''.join(output), end='')
                    if result or not any('PASS delayed DNS' in line for line in output):
                        raise RuntimeError(f'Delayed DNS {mode} failed: {result}')
                    dns.settimeout(10)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=5)
                    reader.join(timeout=2)
                    while not lines.empty():
                        line = lines.get_nowait()
                        if line:
                            output.append(line)
                    if process.returncode:
                        print(''.join(output), file=sys.stderr)
                    process.stdout.close()
    print('PASS bounded delayed NSS close/free in disposable namespaces')


if __name__ == '__main__':
    main()
