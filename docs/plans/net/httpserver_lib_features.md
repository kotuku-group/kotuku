# HTTP server library feature status

## Future WebSocket integration

The official `WebSocketServer` now provides an external-listener adoption contract suitable for `packages/httpserver`.
An HTTP listener can pass a complete opening request and all post-header bytes to `Adopt()` and forward Incoming,
Outgoing and Disconnected with `Dispatch()`.  It must propagate Incoming termination, preserve the global Outgoing
callback, and run both objects on the same message-processing thread.

Backstage is the first integration.  Migrating `packages/httpserver` remains separate work; this change does not
modify that package.  See [the adoption and migration plan](backstage_websocket_migration.md).
