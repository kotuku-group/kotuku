"""Independent SMTP/TLS and RFC MIME checks, driven by the SMTP Flute suite.

Uses Python 3's standard library and a public localhost test certificate/private key.
All sockets bind to loopback; no real email or remote OAuth service is contacted.
"""

import base64
from email import policy
from email.parser import BytesParser
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

FIXTURES = Path(__file__).resolve().parent
SDK = FIXTURES.parents[2]
ORIGO = Path(sys.argv[1]).resolve()


def run_flute(mode, *arguments):
    result = subprocess.run(
        [str(ORIGO), str(SDK / "tools/flute.tiri"), f"file={FIXTURES / 'test_smtp_external.tiri'}",
         f"mode={mode}", *arguments, "--log-api", "--gfx-driver=headless"],
        capture_output=True, timeout=15, cwd=SDK,
    )
    diagnostics = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
    assert result.returncode == 0, diagnostics
    assert "fixture-private-token" not in diagnostics, "Credential leaked in SMTP diagnostics"
    return diagnostics


def check_tls(mode):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(FIXTURES / "smtp_cert.pem", FIXTURES / "smtp_key.pem")
    record = {"ehlo": 0, "auth": False, "accepted": False}
    failures = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        port = listener.getsockname()[1]

        def serve():
            connection = None
            try:
                connection, _ = listener.accept()
                connection.settimeout(6)
                secure = mode == "implicit-reject"
                if secure:
                    connection = context.wrap_socket(connection, server_side=True)
                connection.sendall(b"220 independent.example ESMTP\r\n")
                data = bytearray()
                while True:
                    while b"\r\n" not in data:
                        chunk = connection.recv(8192)
                        if not chunk:
                            return
                        data.extend(chunk)
                    line, _, remainder = data.partition(b"\r\n")
                    data = bytearray(remainder)
                    if line.startswith(b"EHLO "):
                        record["ehlo"] += 1
                        connection.sendall(b"250-independent.example\r\n250-STARTTLS\r\n250 AUTH XOAUTH2\r\n")
                    elif line == b"STARTTLS":
                        assert not data, "Unexpected pipelined plaintext before TLS"
                        connection.sendall(b"220 Ready for TLS\r\n")
                        connection = context.wrap_socket(connection, server_side=True)
                        secure = True
                    elif line.startswith(b"AUTH XOAUTH2 "):
                        assert secure and record["ehlo"] == 2, "AUTH requires TLS and a fresh EHLO"
                        credentials = base64.b64decode(line[13:])
                        assert credentials == b"user=fixture@example.com\x01auth=Bearer fixture-private-token\x01\x01"
                        record["auth"] = True
                        if mode == "redact":
                            connection.sendall(b"535 5.7.8 rejected fixture-private-token " + line[13:] + b"\r\n")
                        else:
                            connection.sendall(b"235 2.7.0 Authenticated\r\n")
                    elif line.startswith((b"MAIL FROM:", b"RCPT TO:")):
                        connection.sendall(b"250 OK\r\n")
                    elif line == b"DATA":
                        connection.sendall(b"354 Send data\r\n")
                        while b"\r\n.\r\n" not in data:
                            chunk = connection.recv(8192)
                            if not chunk:
                                raise AssertionError("Disconnected before DATA completed")
                            data.extend(chunk)
                        _, _, remainder = data.partition(b"\r\n.\r\n")
                        data = bytearray(remainder)
                        record["accepted"] = True
                        connection.sendall(b"250 2.0.0 Accepted\r\n")
                    elif line == b"QUIT":
                        connection.sendall(b"221 Bye\r\n")
                        return
                    else:
                        raise AssertionError(f"Unexpected SMTP command: {line[:20]!r}")
            except (ssl.SSLError, ConnectionResetError, BrokenPipeError):
                if mode not in ("starttls-reject", "implicit-reject"):
                    failures.append("Unexpected TLS or transport failure")
            except Exception as error:
                failures.append(str(error))
            finally:
                if connection is not None:
                    connection.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            run_flute(mode, f"port={port}")
        finally:
            thread.join(8)
        assert not thread.is_alive(), "SMTP fixture did not terminate"
        assert not failures, failures
        if mode == "starttls":
            assert record == {"ehlo": 2, "auth": True, "accepted": True}, record
        elif mode == "redact":
            assert record["auth"] and not record["accepted"], record
        else:
            assert not record["auth"] and not record["accepted"], record


def check_mime():
    with tempfile.TemporaryDirectory(prefix="smtp-mime-") as folder:
        run_flute("mime", f"output={folder}")
        for index in range(4):
            wire = (Path(folder) / f"{index}.eml").read_bytes()
            assert b"hidden@example.com" not in wire and b"\n" not in wire.replace(b"\r\n", b"")
            assert all(len(line) <= 998 for line in wire.split(b"\r\n"))
            message = BytesParser(policy=policy.default).parsebytes(wire)
            assert not any(part.defects for part in message.walk())
            assert message["Message-ID"] == "<parser@example.com>"
            assert message["In-Reply-To"] == "<parent@example.com>"
            assert str(message["References"]).split() == ["<root@example.com>", "<parent@example.com>"]
            assert message.get_all("X-Tag") == ["one", "two"]
            content = message
            if index & 2:
                assert content.get_content_type() == "multipart/mixed"
                content, regular = list(content.iter_parts())
                assert regular.get_content_disposition() == "attachment"
                assert regular.get_filename() == "report.txt" and regular.get_payload(decode=True) == b"ordinary"
            if index & 1:
                assert content.get_content_type() == "multipart/alternative"
                plain, content = list(content.iter_parts())
                assert plain.get_content_type() == "text/plain"
                assert plain.get_payload(decode=True).rstrip(b"\r\n") == b"Text alternative"
            assert content.get_content_type() == "multipart/related"
            html, inline = list(content.iter_parts())
            assert html.get_content_type() == "text/html" and b"cid:logo@example.com" in html.get_payload(decode=True)
            assert inline.get_content_disposition() == "inline" and inline["Content-ID"] == "<logo@example.com>"
            assert inline.get_filename() == "ā.png" and inline.get_payload(decode=True) == b"\x00\x01\xffbinary"


for scenario in ("starttls", "starttls-reject", "implicit-reject", "redact"):
    check_tls(scenario)
check_mime()
print("Independent STARTTLS, certificate rejection, credential redaction and MIME parser checks passed.")
