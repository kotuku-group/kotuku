# Localhost SSL Test Fixtures

These self-signed `localhost` certificates and private keys exist solely for the Network module's SSL tests.  They are
installed to `config:ssl/` and must never be used to secure a real service: the private keys are public.

| File | Content |
| --- | --- |
| `localhost.pem` | RSA 2048 certificate |
| `localhost-key.pem` | Its private key in PKCS#8 form |
| `localhost-rsa-key.pem` | The same private key in traditional PKCS#1 form |
| `localhost-ec.pem` | ECDSA P-256 certificate |
| `localhost-ec-key.pem` | Its private key in traditional SEC1 form |
| `localhost.p12` | PKCS#12 bundle of the RSA certificate and key, empty password |
| `localhost-password.p12` | The same bundle with the password `kotuku-test` |

The files are permanent fixtures so that no platform needs the `openssl` executable to build or test.  They are valid
for 100 years and are loadable by both the OpenSSL and Windows Schannel backends.  If they ever need to be replaced,
run this from a Linux host and commit the results:

```bash
cmake -P src/network/generate_ssl_certificates.cmake
```
