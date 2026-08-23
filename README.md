# OBCX Bridge Actor

`obcx-actor-bridge` is the native ABI 2 actor that forwards stored messages
between QQ and Telegram. It consumes
`obcx::message_store::events::MessageStored` through generated reflected
dispatch and performs bot I/O with `ActorContext::await_asio`. Successful and
failed attempts emit `bridge::events::MessageForwarded` and
`bridge::events::MessageForwardFailed`, respectively.

## Package Contract

- Canonical metadata: `actor.toml`
- Actor id: `vollate.bridge`
- Actor name and version: `bridge` `0.1.0`
- ABI: `2`
- CMake target and artifact: `bridge_actor`, `bridge.so`
- Platforms: Linux x86_64 and arm64
- Runtime dependency: `onebot-cxx.message-store >=0.1.0,<1.0.0`

`OBCX_ACTOR_EXPORT_V2` exports the numeric ABI generation, factory,
destructor, actor name, actor version, and generated schema-1 input contract.
OBCX validates that contract before constructing the actor.

## Build Against An Installed SDK

The supported baseline is Linux x86_64/arm64, CMake 3.30+, GCC 16.1+, C++26,
`-freflection`, and `__cpp_impl_reflection >= 202506L`.

Install OBCX first, then configure this repository with that prefix:

```bash
cmake -S /path/to/OBCX -B /tmp/obcx-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/obcx-build -j2
cmake --install /tmp/obcx-build --prefix /tmp/obcx-sdk

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/tmp/obcx-sdk \
  -DOBCX_BRIDGE_BUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/bridge-package
```

Bridge owns forwarding, mapping, media, retry, installed pipeline, and
bot-facing reload behavior tests. `OBCX_BRIDGE_BUILD_TESTS` follows
`BUILD_TESTING` when Bridge is configured as the top-level project and defaults
to `OFF` when OBCX embeds Bridge as an actor subdirectory; an embedding consumer
may explicitly override it. The release coordinator additionally enables
`OBCX_BRIDGE_CONFORMANCE_TESTS` and supplies the installed Message Store actor
and shared install prefix so Bridge registers, runs, and installs its cross-actor
pipeline and reload smoke executables from this repository.

The installed package contains:

```text
lib/obcx/actors/bridge.so
share/obcx/actors/vollate.bridge/actor.toml
```

## Runtime Configuration

The supported pipeline is:

```text
RawMessageEvent -> command coordinator -> typed bridge command -> Continue
obcx::core::events::RawMessageEvent -> message_store ->
obcx::message_store::events::MessageStored -> bridge
obcx::core::events::RawNoticeEvent -> bridge
```

The actor declares typed observations for Telegram `recall`, `checkalive`, and
`poke`, plus QQ `checkalive`. Activate them explicitly with
`[[command_runtime.routes]]`, as shown in
[`actor-config.example.toml`](actor-config.example.toml). Platform parsing and
Telegram menu replacement belong to the runtime adapter. Only an active
`command_runtime.routes` match intercepts slash-prefixed traffic. If a message
such as `/tp 2072 ~ 1080` has no scoped route, the command coordinator submits
the original event to the ordinary message-store and bridge pipeline; bridge
handlers do not reclassify it from its leading `/`.

For a matched bridge command, the actor returns `CommandCompleted(Continue)`,
allowing message-store to persist the source event; the inherited
`obcx.command.processed` header makes the later `MessageStored` bridge stage a
no-op so the command is neither executed nor forwarded twice. A command actor
that returns `Consume` prevents the ordinary pipeline from running at all.

QQ notices use a separate actor pipeline. The runtime converts platform
`NoticeEvent` values into `obcx::core::events::RawNoticeEvent`; bridge consumes
that typed input to retain the former plugin behavior for QQ poke and group
recall notices. Configure the `pipelines.notice` stage shown in the example;
the actor does not register bot callbacks directly.

The actor uses the core `DbManager` service. `db = "main"` selects the
configured database instance and `db_namespace = "bridge"` isolates the
actor-owned schema. `bridge_files_dir` is required for media processing.

```toml
[db.instances.main]
type = "sqlite"
path = "data/obcx.sqlite3"

[actors.bridge]
library = "bridge"
enabled = true
requires = ["message_store"]
partition = "source_bot:conversation_id"
db = "main"
db_namespace = "bridge"

[actors.bridge.config]
legacy_state_pair = "primary"
legacy_unresolved_mapping_policy = "fail"
enable_retry_queue = true
message_retry_max_attempts = 5
message_retry_base_interval_sec = 2
retry_queue_check_interval_sec = 10
max_retry_interval_sec = 300
bridge_files_dir = "/tmp/bridge_files"
bridge_files_container_dir = "/root/llonebot/bridge_files"
ffmpeg_path = "ffmpeg"
gif_max_file_size = 0
gif_max_duration = 5
gif_max_fps = 0
gif_max_width = 0
gif_max_colors = 256
qq_media_download_max_bytes = 10485760
image_placeholder_url = "https://placehold.co/512x512/e9ecef/495057/png?text=NOT+FOUND"

[[actors.bridge.config.installation_pairs]]
id = "primary"
telegram_installation = "telegram_bot"
onebot11_installation = "qq_bot"

# Migration-only route history for a route no longer in group_mappings:
# [[actors.bridge.config.legacy_mapping_routes]]
# pair = "primary"
# telegram_conversation_id = "chat:-1001234567890"
# telegram_topic_id = -1
# qq_conversation_id = "group:123456789"
```

`image_placeholder_url` must be a direct image URL. It defaults to a PNG that
shows `NOT FOUND`; the embedded image is used only if this URL cannot be
downloaded or validated.

`qq_media_download_max_bytes` bounds each full QQ image downloaded after
Telegram rejects direct URL delivery. It defaults to and cannot exceed 10 MiB,
the public Telegram photo limit. A failed, expired, invalid, or oversized item
is replaced during multipart fallback without discarding valid peers in the
same media group.

Before multipart upload, the fallback also checks Telegram's photo-dimension
rules: width plus height must not exceed 10,000 pixels and the larger-to-smaller
ratio must not exceed 20. Compliant images retain their original bytes.
Recoverable overlong images are converted through `ffmpeg_path` without
upscaling or cropping; extreme ratios are padded before bounded downscaling.
Successful conversions are reported as `已调整` separately from placeholder
substitutions reported as `已替换`.

Dimension conversion runs on the process blocking executor, one image at a
time per media batch, with a 15-second per-image deadline. Images declaring
more than 64 megapixels are not decoded. `invalid_dimensions`,
`unsafe_dimensions`, and `normalization_failed` replace only the affected item
with the configured placeholder. Logs may include item indices and dimensions,
but never signed QQ URLs, complete decoder output, or complete Telegram
responses.

The forwarding runtime resolves every event by exact `source_bot` and a named,
disjoint Telegram/OneBot pair. Each installation may belong to only one pair,
and a source conversation maps to one target rather than fan-out. When more
than one pair is configured, every `group_mappings` entry must name `pair`.
Missing routes remain successful no-ops, while unknown source installations
fail without selecting another account.

Existing single-pair deployments may retain scalar `telegram_installation` and
`onebot11_installation` fields with pair-less mappings. Do not mix scalar and
named forms. Command routes must list every source bot whose Bridge commands
should be active.

### Execution domains and partitions

Configure bridge with `partition = "source_bot:conversation_id"` so equal
native conversations from different installations have independent mailboxes.
A suspended
handler still owns its partition mailbox: later messages for the same
conversation remain FIFO, while another conversation can use a different actor
worker.

Bridge resolves the process-owned `obcx::core::BlockingExecutor` from
`ActorContext`; it never obtains worker capacity from a bot. Repository calls,
filesystem operations, and media conversion run through
`BlockingExecutor::run()` inside the actor-tracked Asio graph. Bot sends,
downloads, timers, and other asynchronous transport operations remain on their
Asio executor. Do not add `std::async`, detached threads, or actor-local thread
pools for synchronous work.

`ffmpeg_path` may be an absolute executable path; its default, `ffmpeg`, uses
the process `PATH`. `action_timeout` should remain above the upstream
first-media-send latency; 30 seconds is the example default. Telegram
`poll_force_close` must be larger than `poll_timeout`.

### Conversation-scoped schema migration

Bridge-owned state uses schema version 3. Every live message identity is
`(installation, platform, conversation_id, message_id)`: QQ groups use
`group:<id>`, Telegram chats use `chat:<id>`, and Telegram topic id remains
separate route metadata. Mappings, retry rows, and media-group rows include both
source and target conversations; installation-scoped user/sticker caches and
heartbeats retain their version-2 shape. Equal Telegram message ids in two
chats are valid and MUST NOT be deduplicated or deleted as duplicates.

An unversioned database is first assigned to its deterministic legacy pair as
version 1 -> 2. Version 2 -> 3 then classifies each mapping from its exact
Message Store source identity and a current route or migration-only
`legacy_mapping_routes` entry. Telegram thread metadata selects an exact
topic-to-group route when configured; a chat-wide group-to-group route applies
to every forum thread in that chat and therefore does not require a synthetic
topic-history entry. The default
`legacy_unresolved_mapping_policy = "fail"` rolls back the complete transaction
when a source conversation, historical target route, or album primary cannot be
proven. `"archive"` is an explicit operator choice: unresolved mapping/media
rows are retained in namespaced version-2 archive tables, but forwarding,
de-duplication, replies, edits, recalls, commands, and retries never query
those tables. Pending retries cannot be archived or retargeted and block
migration until safely drained or explicitly removed after backup.

Stop OBCX and take a SQLite-consistent backup before upgrading; do not copy only
the main `.db` file while WAL writes are active. Migration and all row/count,
shape, primary, and index checks run under one transaction during typed actor
generation preparation, before scheduler or pipeline ingress can invoke
Bridge. A failed preparation rejects the generation and never publishes an
uninitialized repository. A reload candidate performs a read-only version-3
shape check and cannot perform version 1 -> 2 or version 2 -> 3 migration.
Version 3 is not readable by the preceding binary, so rollback requires
restoring both that binary and the pre-migration database snapshot; there is no
down-migration.

With OBCX stopped, create and verify the snapshot with SQLite itself, for
example:

```bash
sqlite3 bridge_bot.db ".timeout 10000" ".backup 'bridge_bot.pre-v3.db'"
sqlite3 bridge_bot.pre-v3.db "PRAGMA integrity_check;"
```

After startup, verify `SELECT MAX(version) FROM bridge_schema_version;` returns
`3`, inspect the logged live/archive counts, and confirm known equal ids are
separate by `target_conversation_id`. Do not delete WAL/SHM files or copy only
the main file from a running process.

The Message Store already keys rows by `source_bot` and `conversation_id`.
Bridge reads those existing values during preflight without changing Message
Store tables, indexes, payloads, or event types. Migration diagnostics contain
only bounded counts and non-secret route identities. The migration does not
rewrite a message that was already sent with an incorrect reply reference;
after deployment an operator must explicitly remove and resend that message if
desired. Do not delete one of two valid rows merely because their native ids
are equal in different conversations.

### Direct mapping persistence

Immediate forwarding has one explicit durability owner. QQ and Telegram
handlers return a typed outcome containing exact source/target installations,
conversations, native message ids, and one of
`NewDelivery`, `AlreadyPersisted`, or `NotForwarded`. The forwarding runtime
passes that value through without querying the mapping table after the bot
send.

For `NewDelivery`, `BridgeActor` performs the only primary mapping upsert and
publishes `MessageForwarded` only after that write succeeds. For
`AlreadyPersisted`, the pre-send de-duplication read supplies the durable
target id, so the actor emits the existing completion without another bot send
or mapping write. An incomplete result or failed upsert emits
`MessageForwardFailed`; it does not blindly repeat a bot send whose remote side
effect may already have succeeded. A message outside configured mappings, a
route disabled for that direction, a loop-suppressed message, or an accepted
deferred media-group item is a successful no-op rather than a
`bridge_not_forwarded` failure. An attempted delivery failure remains explicit
as `bridge_delivery_failed`.

Retry completion and deferred Telegram media-group flush remain specialized
persistence owners because they update retry state or fan one target id out to
multiple source mappings. An inline QQ media-group send is awaited normally
and returns its primary Telegram message id to the actor for the single direct
upsert.

### Message retry operations

When `enable_retry_queue` is `true`, one worker belongs to the active bridge
actor generation. A definitely-not-submitted, retryable QQ-to-Telegram or
Telegram-to-QQ failure is stored in `bridge_message_retry_queue`; the worker
resends through the exact-installation `BotOperationClient`, writes the
source-to-target mapping, and removes the queue row only after both persistence
operations succeed. Pending runnable rows survive process restart and actor
reload. Reload stops the retired generation's worker before post-cutover
ingress can initialize the candidate worker.

DNS/connect, proxy-tunnel, and TLS-handshake failures before HTTP request
writing are definitely not submitted and may be retried. Once request writing
begins, a timeout or disconnect remains possibly submitted. A possibly
submitted send is never automatically retried and creates no fabricated
mapping. The existing retry row is terminalized with its finite
attempt fields; no new outbox or reconciliation table is introduced. A process
crash at the provider boundary still cannot prove exactly-once delivery.
Duplicate enqueue identity contains the complete source message identity and
the exact target installation/platform/conversation. Retry callbacks are
registered by target installation but validate the persisted target
conversation against the configured pair before dispatch, so a removed account
or route is reported as unavailable and never replaced by another bot, group,
or chat. Successful completion writes the same two conversations into the
mapping before removing only that exact retry row.

The defaults are 5 maximum attempts, a 2-second base backoff, a 10-second queue
check interval, and a 300-second maximum backoff. All four values must be
positive; the base and check intervals must not exceed the maximum. Startup,
`--validate-config`, and reload reject invalid values with
`reload_actor_config_invalid` before activating the generation.

Diagnostics distinguish an explicitly disabled queue (`消息发送失败且未启用重试`)
from an enabled but unavailable queue (`消息发送失败且重试队列不可用`). Retry logs
contain platform direction, source identity, and attempt outcome, but not
message bodies, bot tokens, proxy credentials, or complete API responses.

[`actor-config.example.toml`](actor-config.example.toml) lists the bot,
media, pair, and group-mapping options. Named pairs contain one exact Telegram
and OneBot installation; the scalar fields remain the one-pair compatibility
form. Use the actor dependency and database block above as the current runtime
contract.

## Features

- Bidirectional QQ group and Telegram group/topic forwarding
- Cross-platform reply and message-id mapping
- Image, video, audio, document, sticker, and GIF forwarding
- WebM/TGS-to-GIF conversion with size fallbacks
- Persistent retry queues with exponential backoff
- Group-to-group and topic-to-group mappings

## Tests

The repository tests cover reflected actor dispatch, mapping persistence,
retries, message adaptation, database schema, and forwarding failure behavior.
The OBCX cross-repository conformance test additionally installs a clean SDK,
builds and installs bridge plus message-store, dynamically loads both actors,
and verifies the complete pipeline and shutdown path.

## License

MIT
