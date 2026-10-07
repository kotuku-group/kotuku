# Network feature status

## WebSocket external listeners

`WebSocketServer` supports `WSF::EXTERNAL_LISTENER`, `Adopt(ClientSocket, RequestData, Connection)` and
`Dispatch(ClientSocket, WSE)` to serve WebSocket and HTTP requests on one existing `NetServer`.  Dedicated listeners
retain the same protocol engine.  Adopted sockets use the official message assembly, validation, bounded send queue
and closing handshake.  The listener remains the framework owner and must forward all three socket event types.

Backstage `/streaming` uses this contract on its REST port, with a 64 KiB message limit, a bounded 256 KiB send queue
and its existing loopback-origin policy.  Builds without WebSocket support retain REST and return 501 for streaming.

See [the migration plan](backstage_websocket_migration.md) for implementation and validation details.
