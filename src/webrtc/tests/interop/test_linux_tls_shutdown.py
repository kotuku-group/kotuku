#!/usr/bin/env python3
"""Exercise fast TLS connection completion, response delivery and remote EOF through Flute."""

import argparse
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading

from run_turn_fixture import closing_stream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--origo', type=Path, required=True)
    parser.add_argument('--openssl', required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=100)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error('--repetitions must be positive')
    args.evidence.mkdir(parents=True, exist_ok=True)
    repository = Path(__file__).resolve().parents[4]
    executable = args.origo.resolve()
    with tempfile.TemporaryDirectory(prefix='kotuku-tls-shutdown-') as temporary:
        work = Path(temporary)
        certificate, key = work / 'certificate.pem', work / 'key.pem'
        subprocess.run([args.openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                        '-out', str(certificate), '-days', '1', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost'], check=True, capture_output=True, timeout=30)
        environment = os.environ.copy()
        environment['SSL_CERT_FILE'] = str(certificate)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        for iteration in range(args.repetitions):
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen(1)
                port = listener.getsockname()[1]
                errors = []
                worker = threading.Thread(target=closing_stream, args=(listener, context, errors), daemon=True)
                worker.start()
                try:
                    result = subprocess.run([str(executable), 'tools/flute.tiri',
                        'file=src/webrtc/tests/interop/test_network_stream.tiri', f'port={port}',
                        'secure=true', 'shutdown=true', '--log-warning', '--gfx-driver=headless'],
                        cwd=repository, env=environment, capture_output=True, text=True, timeout=15)
                finally:
                    worker.join(timeout=6)
                output = result.stdout + result.stderr
                (args.evidence / f'{iteration:03d}.log').write_text(output)
                if result.returncode or errors or worker.is_alive():
                    raise RuntimeError(f'TLS shutdown iteration {iteration + 1} failed: {errors}\n{output}')
        summary = {'passed': args.repetitions, 'transport': 'TLS', 'scope': 'loopback response and remote EOF'}
        (args.evidence / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(f'PASS {args.repetitions} TLS connections: split writes, exact response and remote shutdown')


if __name__ == '__main__':
    main()
