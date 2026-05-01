# OBCX Bridge Plugin

QQ 与 Telegram 双向消息桥接插件，基于 [OBCX](https://github.com/Onebot-CXX/obcx) 框架。

## 功能

- **双向消息转发**：QQ 群 <-> Telegram 群/Topic
- **跨平台回复**：回复消息自动映射到对应平台的原始消息
- **媒体文件转发**：图片、视频、音频、文档、贴纸、GIF 动画
- **贴纸转换**：Telegram WebM/TGS 贴纸自动转换为 GIF，支持多级压缩和文件大小限制
- **消息重试队列**：发送失败的消息自动重试，指数退避
- **两种桥接模式**：
  - `group_to_group` — QQ 群对应一个 Telegram 群
  - `topic_to_group` — QQ 群对应一个 Telegram Topic

## 依赖

- OBCX 框架（obcx_core）
- SQLite3
- toml++
- ffmpeg（运行时，用于 WebM -> GIF 转换）
- lottie_convert.py（可选，用于 TGS -> GIF 转换）

## 编译

插件作为 OBCX 的本地插件编译：

```bash
# 在 OBCX 根目录
cmake -B build -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

编译产物：
- `build/plugins/qq_to_tg.so` — QQ 到 Telegram 转发插件
- `build/plugins/tg_to_qq.so` — Telegram 到 QQ 转发插件

## 配置

参考 `example_plug_config.toml`，完整配置说明如下。

### Bot 配置

```toml
[bots.qq_bot]
type = "qq"
enabled = true
plugins = ["qq_to_tg"]

[bots.qq_bot.connection]
type = "websocket"
host = "127.0.0.1"
port = 3001
access_token = ""
use_ssl = false
connect_timeout = 5000      # TCP 建连超时
action_timeout = 30000      # OneBot action 响应超时（见"超时字段速查"）
heartbeat_interval = 5000

[bots.telegram_bot]
type = "telegram"
enabled = true
plugins = ["tg_to_qq"]

[bots.telegram_bot.connection]
type = "http"
host = "api.telegram.org"
port = 443
access_token = "YOUR_BOT_TOKEN"
use_ssl = true
connect_timeout = 5000      # HTTP 单次请求超时
poll_timeout = 25000        # 长轮询服务端侧超时
poll_force_close = 30000    # 长轮询客户端强制关闭（> poll_timeout）
poll_retry_interval = 3000  # 轮询重试退避
proxy_host = "127.0.0.1"    # 可选
proxy_port = 10086           # 可选
proxy_type = "http"          # 可选
```

### 超时字段速查

> 本节说明 bridge 插件实际使用的所有超时配置。框架侧的完整定义请看根目录 `README.md` 的"超时参数"章节。

| 字段 | 所在块 | 默认 | 用途 |
| --- | --- | --- | --- |
| `connect_timeout` | `[bots.*.connection]` | 5000 ms | TCP / HTTP 单次请求的底层超时 |
| `action_timeout` | `[bots.qq_bot.connection]` | 30000 ms | OneBot11 WebSocket 等待 action（例如 `send_group_msg`）echo 响应的超时。过短会在 llonebot 首次发送媒体时（通常 8–10 s）触发 retry queue，导致 QQ 群里出现重复消息 |
| `poll_timeout` | `[bots.telegram_bot.connection]` | 25000 ms | 发给 Telegram `getUpdates` 的服务端长轮询超时 |
| `poll_force_close` | `[bots.telegram_bot.connection]` | 30000 ms | 客户端强制关闭长轮询连接的安全超时，**必须大于 `poll_timeout`** |
| `poll_retry_interval` | `[bots.telegram_bot.connection]` | 3000 ms | 轮询失败后的退避间隔 |
| `heartbeat_interval` | `[bots.*.connection]` | 30000 ms | 心跳间隔 |

#### 插件内部的重试节奏（硬编码，暂不支持 TOML）

定义于 `include/retry_queue_manager.hpp`：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `DEFAULT_MESSAGE_RETRY_INTERVAL_SECONDS` | 2 s | 文本/普通消息首次重试间隔 |
| `DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS` | 5 s | 媒体消息首次重试间隔 |
| `MAX_RETRY_INTERVAL_SECONDS` | 300 s | 指数退避上限 |
| `MESSAGE_RETRY_MAX_ATTEMPTS` | 5 | 文本消息最大重试次数（`config.cpp`） |
| `MEDIA_RETRY_MAX_ATTEMPTS` | 3 | 媒体消息最大重试次数（`config.cpp`） |
| `RETRY_QUEUE_CHECK_INTERVAL_SEC` | 10 s | retry queue 扫描节拍 |

退避公式：`next_interval = min(2^retry_count × base, MAX_RETRY_INTERVAL_SECONDS)`。

> **重要**：`action_timeout` 过短会和 retry queue 互相作用放大错误——action 请求超时触发重试，但原请求往往已经在 llonebot 侧成功，出现双发。实测 llonebot 第一次上传某张图/贴纸时的响应延迟约 8 s，因此 `action_timeout` 建议保持在 **15 s 以上**（默认 30 s 是安全值）。

### 插件配置

```toml
[plugins.qq_to_tg]
enabled = true
callbacks = ["on_message", "on_notice"]
priority = 100

[plugins.qq_to_tg.config]
database_file = "bridge_bot.db"
enable_retry_queue = true

[plugins.tg_to_qq]
enabled = true
callbacks = ["on_message", "on_notice"]
priority = 100

[plugins.tg_to_qq.config]
database_file = "bridge_bot.db"
enable_retry_queue = true
```

### GIF 转换配置

在 `[plugins.tg_to_qq.config]` 中配置（所有字段可选）：

```toml
[plugins.tg_to_qq.config]
gif_max_file_size = 0        # 最大输出文件大小，单位字节，0 = 不限制（默认 0）
gif_max_duration = 5         # 最大动画时长，单位秒（默认 5）
gif_max_fps = 0              # 最大帧率，0 = 不限（默认 0）
gif_max_width = 0            # 最大宽度，0 = 保持原始分辨率（默认 0）
gif_max_colors = 256         # 最大调色板颜色数，范围 2-256（默认 256）
```

转换时使用多级回退策略，逐步增加压缩力度直到输出文件符合大小限制：

| 级别 | 宽度 | 帧率 | 颜色数 | 说明 |
|------|------|------|--------|------|
| quality | 原始 | 原始 | 256 | 最高质量 |
| balanced | 320px | 原始 | 256 | 与旧版压缩回退一致 |
| compact | 200px | 8fps | 64 | 压缩 |
| minimal | 160px | 5fps | 32 | 最大压缩 |

### 群组映射

#### 群组对群组模式

```toml
[[group_mappings.group_to_group]]
telegram_group_id = "YOUR_TG_GROUP_ID"
qq_group_id = "YOUR_QQ_GROUP_ID"
show_qq_to_tg_sender = true    # QQ -> TG 消息显示发送者名称
show_tg_to_qq_sender = true    # TG -> QQ 消息显示发送者名称
enable_qq_to_tg = true         # 启用 QQ -> TG 转发
enable_tg_to_qq = true         # 启用 TG -> QQ 转发
```

#### Topic 对群组模式

```toml
[[group_mappings.topic_to_group]]
telegram_group_id = "YOUR_TG_GROUP_ID"
show_qq_to_tg_sender = true
show_tg_to_qq_sender = true

[[group_mappings.topics]]
telegram_group_id = "YOUR_TG_GROUP_ID"   # 所属 Telegram 群
telegram_topic_id = 33                    # Topic ID
qq_group_id = "YOUR_QQ_GROUP_ID"
show_qq_to_tg_sender = true
show_tg_to_qq_sender = false
```

一个 Telegram 群可以配置多个 Topic，每个 Topic 映射到不同的 QQ 群。

## 项目结构

```
obcx-plugin-bridge/
├── include/                    # 头文件
│   ├── config.hpp              # 配置结构与加载
│   ├── media_converter.hpp     # 媒体格式转换（WebM/TGS -> GIF）
│   ├── media_processor.hpp     # 通用媒体处理
│   ├── path_manager.hpp        # 主机/容器路径映射
│   ├── retry_queue_manager.hpp # 消息重试队列
│   ├── database/               # 数据库管理
│   ├── qq/                     # QQ 侧处理器
│   └── telegram/               # Telegram 侧处理器
├── dependency/                 # 实现代码 -> libbridge_core.so
│   ├── config.cpp
│   ├── media_converter.cpp
│   ├── media_processor.cpp
│   ├── path_manager.cpp
│   ├── retry_queue_manager.cpp
│   ├── database/               # SQLite 数据库操作
│   ├── qq/                     # QQ 消息处理、格式化、事件
│   └── telegram/               # Telegram 消息处理、格式化、事件
├── qq_to_tg/                   # QQ -> TG 插件入口
├── tg_to_qq/                   # TG -> QQ 插件入口
├── tests/                      # 测试
├── example_plug_config.toml    # 配置示例
└── plugin.toml                 # 插件元信息
```

## 许可证

MIT
