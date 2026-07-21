#include "qq/event_handler.hpp"
#include "bridge_state_repository.hpp"
#include "config.hpp"
#include "received_message_repository.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>
#include <interfaces/telegram_bot.hpp>
#include <nlohmann/json.hpp>

namespace bridge::qq {
namespace {

auto is_picture_message(const storage::MessageInfo &message) -> bool {
  if (message.message_type == "image") {
    return true;
  }

  try {
    const auto raw_message = nlohmann::json::parse(message.raw_message);
    if (!raw_message.contains("message") ||
        !raw_message["message"].is_array()) {
      return false;
    }

    for (const auto &segment : raw_message["message"]) {
      if (segment.contains("type") && segment["type"].is_string() &&
          segment["type"].get<std::string>() == "image") {
        return true;
      }
    }
  } catch (const std::exception &e) {
    OBCX_WARN("解析原始QQ消息判断图片类型失败: {}", e.what());
  }

  return false;
}

} // namespace

QQEventHandler::QQEventHandler(
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<bridge::ReceivedMessageRepository>
        received_message_repository)
    : config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)) {}

auto QQEventHandler::handle_recall_event(obcx::core::IBot &telegram_bot,
                                         obcx::core::IBot &qq_bot,
                                         obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  try {
    auto notice_event = std::get<obcx::common::NoticeEvent>(event);

    if (notice_event.notice_type == "notify") {
      std::string sub_type;
      if (notice_event.data.contains("sub_type")) {
        sub_type = notice_event.data["sub_type"].get<std::string>();
      }

      if (sub_type == "poke") {
        co_await handle_poke_event(telegram_bot, qq_bot, notice_event);
        co_return;
      }
    }

    if (notice_event.notice_type != "group_recall") {
      co_return;
    }

    if (!notice_event.group_id.has_value()) {
      OBCX_DEBUG("撤回事件缺少群ID");
      co_return;
    }

    const std::string qq_group_id = notice_event.group_id.value();

    std::string recalled_message_id;
    if (notice_event.data.contains("message_id")) {
      auto message_id_value = notice_event.data["message_id"];
      if (message_id_value.is_string()) {
        recalled_message_id = message_id_value.get<std::string>();
      } else if (message_id_value.is_number()) {
        recalled_message_id = std::to_string(message_id_value.get<int64_t>());
      } else {
        OBCX_WARN("撤回事件message_id类型不支持: {}",
                  message_id_value.type_name());
        co_return;
      }
    } else {
      OBCX_WARN("撤回事件缺少message_id信息");
      co_return;
    }

    OBCX_INFO("处理QQ群 {} 中消息 {} 的撤回事件", qq_group_id,
              recalled_message_id);

    auto [telegram_group_id, topic_id] =
        config_->tg_group_and_topic_id(qq_group_id);
    if (telegram_group_id.empty()) {
      OBCX_DEBUG("未找到QQ群 {} 对应的Telegram群映射", qq_group_id);
      co_return;
    }

    const GroupBridgeConfig *bridge_config =
        config_->bridge_config(telegram_group_id);
    if (!bridge_config) {
      OBCX_DEBUG("未找到Telegram群 {} 的bridge配置", telegram_group_id);
      co_return;
    }

    auto target_message_id = state_repository_
                                 ? state_repository_->get_target_message_id(
                                       "qq", recalled_message_id, "telegram")
                                 : std::optional<std::string>{};

    if (!target_message_id.has_value()) {
      OBCX_DEBUG("未找到QQ消息 {} 对应的Telegram消息映射", recalled_message_id);
      co_return;
    }

    auto original_message = received_message_repository_
                                ? received_message_repository_->get_message(
                                      "qq", recalled_message_id)
                                : std::optional<storage::MessageInfo>{};
    const bool should_delete_tg_message =
        original_message.has_value() && is_picture_message(*original_message);

    try {
      auto *tg_bot = dynamic_cast<obcx::core::ITelegramBot *>(&telegram_bot);
      if (!tg_bot) {
        OBCX_ERROR("当前 bot 不支持 Telegram 扩展能力");
        co_return;
      }

      if (should_delete_tg_message) {
        const std::string tg_message_ref =
            fmt::format("{}:{}", telegram_group_id, target_message_id.value());
        auto response = co_await telegram_bot.delete_message(tg_message_ref);

        nlohmann::json response_json = nlohmann::json::parse(response);
        if (response_json.contains("ok") && response_json["ok"].get<bool>()) {
          OBCX_INFO("成功删除已撤回QQ图片对应的Telegram消息: {}:{}",
                    telegram_group_id, target_message_id.value());
        } else {
          OBCX_WARN("删除Telegram图片消息失败: {}:{}, 响应: {}",
                    telegram_group_id, target_message_id.value(), response);
        }

        co_return;
      }

      bool show_sender = false;
      if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
        show_sender = bridge_config->show_qq_to_tg_sender;
      } else {
        const TopicBridgeConfig *topic_config =
            config_->topic_config(telegram_group_id, topic_id);
        show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
      }

      std::string edited_content;
      if (show_sender && original_message.has_value()) {
        std::string sender_display_name = co_await fetch_user_display_name(
            qq_bot, original_message->user_id, qq_group_id);
        edited_content = fmt::format(
            "{}~Message has been recalled~",
            escape_markdown_v2(fmt::format("[{}]\t", sender_display_name)));
      } else {
        edited_content = "~Message has been recalled~";
      }

      auto response = co_await tg_bot->edit_message_text(
          telegram_group_id, target_message_id.value(), edited_content,
          "MarkdownV2");

      nlohmann::json response_json = nlohmann::json::parse(response);

      if (response_json.contains("ok") && response_json["ok"].get<bool>()) {
        OBCX_INFO("成功编辑Telegram消息为撤回状态: {}:{}", telegram_group_id,
                  target_message_id.value());
      } else {
        OBCX_WARN("编辑Telegram消息失败: {}:{}, 响应: {}", telegram_group_id,
                  target_message_id.value(), response);
      }

    } catch (const std::exception &e) {
      OBCX_WARN("尝试编辑Telegram消息时出错: {}", e.what());
    }

  } catch (const std::bad_variant_access &e) {
    OBCX_DEBUG("事件不是NoticeEvent类型，跳过撤回处理");
  } catch (const std::exception &e) {
    OBCX_ERROR("处理QQ撤回事件时出错: {}", e.what());
  }
}

auto QQEventHandler::handle_poke_event(obcx::core::IBot &telegram_bot,
                                       obcx::core::IBot &qq_bot,
                                       const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  try {
    if (!event.group_id.has_value()) {
      OBCX_DEBUG("戳一戳事件缺少群ID");
      co_return;
    }

    const std::string qq_group_id = event.group_id.value();

    OBCX_DEBUG("戳一戳事件数据: {}", event.data.dump());

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
      OBCX_WARN("戳一戳事件缺少user_id或target_id");
      co_return;
    }

    int poke_type = 1;
    int poke_id = -1;

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

    std::string action_name = get_poke_action_name(poke_type, poke_id);

    if (event.data.contains("action_text")) {
      action_name = event.data["action_text"].get<std::string>();
    } else if (event.data.contains("action")) {
      action_name = event.data["action"].get<std::string>();
    }

    std::string suffix;
    if (event.data.contains("suffix")) {
      suffix = event.data["suffix"].get<std::string>();
    }

    OBCX_INFO("处理QQ群 {} 中的戳一戳事件: {} -> {}, type={}, id={}, action={}",
              qq_group_id, user_id, target_id, poke_type, poke_id, action_name);

    auto [telegram_group_id, topic_id] =
        config_->tg_group_and_topic_id(qq_group_id);
    if (telegram_group_id.empty()) {
      OBCX_DEBUG("未找到QQ群 {} 对应的Telegram群映射", qq_group_id);
      co_return;
    }

    const GroupBridgeConfig *bridge_config =
        config_->bridge_config(telegram_group_id);
    if (!bridge_config) {
      OBCX_DEBUG("未找到Telegram群 {} 的bridge配置", telegram_group_id);
      co_return;
    }

    bool forward_enabled = false;
    bool display_name = false;
    if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
      forward_enabled = bridge_config->enable_qq_to_tg;
      display_name = bridge_config->show_qq_to_tg_sender;
    } else {
      const TopicBridgeConfig *topic_config =
          config_->topic_config(telegram_group_id, topic_id);
      forward_enabled = topic_config ? topic_config->enable_qq_to_tg : false;
      display_name = topic_config ? topic_config->show_qq_to_tg_sender : false;
    }

    if (!forward_enabled) {
      OBCX_DEBUG("QQ群 {} 到Telegram的转发未启用", qq_group_id);
      co_return;
    }

    std::string user_display_name = " ", target_display_name = " ";
    if (display_name) {
      user_display_name =
          co_await fetch_user_display_name(qq_bot, user_id, qq_group_id);
      target_display_name =
          co_await fetch_user_display_name(qq_bot, target_id, qq_group_id);
      if (user_display_name.empty()) {
        user_display_name = user_id;
      }
      if (target_display_name.empty()) {
        target_display_name = target_id;
      }
    }

    std::string poke_text;
    if (!suffix.empty()) {
      poke_text = fmt::format("[{}] {} [{}]{}", user_display_name, action_name,
                              target_display_name, suffix);
    } else {
      poke_text = fmt::format("[{}] {} [{}]", user_display_name, action_name,
                              target_display_name);
    }

    obcx::common::Message poke_message;
    obcx::common::MessageSegment text_segment;
    text_segment.type = "text";
    text_segment.data["text"] = poke_text;
    poke_message.push_back(text_segment);

    OBCX_INFO("发送戳一戳消息到Telegram: {}", poke_text);

    auto *tg_bot = dynamic_cast<obcx::core::ITelegramBot *>(&telegram_bot);
    if (!tg_bot) {
      OBCX_ERROR("当前 bot 不支持 Telegram 扩展能力");
      co_return;
    }

    try {
      std::string response;
      if (bridge_config->mode == BridgeMode::TOPIC_TO_GROUP && topic_id != 0) {
        response = co_await tg_bot->send_topic_message(telegram_group_id,
                                                       topic_id, poke_message);
      } else {
        response = co_await telegram_bot.send_group_message(telegram_group_id,
                                                            poke_message);
      }

      nlohmann::json response_json = nlohmann::json::parse(response);
      if (response_json.contains("ok") && response_json["ok"].get<bool>()) {
        OBCX_INFO("成功发送戳一戳消息到Telegram群 {}", telegram_group_id);
      } else {
        OBCX_WARN("发送戳一戳消息失败: {}", response);
      }
    } catch (const std::exception &e) {
      OBCX_ERROR("发送戳一戳消息时出错: {}", e.what());
    }

  } catch (const std::exception &e) {
    OBCX_ERROR("处理戳一戳事件时出错: {}", e.what());
  }
}

auto QQEventHandler::get_poke_action_name(int poke_type, int poke_id)
    -> std::string {
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

  if (poke_type == 126) {
    switch (poke_id) {
    case 2001:
      return "抓了一下";
    case 2002:
      return "碎了屏给";
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

  return "戳了戳";
}

auto QQEventHandler::escape_markdown_v2(const std::string &text)
    -> std::string {
  std::string result;
  result.reserve(text.size() * 2);
  for (char c : text) {
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
      state_repository_
          ? state_repository_->query_user_display_name("qq", user_id, group_id)
          : std::optional<std::string>{};

  if (!display_name.has_value()) {
    OBCX_DEBUG("Fetch userinfo for platform: qq, group: {}, id: {}", group_id,
               user_id);
    co_await fetch_user_info(qq_bot, user_id, group_id);
    display_name = state_repository_
                       ? state_repository_->query_user_display_name(
                             "qq", user_id, group_id)
                       : std::optional<std::string>{};
  }

  co_return display_name.value_or(user_id);
}

auto QQEventHandler::fetch_user_info(obcx::core::IBot &qq_bot,
                                     const std::string &user_id,
                                     const std::string &group_id)
    -> boost::asio::awaitable<void> {
  try {
    std::string response =
        co_await qq_bot.get_group_member_info(group_id, user_id, false);
    nlohmann::json response_json = nlohmann::json::parse(response);

    OBCX_DEBUG("QQ群成员信息API响应: {}", response);

    if (response_json.contains("status") && response_json["status"] == "ok" &&
        response_json.contains("data") && response_json["data"].is_object()) {

      auto data = response_json["data"];
      OBCX_DEBUG("QQ群成员信息详细数据: {}", data.dump());

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

      if (!card.empty()) {
        user_info.nickname = card;
        OBCX_DEBUG("使用QQ群名片作为显示名称: {} -> {}", user_id, card);
      } else if (!title.empty()) {
        user_info.nickname = title;
        OBCX_DEBUG("使用QQ群头衔作为显示名称: {} -> {}", user_id, title);
      } else if (!general_nickname.empty()) {
        user_info.nickname = general_nickname;
        OBCX_DEBUG("使用QQ一般昵称作为显示名称: {} -> {}", user_id,
                   general_nickname);
      }

      if (!title.empty()) {
        user_info.title = title;
      }

      if (state_repository_ &&
          state_repository_->save_or_update_user(user_info, true)) {
        OBCX_DEBUG("获取QQ用户信息成功：{} -> {}", user_id, user_info.nickname);
      } else {
        OBCX_WARN("保存QQ用户信息失败：{} -> {}", user_id, user_info.nickname);
      }
    }
  } catch (const std::exception &e) {
    OBCX_DEBUG("获取QQ用户信息失败：{}", e.what());
  }
}

} // namespace bridge::qq
