# OBCX Bridge Actor

`obcx-actor-bridge` is the V2 actor that forwards stored messages between QQ
and Telegram. It is loaded through the OBCX actor ABI and has one entry point,
`obcx_create_actor_v2`.

The actor accepts `obcx::message_store::events::MessageStored` envelopes from
[`obcx-actor-message-store`](../obcx-actor-message-store), performs QQ or
Telegram I/O through `ActorContext::await_asio`, and emits
`bridge::events::MessageForwarded` or
`bridge::events::MessageForwardFailed`.

## Package contract

- Canonical metadata: `actor.toml`
- Actor id: `vollate.bridge`
- ABI: `2`
- Artifact: `bridge.so`
- CMake target: `bridge_actor`
- Runtime dependency: `onebot-cxx.message-store >=0.1.0,<1.0.0`

## Build against an installed SDK

Install OBCX first, then configure this repository with that prefix:

```bash
cmake -S /path/to/OBCX -B /tmp/obcx-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/obcx-build -j2
cmake --install /tmp/obcx-build --prefix /tmp/obcx-sdk

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/tmp/obcx-sdk
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/bridge-package
```

The installed package contains:

```text
lib/obcx/actors/bridge.so
share/obcx/actors/vollate.bridge/actor.toml
```

## Runtime configuration

Start from [`actor-config.example.toml`](actor-config.example.toml). The
supported pipeline is:

```text
obcx::core::events::RawMessageEvent -> message_store ->
obcx::message_store::events::MessageStored -> bridge
```

Bridge-specific settings live under `[actors.bridge.config]`. Both bot
connections must be configured because the actor resolves them through the
runtime `BotRegistry`. `bridge_files_dir` is required. A representative
configuration is:

```toml
[actors.bridge]
library = "bridge"
enabled = true
partition = "source_platform:conversation_id"
db = "main"
db_namespace = "bridge"

[actors.bridge.config]
database_file = "bridge_bot.db"
enable_retry_queue = true
bridge_files_dir = "/tmp/bridge_files"
bridge_files_container_dir = "/root/llonebot/bridge_files"
ffmpeg_path = "ffmpeg"
gif_max_file_size = 0
gif_max_duration = 5
gif_max_fps = 0
gif_max_width = 0
gif_max_colors = 256
```

`ffmpeg_path` may be an absolute executable path; its default, `ffmpeg`, uses
the process `PATH`.

`action_timeout` should remain above the upstream first-media-send latency;
30 seconds is the example default. Telegram `poll_force_close` must be larger
than `poll_timeout`.

## Features

- Bidirectional QQ group and Telegram group/topic forwarding
- Cross-platform reply and message-id mapping
- Image, video, audio, document, sticker, and GIF forwarding
- WebM/TGS-to-GIF conversion with size fallbacks
- Persistent retry queues with exponential backoff
- Group-to-group and topic-to-group mappings

## Tests

The repository tests cover actor dispatch, mapping persistence, retries,
message adaptation, database schema, and forwarding failure behavior. The OBCX
cross-repository conformance test additionally proves clean-SDK configure,
build, install, metadata installation, and test execution.

## License

MIT
