#!/usr/bin/env python3
"""Ephemeral coturn fixture for transport, public relay and restart evidence."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time


def run(command, env=None, cwd=None, timeout=90):
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f'{Path(command[0]).name} exceeded the {timeout}-second deadline') from None
    if result.returncode:
        # uclient arguments contain an ephemeral credential; never print the command.
        output = result.stdout + result.stderr
        if '-w' in command:
            output = output.replace(command[command.index('-w') + 1], '[redacted]')
        raise RuntimeError(f"{Path(command[0]).name} failed ({result.returncode}):\n"
                           f"{output}")
    return result.stdout + result.stderr


def binding(sock, datagram=False):
    transaction = secrets.token_bytes(12)
    request = struct.pack('!HHI', 1, 0, 0x2112A442) + transaction
    sock.sendall(request)
    data = sock.recv(65536)
    if not datagram:
        while len(data) < 20:
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError('Premature stream EOF')
            data += chunk
        length = struct.unpack('!H', data[2:4])[0]
        while len(data) < 20 + length:
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError('Premature frame EOF')
            data += chunk
    if len(data) < 20 or data[:2] != b'\x01\x01' or data[8:20] != transaction:
        raise RuntimeError('Binding response identity/type mismatch')


def closing_stream(listener, context, errors):
    try:
        with listener:
            listener.settimeout(5)
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5)
                stream = context.wrap_socket(connection, server_side=True) if context else connection
                with stream:
                    data = b''
                    while len(data) < 20:
                        chunk = stream.recv(20 - len(data))
                        if not chunk:
                            raise RuntimeError('Probe closed before request completed')
                        data += chunk
                    stream.sendall(b'\x01\x01\0\0' + data[4:])
                    # Closing the transport must deliver the Network disconnect callback.
    except Exception as error:
        errors.append(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl', default='openssl', help='OpenSSL CLI used for ephemeral fixture certificates')
    parser.add_argument('--turnserver', required=True)
    parser.add_argument('--uclient', required=True)
    parser.add_argument('--origo', required=True)
    parser.add_argument('--port', type=int, default=34780)
    parser.add_argument('--tls-port', type=int, default=34781)
    parser.add_argument('--relay-ip', default=None, help='Local relay address; browser tests require a non-loopback IPv4')
    parser.add_argument('--relay-min', type=int, default=49160)
    parser.add_argument('--relay-max', type=int, default=49179)
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--browser-turn', type=Path, help='Chromium executable for browser relay interop')
    parser.add_argument('--windows-trust-ca', action='store_true',
                        help='Temporarily trust the fixture CA in the selected Windows root store')
    parser.add_argument('--windows-trust-scope', choices=['user', 'machine'], default='user',
                        help='Machine scope requires elevation and supports unattended root import')
    parser.add_argument('--transport', choices=['udp', 'tcp', 'tls'],
                        help='Run one TURN client transport while iterating on a fixture')
    parser.add_argument('--turn-adapter', type=Path, help='Production TLS queue and maintenance probe')
    parser.add_argument('--adapter-only', action='store_true', help='Run only the TLS adapter probe')
    parser.add_argument('--public-turn', type=Path, help='Run the public C++ relay matrix')
    parser.add_argument('--route-transitions-only', action='store_true', help='Run only the route-loss transitions')
    args = parser.parse_args()
    if args.windows_trust_ca and os.name != 'nt':
        parser.error('--windows-trust-ca is available only on Windows')
    if args.adapter_only and not args.turn_adapter:
        parser.error('--adapter-only requires --turn-adapter')
    if args.route_transitions_only and not (args.public_turn and os.environ.get('WEBRTC_PEER_TRANSITION')
                                           and os.environ.get('WEBRTC_DIRECT_LOSS')):
        parser.error('Focused transitions require a public TURN target and both transition fixture paths')
    root = Path(__file__).resolve().parents[4]
    origo = str(Path(args.origo).resolve())
    args.turnserver = str(Path(args.turnserver).resolve())
    args.uclient = str(Path(args.uclient).resolve())
    if args.evidence_dir:
        args.evidence_dir = args.evidence_dir.resolve()
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    os.chdir(root)
    # coturn's config lookup treats a Windows drive path as relative.  Keep the fixture on the
    # repository drive and pass paths relative to the working directory on that platform.
    temporary_root = root / 'build' if os.name == 'nt' else None
    with tempfile.TemporaryDirectory(prefix='kotuku-interop-', dir=temporary_root) as directory, ExitStack() as cleanup:
        work = Path(directory)
        def server_path(value):
            return os.path.relpath(value, root).replace('\\', '/') if os.name == 'nt' else str(value)
        # Fresh keys/credentials, confined to this fixture and removed on every exit.
        openssl_config = work / 'openssl.cnf'
        openssl_config.write_text('[req]\ndistinguished_name=dn\n[dn]\n')
        run([args.openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
             '-config', str(openssl_config),
             '-subj', '/CN=Kotuku test CA', '-keyout', str(work / 'ca.key'),
             '-out', str(work / 'ca.pem'), '-addext', 'basicConstraints=critical,CA:TRUE',
             '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
             '-addext', 'subjectKeyIdentifier=hash'])
        run([args.openssl, 'req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
             '-config', str(openssl_config),
             '-keyout', str(work / 'server.key'), '-out', str(work / 'server.csr')])
        (work / 'extensions').write_text('subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n'
                                        'basicConstraints=critical,CA:FALSE\n'
                                        'keyUsage=digitalSignature,keyEncipherment\n'
                                        'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid,issuer\n')
        run([args.openssl, 'x509', '-req', '-in', str(work / 'server.csr'), '-CA', str(work / 'ca.pem'),
             '-CAkey', str(work / 'ca.key'), '-CAcreateserial', '-days', '2',
             '-extfile', str(work / 'extensions'), '-out', str(work / 'server.pem')])
        if args.evidence_dir:
            shutil.copyfile(work / 'ca.pem', args.evidence_dir / 'ca.pem')
            shutil.copyfile(work / 'server.pem', args.evidence_dir / 'server.pem')
        if args.windows_trust_ca:
            pem = (work / 'ca.pem').read_text()
            thumbprint = hashlib.sha1(ssl.PEM_cert_to_DER_cert(pem)).hexdigest().upper()
            ca_trust = {'active': False}
            store_args = ['-user'] if args.windows_trust_scope == 'user' else []
            def trust_ca():
                if not ca_trust['active']:
                    # Track ownership before import so partial failure still removes this exact fresh certificate.
                    ca_trust['active'] = True
                    run(['certutil', *store_args, '-addstore', 'Root', str(work / 'ca.pem')])
            def untrust_ca():
                if ca_trust['active']:
                    run(['certutil', *store_args, '-delstore', 'Root', thumbprint])
                    ca_trust['active'] = False
            cleanup.callback(untrust_ca)
            trust_ca()
        username, password = 'interop', secrets.token_hex(24)
        relay_ip = args.relay_ip or '127.0.0.1'
        if args.browser_turn and not args.relay_ip:
            # Route lookup only: connect() on a UDP socket sends no packets to the documentation-only address.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
                route.connect(('192.0.2.1', 9))
                relay_ip = route.getsockname()[0]
            if relay_ip.startswith('127.'):
                raise RuntimeError('Browser TURN fixture needs a non-loopback local IPv4 relay address')
        config = work / 'turnserver.conf'
        config.write_text(f'''listening-ip=127.0.0.1
relay-ip={relay_ip}
listening-port={args.port}
tls-listening-port={args.tls_port}
min-port={args.relay_min}
max-port={args.relay_max}
realm=kotuku-interop.invalid
lt-cred-mech
user={username}:{password}
cert={server_path(work / 'server.pem')}
pkey={server_path(work / 'server.key')}
no-cli
no-dtls
no-tcp-relay
allow-loopback-peers
no-multicast-peers
relay-threads=1
simple-log
log-file={server_path(work / 'turn.log')}
pidfile={server_path(work / 'turn.pid')}
''')
        config.chmod(0o600)
        with (work / 'stdout.log').open('w') as output:
            server = subprocess.Popen([args.turnserver, '-c', server_path(config)], stdout=output,
                                      stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 5
                while True:
                    if server.poll() is not None:
                        raise RuntimeError('coturn exited before readiness')
                    try:
                        with socket.create_connection(('127.0.0.1', args.port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise RuntimeError('coturn readiness timeout')
                        time.sleep(0.05)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.settimeout(3)
                    sock.connect(('127.0.0.1', args.port))
                    binding(sock, datagram=True)
                print('PASS UDP Binding listener')
                with socket.create_connection(('127.0.0.1', args.port), timeout=3) as sock:
                    binding(sock)
                print('PASS TCP Binding listener')
                context = ssl.create_default_context(cafile=str(work / 'ca.pem'))
                with socket.create_connection(('127.0.0.1', args.tls_port), timeout=3) as raw:
                    with context.wrap_socket(raw, server_hostname='localhost') as sock:
                        binding(sock)
                print('PASS TLS Binding listener with trusted CA and hostname')
                for name, flags, port in [('UDP', [], args.port), ('TCP', ['-t'], args.port),
                                           ('TLS', ['-t', '-S', '-E', server_path(work / 'ca.pem')], args.tls_port)]:
                    if args.transport and name.lower() != args.transport:
                        continue
                    result = run([args.uclient, '-y', '-c', '-s', '-z', '100', '-n', '5',
                                  '-u', username, '-w', password,
                                  '-p', str(port), *flags, '127.0.0.1'])
                    if 'tot_send_msgs=10, tot_recv_msgs=10' not in result or 'Total lost packets 0' not in result:
                        raise RuntimeError(f'{name} relay delivery was not proved:\n'
                                           f'{result.replace(password, "[redacted]")}')
                    if args.evidence_dir:
                        (args.evidence_dir / f'relay_{name.lower()}.log').write_text(
                            result.replace(password, '[redacted]'))
                    print(f'PASS {name} authenticated relay delivery, 10/10 packets, UDP relay range '
                          f'{args.relay_min}-{args.relay_max}')
                env = os.environ.copy()
                env['SSL_CERT_FILE'] = str(work / 'ca.pem')
                # Empty hashed CA directory prevents fallback to the system trust store in negative tests.
                (work / 'empty').mkdir()
                env['SSL_CERT_DIR'] = str(work / 'empty')
                if args.turn_adapter:
                    result = run([str(args.turn_adapter.resolve()), '--turn-tls-stall',
                                  str((work / 'server.pem').resolve()), str((work / 'server.key').resolve()),
                                  '--log-warning'], env, Path(origo).parent)
                    print(result, end='')
                    if args.evidence_dir:
                        (args.evidence_dir / 'tls_adapter.log').write_text(result)
                if args.adapter_only:
                    return
                if args.public_turn or args.browser_turn:
                    for name, port, suffix in [('udp', args.port, '?transport=udp'),
                                               ('tcp', args.port, '?transport=tcp'),
                                               ('tls', args.tls_port, '?transport=tcp')]:
                        if args.transport and name != args.transport:
                            continue
                        config_path = work / 'ice_servers.json'
                        scheme = 'turns' if name == 'tls' else 'turn'
                        config_path.write_text(json.dumps([{'url': f'{scheme}:localhost:{port}{suffix}',
                                                           'username': username, 'credential': password}]))
                        config_path.chmod(0o600)
                        if args.public_turn and not args.route_transitions_only:
                            result = run([str(args.public_turn.resolve()), str(config_path), '--log-warning'], env, Path(origo).parent)
                            if args.evidence_dir:
                                (args.evidence_dir / f'public_turn_{name}.log').write_text(
                                    result.replace(password, '[redacted]'))
                            print(result, end='')
                            print(f'PASS production public TURN/{name} matrix')
                        transition = os.environ.get('WEBRTC_PEER_TRANSITION')
                        loss_fixture = os.environ.get('WEBRTC_DIRECT_LOSS')
                        if transition and loss_fixture:
                            loss_env = dict(env, LD_PRELOAD=loss_fixture)
                            result = run([transition, str(config_path), '--log-warning'], loss_env, Path(origo).parent)
                            if args.evidence_dir:
                                (args.evidence_dir / f'route_transition_{name}.log').write_text(
                                    result.replace(password, '[redacted]'))
                            print(result, end='')

                        if args.browser_turn and not args.route_transitions_only:
                            report = (args.evidence_dir or work) / f'browser_turn_{name}.json'
                            browser_result = run(['node', str(root / 'src/webrtc/tests/interop/public_turn.cjs'),
                                                  origo, str(root / 'src/webrtc/tests/interop/turn_signalling.tiri'),
                                                  str(args.browser_turn.resolve()), str(config_path), str(report)], env)
                            print(browser_result, end='')

                if args.public_turn and not args.route_transitions_only:
                    config_path = work / 'negative_ice_servers.json'
                    cases = [
                        ('wrong_credentials', f'turn:localhost:{args.port}?transport=tcp', password + '-wrong', env),
                        ('wrong_identity', f'turns:127.0.0.1:{args.tls_port}', password, env),
                        ('untrusted', f'turns:localhost:{args.tls_port}', password, dict(env,
                            SSL_CERT_FILE=str(work / 'server.pem'))),
                    ]
                    for name, url, secret, probe_env in cases:
                        config_path.write_text(json.dumps([{'url': url, 'username': username, 'credential': secret}]))
                        config_path.chmod(0o600)
                        if name == 'untrusted' and args.windows_trust_ca:
                            untrust_ca()
                        try:
                            result = run([str(args.public_turn.resolve()), str(config_path), 'security', '--log-warning'],
                                         probe_env, Path(origo).parent)
                        finally:
                            if name == 'untrusted' and args.windows_trust_ca:
                                trust_ca()
                        if args.evidence_dir:
                            (args.evidence_dir / f'public_{name}.log').write_text(result.replace(secret, '[redacted]'))
                        print(f'PASS public TURN {name} security rejection')
                    for url in [f'turn:localhost:{args.port}', f'turn:localhost:{args.port}?transport=tcp',
                                f'turns:localhost:{args.tls_port}']:
                        config_path.write_text(json.dumps([{'url': url, 'username': username, 'credential': password}]))
                        print(run([str(args.public_turn.resolve()), str(config_path), 'cancel', '--log-warning'],
                                  env, Path(origo).parent), end='')
                    # A failed server must not invalidate another usable relay route.
                    config_path.write_text(json.dumps([
                        {'url': f'turns:127.0.0.1:{args.tls_port}', 'username': username, 'credential': password},
                        {'url': f'turn:localhost:{args.port}?transport=tcp', 'username': username, 'credential': password}]))
                    result = run([str(args.public_turn.resolve()), str(config_path), '--log-warning'],
                                 env, Path(origo).parent)
                    print('PASS independent server failure with usable relay')
                    if args.evidence_dir:
                        (args.evidence_dir / 'public_failover.log').write_text(result.replace(password, '[redacted]'))
                for name, port, secure, identity, reject, trusted in [
                    ('TCP', args.port, False, 'localhost', False, True),
                    ('TLS', args.tls_port, True, 'localhost', False, True),
                    ('TLS wrong host', args.tls_port, True, 'wrong.invalid', True, True),
                    ('TLS untrusted', args.tls_port, True, 'localhost', True, False),
                ]:
                    probe_env = env.copy()
                    if not trusted:
                        # A different, valid CA file is required: a missing file may trigger a fallback.
                        shutil.copyfile(work / 'server.pem', work / 'untrusted.pem')
                        probe_env['SSL_CERT_FILE'] = str(work / 'untrusted.pem')
                    if not trusted and args.windows_trust_ca:
                        untrust_ca()
                    try:
                        result = run([origo, 'tools/flute.tiri',
                                      'file=src/webrtc/tests/interop/test_network_stream.tiri',
                                      f'port={port}', f'secure={str(secure).lower()}',
                                      f'identity={identity}', f'reject={str(reject).lower()}',
                                      '--log-warning', '--gfx-driver=headless'], probe_env)
                    finally:
                        if not trusted and args.windows_trust_ca:
                            trust_ca()
                    if 'FAIL' in result:
                        raise RuntimeError(f'{name} Network probe failed:\n{result}')
                    if args.evidence_dir:
                        (args.evidence_dir / f'network_{name.lower().replace(" ", "_")}.log').write_text(result)
                    print(f'PASS Network {name} probe')
                for secure in [False, True]:
                    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    listener.bind(('127.0.0.1', 0))
                    listener.listen(1)
                    port = listener.getsockname()[1]
                    context = None
                    if secure:
                        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                        context.load_cert_chain(work / 'server.pem', work / 'server.key')
                    errors = []
                    worker = threading.Thread(target=closing_stream, args=(listener, context, errors), daemon=True)
                    worker.start()
                    try:
                        result = run([origo, 'tools/flute.tiri',
                             'file=src/webrtc/tests/interop/test_network_stream.tiri',
                             f'port={port}', f'secure={str(secure).lower()}', 'shutdown=true',
                             '--log-warning', '--gfx-driver=headless'], env)
                    finally:
                        worker.join(timeout=6)
                    if worker.is_alive() or errors:
                        raise RuntimeError(f'Shutdown fixture failed: {errors}')
                    if args.evidence_dir:
                        name = 'tls' if secure else 'tcp'
                        (args.evidence_dir / f'network_{name}_shutdown.log').write_text(result)
                    print(f'PASS Network {"TLS" if secure else "TCP"} remote shutdown callback')
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
                if args.evidence_dir:
                    output.flush()
                    for path in work.glob('*.log'):
                        sanitised = path.read_text(errors='replace').replace(password, '[redacted]')
                        (args.evidence_dir / path.name).write_text(sanitised)


if __name__ == '__main__':
    main()
