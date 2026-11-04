"""Regenerate public, offline RS256 fixtures; the ephemeral private keys are never saved."""
import base64
import json
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives import hashes


def encoded(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


def compact(value):
    return json.dumps(value, separators=(',', ':')).encode()


key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
other = rsa.generate_private_key(public_exponent=65537, key_size=2048)
public = key.public_key().public_numbers()
jwk = dict(kid='fixture', kty='RSA', alg='RS256', use='sig', key_ops=['verify'],
           n=encoded(public.n.to_bytes(256, 'big')), e=encoded(public.e.to_bytes(3, 'big')))
claims = dict(iss='https://issuer.invalid', aud='offline-client', sub='subject',
              iat=1700000000, exp=4102444800, nonce='fixture-nonce')


def token(body=None, header=None, signer=key, raw=None):
    message = encoded(compact(header or dict(alg='RS256', kid='fixture'))) + '.'
    message += encoded(raw if raw is not None else compact(body or claims))
    signature = signer.sign(message.encode(), padding.PKCS1v15(), hashes.SHA256())
    return message + '.' + encoded(signature)


fixtures = dict(jwk=jwk, valid=token(), wrong_signature=token(signer=other))
for name, changes in dict(wrong_issuer={'iss':'https://evil.invalid'}, wrong_audience={'aud':'other'},
                         expired={'exp':1700000001}, future_iat={'iat':4102444700},
                         future_nbf={'nbf':4102444700}, wrong_nonce={'nonce':'other'},
                         wrong_subject={'sub':'other'}, missing_azp={'aud':['offline-client','other']},
                         wrong_azp={'azp':'other'}, multi_audience={'aud':['offline-client','other'],
                                                                 'azp':'offline-client'}).items():
    fixtures[name] = token(claims | changes)
fixtures['duplicate_claim'] = token(raw=compact(claims)[:-1] + b',"sub":"other"}')
fixtures['unknown_kid'] = token(header=dict(alg='RS256', kid='rotated'))
fixtures['remote_jku'] = token(header=dict(alg='RS256', kid='fixture', jku='https://evil.invalid/jwks'))
fixtures['unsupported_alg'] = token(header=dict(alg='none', kid='fixture'))
fixtures['no_nonce_refresh'] = token({k:v for k,v in claims.items() if k != 'nonce'})
Path(__file__).with_name('oauth_oidc.json').write_text(json.dumps(fixtures, indent=2) + '\n')
