# Bridge Bot 插件

## Telegram Topic 消息处理重要注意事项

- **关键概念**：在 Telegram Group 中，如果消息包含 `message_thread_id`，说明这是 Topic 消息
- **回复判断逻辑**：
  - 当 `reply_to_message.message_id == message_thread_id` 时，表示消息是发送到 Topic 中，**不是回复**
  - 当 `reply_to_message.message_id != message_thread_id` 时，才是真正的回复其他消息
- **核心代码逻辑**：`has_genuine_reply = (reply_msg_id != thread_id)`
- **重构时必须保持**：这个逻辑在任何重构或修改中都必须严格保持，不能改变
- **测试验证**：修改 Topic 相关逻辑后，必须验证回复消息的正确识别和处理

## 消息回复跨平台映射逻辑重要注意事项

- **核心原理**：所有回复消息都需要转发，关键是要正确处理四种回复情况的消息 ID 映射，确保转发后的消息能够引用正确的对应平台的消息 ID

- **四种回复情况及处理逻辑**：

  1. **TG 回复 TG 原生消息** → 转发到 QQ 时：
     - 先查找该 TG 消息是否曾转发到 QQ 过 (`get_target_message_id("telegram", 被回复TG消息ID, "qq")`)
     - 如果找到 QQ 消息 ID，在转发时引用该 QQ 消息；如果没找到，显示回复提示

  2. **TG 回复 QQ 转发消息** → 转发到 QQ 时：
     - 查找该 TG 消息是否来源于 QQ (`get_source_message_id("telegram", 被回复TG消息ID, "qq")`)
     - 如果找到 QQ 原始消息 ID，在转发时引用该 QQ 消息 ID

  3. **QQ 回复 QQ 原生消息** → 转发到 TG 时：
     - 先查找该 QQ 消息是否曾转发到 TG 过 (`get_target_message_id("qq", 被回复QQ消息ID, "telegram")`)
     - 如果找到 TG 消息 ID，在转发时引用该 TG 消息；如果没找到，显示回复提示

  4. **QQ 回复 TG 转发消息** → 转发到 TG 时：
     - 查找该 QQ 消息是否来源于 TG (`get_source_message_id("qq", 被回复QQ消息ID, "telegram")`)
     - 如果找到 TG 原始消息 ID，在转发时引用该 TG 消息 ID

- **数据库查询顺序**：
  - 先查 `get_target_message_id()` — 查找消息是否已转发到目标平台
  - 再查 `get_source_message_id()` — 查找消息是否来源于目标平台
  - 这个顺序确保正确处理所有四种回复情况

- **字段名统一要求**：所有 reply segment 都必须使用 `data["id"]` 字段存储消息 ID，不能使用 `message_id` 或其他字段名

- **重要提醒**：这四种情况涵盖了所有可能的回复场景，修改相关逻辑时必须确保四种情况都能正确处理，实现真正的跨平台回复体验

## DatabaseManager 单例模式

`DatabaseManager`（`dependency/database/database_manager.hpp`）使用单例模式，多个插件共享同一实例：

```cpp
// 获取单例（首次调用需要提供 db_path）
db_manager_ = DatabaseManager::instance(config_.database_file);

// 重置单例（在所有插件都 unload 后调用）
DatabaseManager::reset_instance();
```

**注意**：插件的 `shutdown()` 中不要 `reset()` db_manager_，只需置空指针。单例的生命周期由框架管理。

## 注释风格（Comment style）

**核心原则**：好的代码自我说明，注释只在逻辑足够复杂时才加。

- **保留**：
  - 头文件（`.hpp`）中函数声明前的 doxygen 注释（`@brief / @param / @return`），用于文档化对外用法
  - 解释 **WHY** 的注释：非显而易见的取舍、协议/数据怪癖、生命周期/线程注意事项、踩过的坑
  - `TODO` / `FIXME` / `HACK` / `NOTE` 标记
  - License / 版权头
- **删除**：
  - 函数体内只是把下一行代码翻译一遍的注释（中文/英文都算）
  - `// =====` / `// ----` 等装饰性 section 分隔条
  - 仅描述「本文件包含 xxx」的文件顶部 banner（文件名已经说了）
  - `// 构造函数` / `// 析构函数` 这类对 ctor / dtor 的废话
  - 内部 `static` / 匿名 namespace 辅助函数上的整块 doxygen — 必要时换成一行 `//` WHY 注释，否则直接删掉
  - 注释掉的旧代码块
- **修改时**：当一个注释同时包含「重述代码」和「真正的 WHY」时，只保留 WHY 那部分；不要整段删掉

