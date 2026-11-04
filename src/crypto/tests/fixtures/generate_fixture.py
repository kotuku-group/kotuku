"""Regenerate the public-only RS256 test fixture with Python cryptography.

Run from the repository root with:
    python3 src/crypto/tests/fixtures/generate_fixture.py

The private key exists in this process only.  It is never written to disk.
"""

import base64
import json
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa


def integer(value):
    encoded = value.to_bytes((value.bit_length() + 7) // 8, "big")
    return base64.urlsafe_b64encode(encoded).rstrip(b"=").decode("ascii")


key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
public = key.public_key().public_numbers()
message = b"crypto module independent RS256 fixture\x00\x80"
rs256 = key.sign(message, padding.PKCS1v15(), hashes.SHA256())
pss = key.sign(message, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
fixture = {
    "provenance": "Generated offline with Python cryptography RSA PKCS1v15/PSS SHA256; public-only fixture",
    "kty": "RSA",
    "alg": "RS256",
    "n": integer(public.n),
    "e": integer(public.e),
    "message_hex": message.hex(),
    "signature_hex": rs256.hex(),
    "pss_signature_hex": pss.hex(),
}
Path(__file__).with_name("rs256.json").write_text(json.dumps(fixture, indent=2) + "\n")
