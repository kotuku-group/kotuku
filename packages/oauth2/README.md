# OAuth 2.0 for Tiri

`import 'net/oauth2'` binds the `oauth2` namespace.  The package requires the Crypto, HTTP and Network native modules
and the `json` and `net/url` packages.  It provides synchronous operations that pump the calling thread's event loop.
Application callbacks run on that thread, can cancel an operation, and must return promptly.  Each client permits one
pending authorisation or operation.  Use separate clients for separate accounts; serialise refreshes in the credential
store, including across processes when applicable.

## Support boundaries

| Operation | Support |
|---|---|
| Authorization code | S256 PKCE, manual exchange or bounded IPv4 loopback browser callback. |
| OIDC | Explicit trusted issuer, RS256, strict JSON/JWT/JWK parsing, signature and claim validation, bounded discovery/JWKS caches. |
| Device grant | Explicit custom endpoint configuration; OAuth-only RFC 8628 polling. |
| Refresh | OAuth refresh token or prior OIDC token record; rotating refresh tokens are returned to the caller. |
| Revocation | Explicit RFC 7009 endpoint; HTTP 200 succeeds even with an empty body. |
| Google/Microsoft presets | Endpoint/default-scope configuration for code flow.  No live provider certification is claimed. |
| Legacy `getGoogleTokens()` / `getMicrosoftTokens()` | Return `unsupported_provider_flow`.  Their device flows have not been validated with suitable registrations/scopes. |
| Microsoft logout | Not token revocation; no revocation endpoint is supplied by the preset. |
| OIDC device, implicit/hybrid grants, non-RS256 signatures, IPv6 callbacks | Unsupported. |

The implementation follows [RFC 8252](https://www.rfc-editor.org/rfc/rfc8252.html),
[RFC 8628](https://www.rfc-editor.org/rfc/rfc8628.html), [RFC 7009](https://www.rfc-editor.org/rfc/rfc7009.html), and
[OIDC Core](https://openid.net/specs/openid-connect-core-1_0.html).  Provider registration, allowed scopes, issuer policy
and consent requirements remain application configuration.  Microsoft multi-tenant issuer selection is not inferred
from an unverified token: configure a concrete trusted tenant issuer.  OpenAI dynamic registration belongs in its own
adapter, not a global provider preset.

## Client configuration

```tiri
import 'net/oauth2'

client = oauth2.Client({
   clientID = 'registered-public-client',
   scope = 'api',
   custom_endpoints = {
      auth_endpoint = 'https://issuer.example/authorize',
      token_endpoint = 'https://issuer.example/token'
   },
   presentAuthorization = function(URL:str)
      -- Pass URL to an application browser/UI.  Never write it to diagnostic logs.
      print('Present the sign-in link in your application.')
      return false -- Replace this example with the application's presentation implementation.
   end
})
```

`Client(Config)` validates configuration and raises on invalid configuration.  Policy options are copied at creation;
the three presentation callbacks on the returned client can be replaced.  Public `clientID`, `scope`, `redirectURI`,
`state` and `pkce` fields are informational; changing them does not change a private attempt's binding.  `state` and
`pkce` are cleared when the attempt is consumed or cancelled.  Do not log them.

| Option | Contract/default |
|---|---|
| `clientID` | Required non-empty registered client ID, at most 256 bytes. |
| `clientSecret` | Optional secret; uses `client_secret_post`.  Omit for public/native clients.  Other client authentication modes are unsupported. |
| `provider` | `google` or `microsoft`; optional when `custom_endpoints` is supplied. |
| `custom_endpoints` | Trusted endpoint configuration.  Overrides the preset; `auth_endpoint` and `token_endpoint` are required. |
| `scope` | Space-separated requested scopes; defaults to the preset's mail scope.  `openid` requires `oidc`. |
| `requiredScopes` | Optional native string array of grants required before returning a token.  Returned grants must be a subset of requested scopes. |
| `redirectURI` | `http://127.0.0.1:0/callback`; explicit port and simple absolute path, without query or fragment.  Port zero is for `authorizeWeb()` only. |
| `open_browser` / `presentAuthorization` | `Callback(URL)`; browser callback takes precedence.  Return `false` for failure, call `cancel()` to cancel.  No URL is printed automatically. |
| `presentDevice` | `Callback({user_code, verification_uri, expires_in})`; device code is withheld.  Return `false` to cancel. |
| `timeout` | HTTP request deadline, 30 seconds; range 0.05–120. |
| `authorizationTimeout` | Overall authorisation deadline, 300 seconds; range 0.05–1800. |
| `maxResponseBytes` | 262144; range 1024–1048576.  Strict protocol JSON has an additional fixed 262144-byte limit and depth 24. |
| `oidc` | Trusted issuer configuration below. |
| `allowIssuedClientID` | Explicit adapter permission for a registered client ID issued during consent; defaults to false. |
| `resolveClientID` | Optional trusted adapter callback receiving the state-validated browser callback parameters; returns the issued client ID.  Requires `allowIssuedClientID`. |
| `tokenParams` | Optional fixed string fields added to authorization-code and refresh token requests. Protocol and credential fields are reserved and cannot be replaced. |
| `testLoopback` | Explicit fixture-only permission for HTTP endpoints on literal `127.0.0.1`.  Does not disable HTTPS certificate checking. |
| `transport` | Optional trusted synchronous transport for fixtures/embedding: `Request => {status, body, headers}`.  Request contains `method`, `url`, `form`, `deadline`.  It receives credentials and must enforce its deadline, TLS policy, bounded I/O and no redirects.  Cancellation is checked before/after it returns. |

Endpoint configuration may additionally contain `revoke_endpoint`, `device_endpoint`, `supports_device_flow=true`,
`default_scope` and `auth_params`.  Put authorisation query parameters in `auth_params`, not in the endpoint URL.
Generic clients never force consent.  The Google preset requests offline access;
callers can explicitly supply `prompt='consent'` when necessary.  Authorisation parameters may not replace
`response_type`, `client_id`, `redirect_uri`, `scope`, `state`, `nonce` or PKCE fields.  HTTPS endpoint URLs must have a
DNS/IPv4 authority without credentials or fragments; IPv6 endpoint literals are currently unsupported.

## Operations and results

- `client.authorizeWeb(ExtraParams)` binds before presenting the URL and returns a token record or `nil, Message, Details`.
  The listener allows four sockets, 8192 bytes per request and a five-second per-connection deadline, bounded by the
  overall deadline.  It accepts only the exact GET callback path, Host including selected port and matching state,
  including on consent denial.  Invalid local requests are closed without consuming the attempt.  Callback replies
  contain fixed text and no reflected parameters.  Listener/socket resources are released before the operation returns.
- `client.buildAuthUrl(ExtraParams)` creates a pending attempt for a configured non-zero callback port and returns its
  URL.  It raises if another attempt/operation is pending.  For an external callback, the application must validate
  exact URI/method/Host/state and duplicates before invoking `exchangeCodeForTokens(Code, IssuedClientID)`.
  An exchange consumes the attempt even on failure; a failed exchange must not be retried with the same code.
- `client.cancel()` is idempotent and clears pending attempt material.  Active operations observe cancellation during
  event processing.  It cannot interrupt a user callback or injected transport that blocks without returning.
- `client.authorizeDevice()` requires explicit device support and `presentDevice`.  Polling uses monotonic deadlines,
  respects `authorization_pending`, adds five seconds for `slow_down`, and backs off transport/timeouts.  Denial and
  expiry are terminal.  No OIDC device-flow support is implied by this API.
- `client.refreshToken(Previous)` accepts a refresh-token string for OAuth-only clients or a prior record.  OIDC requires
  a trusted prior record from the owning store, including issuer, subject, client ID and original nonce.  A returned ID
  token is verified and bound to that identity; absent refreshed ID tokens retain the prior identity.  If a refresh
  response includes a nonce it must match the original; omission is permitted.  Missing replacement refresh tokens
  retain the old token.  Commit rotating tokens atomically outside this package.
- `client.revokeToken(Token, Hint)` accepts optional `access_token` or `refresh_token`, returning `true` or
  `nil, Message, Details`.  Revocation does not delete caller-owned records.
- `client.validateToken(Token)` returns a boolean and message for local access-token presence/expiry only.  It does not
  prove remote validity, signature or identity.

Token records contain `access_token`, canonical `token_type='Bearer'`, optional `refresh_token`, `expires_in`,
`expires_at` (Unix seconds), `refresh_at` (a separate suggested refresh time), optional provider-supplied
`earliest_refresh_at`, granted `scope`, `clientID`, optional
`id_token`, original `nonce`, `validated_oidc`, and optional `identity={issuer, subject, clientID, claims}`.  Missing
expiry stays unknown; it is not invented.  OAuth-only records never claim a verified identity.  Identity claims and
credentials are sensitive.  Provider-defined response fields are not copied into the validated record.

Errors have a stable broad `Details.code`, numeric HTTP `status` (zero when unavailable), `bodyBytes`, and safe
`message`.  Known OAuth error codes such as `invalid_grant`, `access_denied` and `invalid_scope` are retained.  Unknown
provider error text, bodies, arbitrary response headers and URLs are not surfaced because they can echo secrets.
HTTP headers needed for cache control are read internally.  No persistence, account selection or automatic logging is
performed.  Avoid native wire/debug logging when processing credentials.

## OIDC configuration

Set `scope='openid ...'` and `oidc={issuer='https://trusted.example'}`.  Discovery defaults to the issuer plus
`/.well-known/openid-configuration`; an optional `discoveryURL` must share its origin.  `jwksURL` can pin an exact JWKS
URL and is required when discovery points to another origin.  Neither JWT headers nor claims choose network addresses.
Redirects are not followed.  Issuer matching is exact; multi-tenant templates and arbitrary issuer discovery are not
supported.

`cacheSeconds` defaults to 300 (range 0–3600); actual entries live no longer than 300 seconds, the configured bound or
server cache policy, whichever is smaller.  `no-store`, `no-cache`, `max-age` and `Age` are honoured conservatively.
Caches are private to each client/issuer.  An unknown key ID permits one forced key refresh, throttled to once per
minute across attempts.  Key sets are limited to 32 uniquely named keys.  Unknown keys after refresh fail closed.

`clockSkew` defaults to 60 seconds (range 0–300).  RS256 signature, issuer, audience, authorised party, subject,
expiry, issued-at, nonce, applicable not-before/authentication-time and access-token hash claims are checked.
`requireCallbackIssuer=true` additionally requires a matching `iss` in the browser callback.  A supplied callback
issuer is always checked.  Duplicate JSON/JWK members and key IDs, unsupported algorithms, malformed UTF-8, invalid
escapes/numbers, trailing JSON, critical JOSE extensions and token-supplied key URLs are rejected.

## Migration and validation

Use `import 'net/oauth2'`, not assignment-style imports or `import 'oauth2'`.  Replace old `client_id`, `client_secret`
and `redirect_uri` option names with `clientID`, `clientSecret` and `redirectURI`; obsolete names raise an explicit
migration error.  Provider request fields remain snake-case on the wire.  Replace automatic console instructions with
presentation callbacks.  Call `cancel()` before replacing a manual attempt.  `expires_at` now represents actual expiry,
not expiry minus a minute; use `refresh_at` for proactive refresh.  Successful token fixtures must include `token_type`.

Run the offline Flute suite after installation:

```sh
build/agents-install/origo tools/flute.tiri file=packages/tests/test_oauth2.tiri --log-warning --gfx-driver=headless
```

Tests use loopback servers and public signed fixtures, with no credentials, browser or provider account.  Regenerate
OIDC fixtures with `python3 packages/tests/fixtures/generate_oauth_oidc.py` when Python's `cryptography` package is
available; the signing key is ephemeral and is never saved.  Live provider validation and Windows native Crypto
release validation remain separate from this offline package contract.
