# Authentication

The API is enabled by default on `127.0.0.1` without authentication. Authorization
scopes and RBAC are not implemented; all authenticated clients currently have
the same access to the exposed endpoints.

## Bearer authentication

To enable Bearer authentication:

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Auth.Enable = 1
ServerApi.Auth.ApiKey = "change-this-secret"
```

Send the key on versioned REST requests and WebSocket handshakes:

```http
Authorization: Bearer change-this-secret
```

Never commit a production key to the repository or log it.

## No-auth mode

This is the default for the trusted local listener:

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Auth.Enable = 0
ServerApi.Auth.ApiKey = ""
```

In this mode REST and WebSocket endpoints accept requests without a token.
Non-local bind addresses are rejected unless Bearer authentication is enabled
with a non-empty key. This prevents accidental exposure of administrative
operations.

WebSocket clients are also protected by the configured frame, subscription,
per-client queue and total client limits. A slow consumer that overflows its
queue is closed; a new connection above `ServerApi.WebSocket.MaxClients`
receives `503 WS_CLIENT_LIMIT`.

## HTTP status codes

| Status | Meaning |
|---:|---|
| `400` | Invalid request or command parameters |
| `401` | Missing or invalid token when auth is enabled |
| `404` | Endpoint or runtime resource not found |
| `405` | Known endpoint called with an unsupported HTTP method |
| `413` | Request exceeds configured size limit |
| `429` | Global HTTP request rate limit exceeded |
| `501` | An optional module integration is unavailable |
| `503` | Command queue or WebSocket client limit reached |
