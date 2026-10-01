# Tiri OpenAI Responses client

`ai/openai` is a provider-specific client for the public OpenAI Models and Responses HTTP APIs. It supports API keys,
bearer credentials, callback-managed bearer refresh, streaming SSE, structured output, custom function tools and
caller-managed conversation history. It does not provide ChatGPT-plan sign-in; that policy belongs to `ai/codex`.

```tiri
import 'ai/openai'

client = openai.Client({ credential=openai.apiKey(ApiKey) })
response, err = client.responses.create({ model=Model, input='Explain the result briefly.' })
if err then raise err.message end
print(response.outputText())
```

Credentials are client-owned providers. Use `openai.apiKey(Key)`, `openai.bearerToken(Token)`, or
`openai.refreshableBearer(Callback)`. The callback is invoked for every request and may return a token string or a
table containing `accessToken`, `access_token`, or `token`; this keeps refresh timing and secure token ownership with
the caller. Authorization headers cannot be overridden through client options. Diagnostics redact common bearer and
credential forms.

`client.models.list()` returns normalised entries with `id`, `created`, `ownedBy`, and `displayName`, plus `raw` for
provider fields unknown to this library. An optional `filter(Model)` callback controls visibility without assuming a
particular account catalogue.

`client.responses.create(Request)` validates and copies its request. Both string and Item-array input are supported.
Structured output uses the native Responses shape: `text={ format={ type='json_schema', name=..., schema=...,
strict=true } }`. Non-streaming calls return a response wrapper; streaming calls return a cancellable stream whose
terminal result has the same wrapper shape. `outputText()`, `functionCalls()`, and `inputItems()` are conveniences;
the complete provider payload remains in `raw`, including unknown Items, content parts, events, and fields.

`openai.ToolRegistry()` is an explicit allow-list. Register a name, JSON Schema, handler, and optional
`consequential=true`. Dispatch strictly decodes and validates function arguments, rejects unregistered names, invokes
the registry approval callback before consequential handlers, and emits a `function_call_output` with the original
`call_id`. Applications remain responsible for deciding when to dispatch model requests.

`openai.History()` stores copied input and output Items, including reasoning and function-call Items. Replaying full
history is caller-managed: pass `history.items()` as the next request's `input`. Every replay sends those Items again,
so input token use, latency, and API cost generally grow with conversation length. Preserve all reasoning and
function-call/output pairs required by the selected model; trim or summarise only when the application accepts the
semantic trade-off. `previous_response_id` remains available to ordinary API-key callers and is not imposed by this
helper.

Use `client.capabilities()` before exposing UI controls. It reports transport, credential and feature support and can
be narrowed by an attached policy. Background Responses, Conversations persistence, WebSocket transport, Realtime,
uploads, and image generation are not implemented by this package.
