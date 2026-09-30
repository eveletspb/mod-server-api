# Authentication

The API uses the provider selected by `ServerApi.Auth.Provider`. The default
is `none`, which selects `NoAuthProvider` for local development. Authorization
roles and scopes are not implemented; authenticated callers share the same
access to API endpoints.

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Auth.Provider = "none"
```

## Registering a provider

Providers are C++ modules compiled into the same worldserver. Register a
factory during module initialization, before `ServerApiWorldScript` starts the
listener:

```cpp
#include "ServerApi/Authentication.h"

ServerApi::GetAuthenticationProviderRegistry().Register("my-provider", []
{
    return std::make_unique<MyAuthenticationProvider>();
});
```

Names contain lowercase ASCII letters, digits and hyphens. Duplicate or
invalid names are rejected. A provider implements `Authenticate` and
`RequiresAuthentication`. The request is a read-only view of method, target and
headers; use `request.HeaderValue("header-name")` for case-insensitive lookup.
Return `AuthenticationResult::Authenticated(subject)` to identify a caller or
`AuthenticationResult::Reject(challenge)` to reject it. The server attaches the
configured provider name to the resulting identity. `NoAuthProvider` returns
an accepted anonymous result without identity.

Provider factories may read their own settings from AzerothCore's config
manager under `ServerApi.Auth.<provider-name>.*`. The provider runs
synchronously on the API I/O worker, so authentication must be a fast local
check. Do not perform network or blocking database calls, and never access
world objects from it. Provider exceptions fail closed with a generic `500`
response; credentials and exception details are not sent to clients or logged.

The selected provider authenticates all versioned REST requests, including
`/api/v1/mod/<module>/...`, and WebSocket handshakes at `/ws/v1/events`.
`/health` and `/ready` remain public. A rejected request receives `401`; the
provider may supply a `WWW-Authenticate` challenge.

Unknown or unregistered provider names prevent the listener from starting.
Non-local bind addresses also require a selected provider whose
`RequiresAuthentication()` returns `true`; `NoAuthProvider` is local-only.
Changing the provider or listener requires a worldserver restart.
The former `ServerApi.Auth.Enable` and `ServerApi.Auth.ApiKey` settings are no
longer read; replace them with a registered provider name.

The listener does not provide TLS. Remote clients should connect through a
trusted TLS-terminating proxy or protected tunnel. Do not expose credentials or
identity headers over plain HTTP on an untrusted network.

## HTTP status codes

| Status | Meaning |
|---:|---|
| `400` | Invalid request or command parameters |
| `401` | Configured provider rejected the request |
| `404` | Endpoint or runtime resource not found |
| `405` | Known endpoint called with an unsupported HTTP method |
| `413` | Request exceeds configured size limit |
| `429` | Global HTTP request rate limit exceeded |
| `500` | Authentication provider failed or returned an invalid result |
| `501` | An optional module integration is unavailable |
| `503` | Command/WebSocket/character request capacity reached or a required database is unavailable |
