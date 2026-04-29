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
timeout = 30000
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
timeout = 30000
proxy_host = "127.0.0.1"    # 可选
proxy_port = 10086           # 可选
proxy_type = "http"          # 可选
```

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
gif_max_file_size = 5242880  # 最大输出文件大小，单位字节（默认 5MB）
gif_max_duration = 5         # 最大动画时长，单位秒（默认 5）
gif_max_fps = 15             # 最大帧率，0 = 不限（默认 15）
gif_max_width = 0            # 最大宽度，0 = 保持原始分辨率（默认 0）
gif_max_colors = 256         # 最大调色板颜色数，范围 2-256（默认 256）
```

转换时使用多级回退策略，逐步增加压缩力度直到输出文件符合大小限制：

| 级别 | 宽度 | 帧率 | 颜色数 | 说明 |
|------|------|------|--------|------|
| quality | 原始 | 15fps | 256 | 最高质量 |
| balanced | 320px | 10fps | 128 | 平衡 |
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
