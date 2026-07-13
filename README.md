# OBCX Bridge Actor

`obcx.bridge` is the ABI-2 actor package that forwards `MessageStored` events
between QQ and Telegram and emits `MessageForwarded` or
`MessageForwardFailed`. `actor.toml` is its only package metadata contract.

The actor keeps mailbox transitions on the native OBCX scheduler. QQ and
Telegram operations remain Boost.Asio awaitables, crossed only through
`ActorContext::await_asio`; they do not occupy actor workers while suspended.

## Runtime services

The host registers these services in `ActorContext`:

- `DbManager` for mappings, retry state, media cache, recall, and reply state;
- `BotRegistry` for QQ and Telegram bot instances;
- `boost::asio::any_io_executor` for forwarding I/O;
- optional `BridgeRuntimeConfig` for mappings, retry/media limits, and paths;
- optional `IBridgeForwarder` for a custom forwarding implementation.

The bridge actor is the only dynamic extension entry point. QQ-to-Telegram and
Telegram-to-QQ handlers are implementation details behind that actor.

## Build and test

Install OBCX's actor SDK, then run:

```bash
cmake -S . -B build -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH=/path/to/obcx-sdk
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/actor-prefix
```

The package installs its actor artifact under `lib/obcx/actors`. The test suite
covers actor suspension/resume, forwarding and failure results, message
mapping, reply/recall state, retry state, media-group state, and DB reuse.

## Pipeline

The received-message pipeline persists first, then forwards:

```toml
[actors.message_store]
library = "message_store"
enabled = true

[actors.bridge]
library = "bridge"
enabled = true

[pipelines.received_message]
source = "RawMessageEvent"

[[pipelines.received_message.stages]]
name = "persist"
actor = "message_store"
input = "RawMessageEvent"
output = "MessageStored"
mode = "await"

[[pipelines.received_message.stages]]
name = "forward"
actor = "bridge"
input = "MessageStored"
output = ["MessageForwarded", "MessageForwardFailed"]
after = ["persist"]
mode = "await"
```
