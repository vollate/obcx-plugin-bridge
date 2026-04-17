#include "qq/event_handler.hpp"
#include "config.hpp"

#include <common/logger.hpp>
#include <core/tg_bot.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace bridge::qq {

QQEventHandler::QQEventHandler(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto QQEventHandler::handle_recall_event(obcx::core::IBot &telegram_bot,
                                         obcx::core::IBot &qq_bot,
                                         obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  try {
    // 尝试转换为NoticeEvent
    auto notice_event = std::get<obcx::common::NoticeEvent>(event);

    // 处理戳一戳事件
    if (notice_event.notice_type == "notify") {
      // 检查是否是poke子类型
      std::string sub_type;
      if (notice_event.data.contains("sub_type")) {
        sub_type = notice_event.data["sub_type"].get<std::string>();
      }

      if (sub_type == "poke") {
        co_await handle_poke_event(telegram_bot, qq_bot, notice_event);
        co_return;
      }
    }

    // 检查是否是撤回事件
    if (notice_event.notice_type != "group_recall") {
      co_return;
    }

    // 确保是群消息撤回且有群ID
    if (!notice_event.group_id.has_value()) {
      PLUGIN_DEBUG("qq_to_tg", "撤回事件缺少群ID");
      co_return;
    }

    const std::string qq_group_id = notice_event.group_id.value();

    // 从事件数据中获取被撤回的消息ID
    std::string recalled_message_id;
    if (notice_event.data.contains("message_id")) {
      // message_id可能是整数或字符串，需要正确处理
      auto message_id_value = notice_event.data["message_id"];
      if (message_id_value.is_string()) {
        recalled_message_id = message_id_value.get<std::string>();
      } else if (message_id_value.is_number()) {
        recalled_message_id = std::to_string(message_id_value.get<int64_t>());
      } else {
        PLUGIN_WARN("qq_to_tg", "撤回事件message_id类型不支持: {}",
                    message_id_value.type_name());
        co_return;
      }
    } else {
      PLUGIN_WARN("qq_to_tg", "撤回事件缺少message_id信息");
      co_return;
    }

    PLUGIN_INFO("qq_to_tg", "处理QQ群 {} 中消息 {} 的撤回事件", qq_group_id,
                recalled_message_id);

    // 根据QQ群ID获取对应的Telegram群ID和topic_id
    auto [telegram_group_id, topic_id] = get_tg_group_and_topic_id(qq_group_id);
    if (telegram_group_id.empty()) {
      PLUGIN_DEBUG("qq_to_tg", "未找到QQ群 {} 对应的Telegram群映射",
                   qq_group_id);
      co_return;
    }

    // 获取bridge配置
    const GroupBridgeConfig *bridge_config =
        get_bridge_config(telegram_group_id);
    if (!bridge_config) {
      PLUGIN_DEBUG("qq_to_tg", "未找到Telegram群 {} 的bridge配置",
                   telegram_group_id);
      co_return;
    }

    // 查找对应的Telegram消息ID
    auto target_message_id = db_manager_->get_target_message_id(
        "qq", recalled_message_id, "telegram");

    if (!target_message_id.has_value()) {
      PLUGIN_DEBUG("qq_to_tg", "未找到QQ消息 {} 对应的Telegram消息映射",
                   recalled_message_id);
      co_return;
    }

    // 获取原始消息内容
    auto original_message = db_manager_->get_message("qq", recalled_message_id);
    std::string message_content = original_message.has_value()
                                      ? original_message->content
                                      : "已撤回的消息";

    // 判断是否需要显示发送者
    bool show_sender = false;
    if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
      show_sender = bridge_config->show_qq_to_tg_sender;
    } else {
      // Topic模式：获取对应topic的配置
      const TopicBridgeConfig *topic_config =
          get_topic_config(telegram_group_id, topic_id);
      show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
    }

    // 创建带删除线的消息内容
    std::string edited_content;
    if (show_sender && original_message.has_value()) {
      std::string sender_display_name = co_await fetch_user_display_name(
          qq_bot, original_message->user_id, qq_group_id);
      // 格式: [用户名]\t~消息内容~
      edited_content = fmt::format(
          "{}~Message has been recalled~",
          escape_markdown_v2(fmt::format("[{}]\t", sender_display_name)));
    } else {
      // 不显示发送者，只显示消息内容
      edited_content = "~Message has been recalled~";
    }

    try {
      // 使用 edit_message_text 编辑消息，添加删除线
      auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(&telegram_bot);
      if (!tg_bot) {
        PLUGIN_ERROR("qq_to_tg", "无法获取TGBot实例");
        co_return;
      }

      auto response = co_await tg_bot->edit_message_text(
          telegram_group_id, target_message_id.value(), edited_content,
          "MarkdownV2");

      // 解析响应
      nlohmann::json response_json = nlohmann::json::parse(response);

      if (response_json.contains("ok") && response_json["ok"].get<bool>()) {
        PLUGIN_INFO("qq_to_tg", "成功编辑Telegram消息为撤回状态: {}:{}",
                    telegram_group_id, target_message_id.value());
      } else {
        PLUGIN_WARN("qq_to_tg", "编辑Telegram消息失败: {}:{}, 响应: {}",
                    telegram_group_id, target_message_id.value(), response);
      }

    } catch (const std::exception &e) {
      PLUGIN_WARN("qq_to_tg", "尝试编辑Telegram消息时出错: {}", e.what());
    }

    // 保留消息映射，不删除（以便后续查询）

  } catch (const std::bad_variant_access &e) {
    PLUGIN_DEBUG("qq_to_tg", "事件不是NoticeEvent类型，跳过撤回处理");
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "处理QQ撤回事件时出错: {}", e.what());
  }
}

auto QQEventHandler::handle_poke_event(obcx::core::IBot &telegram_bot,
                                       obcx::core::IBot &qq_bot,
                                       const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  try {
    // 确保有群ID
    if (!event.group_id.has_value()) {
      PLUGIN_DEBUG("qq_to_tg", "戳一戳事件缺少群ID");
      co_return;
    }

    const std::string qq_group_id = event.group_id.value();

    // 打印完整的事件数据用于调试
    PLUGIN_DEBUG("qq_to_tg", "戳一戳事件数据: {}", event.data.dump());

    // 从事件数据中获取user_id（发起戳一戳的人）和target_id（被戳的人）
    std::string user_id;
    std::string target_id;

    if (event.data.contains("user_id")) {
      auto user_id_value = event.data["user_id"];
      if (user_id_value.is_string()) {
        user_id = user_id_value.get<std::string>();
      } else if (user_id_value.is_number()) {
        user_id = std::to_string(user_id_value.get<int64_t>());
      }
    }

    if (event.data.contains("target_id")) {
      auto target_id_value = event.data["target_id"];
      if (target_id_value.is_string()) {
        target_id = target_id_value.get<std::string>();
      } else if (target_id_value.is_number()) {
        target_id = std::to_string(target_id_value.get<int64_t>());
      }
    }

    if (user_id.empty() || target_id.empty()) {
      PLUGIN_WARN("qq_to_tg", "戳一戳事件缺少user_id或target_id");
      co_return;
    }

    // 尝试获取戳一戳类型 (poke_type/type 和 poke_id/id)
    int poke_type = 1; // 默认为普通戳一戳
    int poke_id = -1;  // 默认 id

    // 尝试多种可能的字段名
    if (event.data.contains("poke_type")) {
      poke_type = event.data["poke_type"].get<int>();
    } else if (event.data.contains("type") && event.data["type"].is_number()) {
      poke_type = event.data["type"].get<int>();
    }

    if (event.data.contains("poke_id")) {
      poke_id = event.data["poke_id"].get<int>();
    } else if (event.data.contains("id") && event.data["id"].is_number()) {
      poke_id = event.data["id"].get<int>();
    }

    // 获取动作名称
    std::string action_name = get_poke_action_name(poke_type, poke_id);

    // 尝试获取 action_text 或 action 字段（某些协议端可能直接提供）
    if (event.data.contains("action_text")) {
      action_name = event.data["action_text"].get<std::string>();
    } else if (event.data.contains("action")) {
      action_name = event.data["action"].get<std::string>();
    }

    // 尝试获取 suffix 字段（如 "的脸"、"的头"）
    std::string suffix;
    if (event.data.contains("suffix")) {
      suffix = event.data["suffix"].get<std::string>();
    }

    PLUGIN_INFO(
        "qq_to_tg",
        "处理QQ群 {} 中的戳一戳事件: {} -> {}, type={}, id={}, action={}",
        qq_group_id, user_id, target_id, poke_type, poke_id, action_name);

    // 根据QQ群ID获取对应的Telegram群ID和topic_id
    auto [telegram_group_id, topic_id] = get_tg_group_and_topic_id(qq_group_id);
    if (telegram_group_id.empty()) {
      PLUGIN_DEBUG("qq_to_tg", "未找到QQ群 {} 对应的Telegram群映射",
                   qq_group_id);
      co_return;
    }

    // 获取bridge配置
    const GroupBridgeConfig *bridge_config =
        get_bridge_config(telegram_group_id);
    if (!bridge_config) {
      PLUGIN_DEBUG("qq_to_tg", "未找到Telegram群 {} 的bridge配置",
                   telegram_group_id);
      co_return;
    }

    // 检查转发是否启用
    bool forward_enabled = false;
    bool display_name = false;
    if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
      forward_enabled = bridge_config->enable_qq_to_tg;
      display_name = bridge_config->show_qq_to_tg_sender;
    } else {
      const TopicBridgeConfig *topic_config =
          get_topic_config(telegram_group_id, topic_id);
      forward_enabled = topic_config ? topic_config->enable_qq_to_tg : false;
      display_name = topic_config ? topic_config->show_qq_to_tg_sender : false;
    }

    if (!forward_enabled) {
      PLUGIN_DEBUG("qq_to_tg", "QQ群 {} 到Telegram的转发未启用", qq_group_id);
      co_return;
    }

    // 获取两个用户的显示名称
    std::string user_display_name = " ", target_display_name = " ";
    if (display_name) {
      user_display_name =
          co_await fetch_user_display_name(qq_bot, user_id, qq_group_id);
      target_display_name =
          co_await fetch_user_display_name(qq_bot, target_id, qq_group_id);
      // 如果显示名称为空，使用QQ号作为后备
      if (user_display_name.empty()) {
        user_display_name = user_id;
      }
      if (target_display_name.empty()) {
        target_display_name = target_id;
      }
    }

    // 构建戳一戳消息
    std::string poke_text;
    if (!suffix.empty()) {
      // 有后缀：[用户A] 戳了戳 [用户B] 的脸
      poke_text = fmt::format("[{}] {} [{}]{}", user_display_name, action_name,
                              target_display_name, suffix);
    } else {
      // 无后缀：[用户A] 戳了戳 [用户B]
      poke_text = fmt::format("[{}] {} [{}]", user_display_name, action_name,
                              target_display_name);
    }

    // 构建Message对象
    obcx::common::Message poke_message;
    obcx::common::MessageSegment text_segment;
    text_segment.type = "text";
    text_segment.data["text"] = poke_text;
    poke_message.push_back(text_segment);

    PLUGIN_INFO("qq_to_tg", "发送戳一戳消息到Telegram: {}", poke_text);

    // 发送到Telegram
    auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(&telegram_bot);
    if (!tg_bot) {
      PLUGIN_ERROR("qq_to_tg", "无法获取TGBot实例");
      co_return;
    }

    try {
      std::string response;
      if (bridge_config->mode == BridgeMode::TOPIC_TO_GROUP && topic_id != 0) {
        // Topic模式
        response = co_await tg_bot->send_topic_message(telegram_group_id,
                                                       topic_id, poke_message);
      } else {
        // 群组模式
        response = co_await tg_bot->send_group_message(telegram_group_id,
                                                       poke_message);
      }

      // 解析响应
      nlohmann::json response_json = nlohmann::json::parse(response);
      if (response_json.contains("ok") && response_json["ok"].get<bool>()) {
        PLUGIN_INFO("qq_to_tg", "成功发送戳一戳消息到Telegram群 {}",
                    telegram_group_id);
      } else {
        PLUGIN_WARN("qq_to_tg", "发送戳一戳消息失败: {}", response);
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR("qq_to_tg", "发送戳一戳消息时出错: {}", e.what());
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "处理戳一戳事件时出错: {}", e.what());
  }
}

auto QQEventHandler::get_poke_action_name(int poke_type, int poke_id)
    -> std::string {
  // 基础戳一戳类型 (id = -1)
  if (poke_id == -1 || poke_id == 0) {
    switch (poke_type) {
    case 1:
      return "戳了戳";
    case 2:
      return "比了个心";
    case 3:
      return "点了个赞";
    case 4:
      return "心碎了";
    case 5:
      return "给了一个666";
    case 6:
      return "放了个大招给";
    default:
      break;
    }
  }

  // SVIP 特殊戳一戳类型 (type = 126)
  if (poke_type == 126) {
    switch (poke_id) {
    case 2001:
      return "抓了一下";
    case 2002:
      return "碎了屏给"; // 碎屏/敲门 共用
    case 2003:
      return "勾引了";
    case 2004:
      return "扔了个手雷给";
    case 2005:
      return "结了个印给";
    case 2006:
      return "使用了召唤术召唤";
    case 2007:
      return "送了朵玫瑰花给";
    case 2009:
      return "让你皮了";
    case 2011:
      return "扔了个宝贝球给";
    default:
      break;
    }
  }

  // 未知类型，返回默认
  return "戳了戳";
}

auto QQEventHandler::escape_markdown_v2(const std::string &text)
    -> std::string {
  std::string result;
  result.reserve(text.size() * 2);
  for (char c : text) {
    // MarkdownV2需要转义的特殊字符
    if (c == '_' || c == '*' || c == '[' || c == ']' || c == '(' || c == ')' ||
        c == '~' || c == '`' || c == '>' || c == '#' || c == '+' || c == '-' ||
        c == '=' || c == '|' || c == '{' || c == '}' || c == '.' || c == '!') {
      result += '\\';
    }
    result += c;
  }
  return result;
}

auto QQEventHandler::fetch_user_display_name(obcx::core::IBot &qq_bot,
                                             const std::string &user_id,
                                             const std::string &group_id)
    -> boost::asio::awaitable<std::string> {

  auto display_name =
      db_manager_->query_user_display_name("qq", user_id, group_id);

  if (!display_name.has_value()) {
    PLUGIN_DEBUG("bridge_qq",
                 "Fetch userinfo for platform: qq, group: {}, id: {}", group_id,
                 user_id);
    co_await fetch_user_info(qq_bot, user_id, group_id);
    display_name =
        db_manager_->query_user_display_name("qq", user_id, group_id);
  }

  co_return display_name.value_or(user_id);
}

auto QQEventHandler::fetch_user_info(obcx::core::IBot &qq_bot,
                                     const std::string &user_id,
                                     const std::string &group_id)
    -> boost::asio::awaitable<void> {
  try {
    // 获取群成员信息
    std::string response =
        co_await qq_bot.get_group_member_info(group_id, user_id, false);
    nlohmann::json response_json = nlohmann::json::parse(response);

    PLUGIN_DEBUG("bridge_qq", "QQ群成员信息API响应: {}", response);

    if (response_json.contains("status") && response_json["status"] == "ok" &&
        response_json.contains("data") && response_json["data"].is_object()) {

      auto data = response_json["data"];
      PLUGIN_DEBUG("bridge_qq", "QQ群成员信息详细数据: {}", data.dump());

      storage::UserInfo user_info;
      user_info.platform = "qq";
      user_info.user_id = user_id;
      user_info.group_id = group_id;
      user_info.last_updated = std::chrono::system_clock::now();

      std::string general_nickname, card, title;

      if (data.contains("nickname") && data["nickname"].is_string()) {
        general_nickname = data["nickname"];
      }

      if (data.contains("card") && data["card"].is_string()) {
        card = data["card"];
      }

      if (data.contains("title") && data["title"].is_string()) {
        title = data["title"];
      }

      // 优先级：群名片 > 群头衔 > 一般昵称
      if (!card.empty()) {
        user_info.nickname = card;
        PLUGIN_DEBUG("qq_to_tg", "使用QQ群名片作为显示名称: {} -> {}", user_id,
                     card);
      } else if (!title.empty()) {
        user_info.nickname = title;
        PLUGIN_DEBUG("qq_to_tg", "使用QQ群头衔作为显示名称: {} -> {}", user_id,
                     title);
      } else if (!general_nickname.empty()) {
        user_info.nickname = general_nickname;
        PLUGIN_DEBUG("qq_to_tg", "使用QQ一般昵称作为显示名称: {} -> {}",
                     user_id, general_nickname);
      }

      // 同时保存群头衔到title字段
      if (!title.empty()) {
        user_info.title = title;
      }

      // 保存用户信息
      if (db_manager_->save_or_update_user(user_info, true)) {
        PLUGIN_DEBUG("qq_to_tg", "获取QQ用户信息成功：{} -> {}", user_id,
                     user_info.nickname);
      } else {
        PLUGIN_WARN("qq_to_tg", "保存QQ用户信息失败：{} -> {}", user_id,
                    user_info.nickname);
      }
    }
  } catch (const std::exception &e) {
    PLUGIN_DEBUG("qq_to_tg", "获取QQ用户信息失败：{}", e.what());
  }
}

} // namespace bridge::qq
