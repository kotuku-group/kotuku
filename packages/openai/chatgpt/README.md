# ChatGPT-plan credentials for Tiri

`import 'ai/chatgpt'` provides account registration, returning sign-in, protected multi-account credential storage,
rotating refresh tokens, account-specific model listing, revocation, and the restricted Responses policy for OpenAI's
Sign in with ChatGPT open-source flow. It is not the Codex coding-agent runtime. The ChatGPT-plan route is an OpenAI
preview; this release implements the contract checked on 1 October 2026.

Create an account manager with an application name and an injected browser callback:

```tiri
import 'ai/chatgpt'

accounts = chatgpt.AccountManager({
   applicationName='My application',
   openBrowser=function(URL)
      -- Open URL in the system browser without logging it.
   end
})

account, err = accounts.signIn()
if account then
   models, err = account.models()
   client = account.openAI()
end
```

The default store is `user:config/ai_chatgpt_credentials.json`. It uses the Crypto module's owner-only atomic file
replacement (mode `0600` on Linux, a protected DACL on Windows). Tokens are stored as JSON protected by file
permissions; no operating-system credential vault is used. Applications may inject a store implementing `loadState()` and `saveState(State)`; legacy `load` and
`save` fields are also accepted. That store owns equivalent
confidentiality, atomicity, and cross-process refresh coordination. The manager serialises refreshes within one process,
honours `earliest_refresh_at`, and commits each rotating token set as one record.

`signIn(AccountID)` reuses the issued client ID and associated ID/email hints. `reauthorise(AccountID,
{consent=true})` is the explicit re-consent path; routine sign-in does not force consent. `disconnect(AccountID)` attempts
remote refresh-token revocation and then clears local bearer credentials. Use `{forget=true}` to remove the registration
mapping as well. `usageSettings()` opens (when a callback is configured) and returns the ChatGPT usage settings URL.

`account.openAI()` always uses the selected ChatGPT account, the policy and the `https://api.openai.com/v1` resource;
its options cannot replace them. It never changes billing paths. An application
that wants an API-key fallback must ask the user and explicitly call `accounts.apiKeyClient(Key)`.

The policy requires `store=false`, `stream=true` and complete context in an input array. It rejects unsupported fields,
system-message Items, and unsupported hosted tools before transport. Function and custom tools may be grouped in
`namespace` tools or supplied in `additional_tools` input Items, as the preview documentation recommends. Choose model
slugs from `account.models()`; the policy does not check model visibility locally. Errors expose stable
categories including `ineligible`, `usage_limited`, `temporarily_unavailable`, `unsupported_capability`,
`invalid_authorization_context`, `revoked`, `reauthorisation_required`, and `client_configuration` while retaining
HTTP/provider diagnostics already bounded and redacted by `ai/openai`.  A client-configuration failure preserves the
credential record so the application can diagnose or repair the issued-client configuration.

The maintained guide covers the account lifecycle, restrictions, error recovery, credential storage, incident response,
the compatibility matrix and migration when the preview changes:
[Tiri-ChatGPT-API](https://github.com/kotuku-group/kotuku/wiki/Tiri-ChatGPT-API).
