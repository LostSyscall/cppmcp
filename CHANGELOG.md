# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [0.2.0] — 2026-08-15

Protocol-conformance and engineering hardening pass (19 findings from a full
audit of server, client and build infrastructure).

### Fixed — protocol conformance

- **protocolVersion negotiation**: the server now echoes a supported client
  version or responds with its latest (`2025-06-18`); the client validates the
  negotiated version and fails the handshake on an unknown one. Previously the
  server always returned `2025-03-26` while the client sent a non-existent
  `2025-11-25` with no validation.
- **`_meta.progressToken`** is now honored: progress notifications carry the
  client-supplied token (string or number) instead of always echoing the request
  id. Third-party clients no longer lose progress.
- **Parameter type errors** return `-32602 Invalid Params` (previously
  `-32603 Internal Error` via a nlohmann type_error).
- **`SERVER_NOT_INITIALIZED` (-32002)** replaces `INVALID_REQUEST` for
  pre-handshake requests, matching the SDK convention.
- **`completion/complete` with malformed `ref`/`argument` sub-objects** no
  longer trips `JSON_ASSERT` (Debug crash / Release UB) — fields are
  type-checked before access.
- **`logging/setLevel`** validates the level against the spec's enum.
- **Requests with `id: null`** are rejected per JSON-RPC 2.0; fractional ids
  classify as `INVALID_REQUEST` rather than `PARSE_ERROR`.
- **`modelPreferences`** correctly modeled for `sampling/createMessage`
  (was a plain `model` string, silently dropped by spec-compliant servers).
- Added `AudioContent` and resource `annotations` (audience/priority) types.
- `ResourceContents` omits an empty `mimeType` instead of serializing `""`.

### Fixed — server

- **Release-build crash on shutdown** (pre-existing, all HTTP tests): member
  destruction order destroyed `io_ctx_` before `transport_`, leaving
  HttpTransport's sockets/acceptor referencing a dead io_context. `~McpServer`
  now drops the transport explicitly first.
- **HTTP listener actually binds `config.host`** (was always `0.0.0.0`,
  exposing a "localhost" server to every interface).
- **HTTP batch + worker pool**: deferral is disabled for batch elements so
  every response is collected into the array body (responses of deferred
  elements were previously dropped and the client timed out).
- **Non-initialize POST without `Mcp-Session-Id`** returns 400 (was 404);
  uninitialized sessions return a JSON-RPC `-32002` error body (was plain 403).
- **initialize failure** (JSON-RPC error response) now rolls back the session;
  the half-registered session no longer survives a rejected handshake.
- **Session teardown deadlock**: `HttpTransport::stop()`/`handle_delete()`/
  `sweep_sessions()` closed SSE sockets while holding `sessions_mutex_`;
  `on_error() -> on_disconnect()` re-acquires the same non-recursive lock —
  the shutdown path hung forever. Connections are now closed outside the lock.
- **Session lifetime**: `session_idle_timeout` defaults to 1h (was 0 = never);
  expired/DELETEd sessions close their SSE sockets so connection slots are
  actually freed (idle SSE read loops used to hold them forever).
- **HTTP hardening**: header count/length caps, URL length cap, `Accept` check
  on POST (406), parse errors return HTTP 400, query strings tolerated on the
  MCP path, 503 reply at the connection cap (was silent close).
- **stdio stdout writer** moved to a dedicated thread with a bounded queue —
  a full parent pipe can no longer stall the io event loop. Win32 console
  reader uses weak references (no UAF after transport destruction). Oversize
  lines drop the connection instead of buffering unboundedly (also in pipe).
- **`resources/subscribe`** is capability-gated and routed per session:
  `notify_resources_updated()` now only notifies sessions that subscribed
  (broadcast fallback preserved for stdio/pipe single-client use).
- **`register_resource_template()`**: RFC 6570 simple-string templates
  (`weather://{city}/current`) match on `resources/read` and are advertised
  via `resources/templates/list`.
- Tool `inputSchema` subset validation (type/required/enum/items) and prompt
  required-argument validation run before user handlers.
- Optional list pagination: `set_list_page_size(n)` honors client cursors and
  emits `nextCursor`.
- `~McpServer()` now calls `stop()`.

### Fixed — client

- **Reconnect**: `connect()` re-attaches transport handlers (transports clear
  them on `disconnect()`; a second connect used to leave the transport deaf
  and the handshake timed out).
- **HTTP non-2xx responses** synthesize a JSON-RPC error so the pending
  request completes instead of hanging forever (404 session-expired maps to
  `CLIENT_NOT_CONNECTED` per spec's re-initialize requirement).
- **User callbacks are exception-isolated** (`on_complete`/`on_error`/
  `on_progress`/notification handlers/disconnect handler): a throwing callback
  logs and drops instead of terminating the process via the io thread.
- **Deadlock fix**: `stop()`/`disconnect()` called from a callback running on
  the client strand (external-io mode) detects the re-entrancy and drains
  inline instead of blocking on a posted task that can never run.
- **Requests fail fast** with `CLIENT_NOT_CONNECTED` when disconnected;
  `set_default_request_timeout()` bounds request waits against dead servers.
- **HTTP transport**: GET SSE stream opened after the first successful POST
  (server-initiated sampling/elicitation/roots/notifications now arrive over
  HTTP); `text/event-stream` POST responses parsed as SSE frames;
  `Mcp-Session-Id` cleared on disconnect; DELETE terminates the session
  (fire-and-forget — waiting for the response deadlocked against keep-alive
  servers); custom headers via `set_extra_headers()`.
- **Progress tokens**: custom numeric tokens no longer collide with request
  ids in the pending map (string tokens are passed through verbatim).
- **spawn**: environment variables (`set_env`) and working directory
  (`set_working_dir`); graceful shutdown (stdin EOF + 2s grace) with forceful
  escalation; Unix SIGKILL after SIGTERM grace (terminate could block
  forever); child exit code exposed; stderr NUL fallback for GUI/service
  parents; Windows argv quoting per CreateProcess rules.
- New APIs: `subscribe_resource`/`unsubscribe_resource`/`complete`,
  `on_resource_updated`/`on_list_changed`/`on_server_log`/`on_roots_changed`
  callbacks, cursor-aware list methods.

### Added — server → client requests

`McpServer::request_sampling()/request_elicitation()/request_roots()` (and
`RequestContext` equivalents) send sampling/elicitation/roots requests with
capability gating, id correlation and async result callbacks — completing the
loop with the existing client-side handlers.

### Engineering

- GitHub Actions CI: Windows/MSVC + Linux/GCC × Debug/Release with unit +
  integration tests, clang-format check, ASan+UBSan job.
- `.clang-format`, `.editorconfig`, `.gitattributes` (LF normalization).
- `/W4` (MSVC) / `-Wall -Wextra -Wpedantic` (GCC/Clang) on library targets;
  all warnings resolved.
- `vcpkg.json` manifest completed (`name`/`version`/`description`/`license`).
- `CMakePresets.json` (shared, env-based `VCPKG_ROOT`); personal
  `CMakeUserPresets.json` untracked.
- Dockerfile proxy/mirror become build-args (portable builds off this machine).
- ctest per-test TIMEOUT (unit 60s, integration 120s).
- 19 new regression tests (92 total, was 73): version negotiation, param
  validation, progressToken echo, template matching, subscribe routing,
  pagination, crash vectors, reconnect, callback isolation, fail-fast,
  notification routing, unsupported-version handshake.

## [0.1.0]

Initial release: MCP server (stdio / SSE / Streamable HTTP / local pipe) and
client (stdio / HTTP / local pipe), JSON-RPC 2.0 core, async I/O on asio +
llhttp, worker/io thread pools, 73 tests.
