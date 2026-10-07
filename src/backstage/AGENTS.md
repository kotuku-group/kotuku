# Backstage development

REST and `/streaming` share one loopback `NetServer`.  The optional official WebSocket module owns all RFC 6455
handshake, frame, UTF-8, ping/pong, backpressure and close behaviour.  Do not introduce a local protocol fallback.

With `BACKSTAGE_WEBSOCKET`, create an external-listener `WebSocketServer` and pass the complete buffered opening
request, including trailing frames, to `Adopt()`.  Forward Incoming before reading HTTP, Outgoing with an `Okay`
callback result, and Disconnected before removing HTTP state.  Only `NotFound` selects ordinary HTTP processing.
Without the module, REST remains available and `/streaming` returns 501.

Sessions are keyed by `WebSocket.UID`.  Keep loopback-origin policy, streaming JSON and topic logic in websocket.cpp.
Never hold `glWebSocketLock` while calling a WebSocket method.  All protocol calls run on the listener's message
processing thread; future background producers must post work to that thread.

Release the adoption server before the listener and the loaded WebSocket module.  Keep callback targets valid through
shutdown.  Other external listener integrations must explicitly forward shutdown disconnections before freeing a
listener first, because NetServer suppresses Feedback during destruction.  Use the C++ programming, Tiri programming and Flute testing skills for changes and tests.
