#include "qq/event_handler.hpp"

#include "bridge_state_repository.hpp"
#include "config.hpp"
#include "received_message_repository.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace bridge::qq {
namespace {

auto is_picture_message(const storage::MessageInfo &message) -> bool {
  if (message.message_type == "image") {
    return true;
  }
  try {
    const auto raw = nlohmann::json::parse(message.raw_message);
    if (!raw.contains("message") || !raw["message"].is_array()) {
      return false;
    }
    return std::ranges::any_of(raw["message"], [](const auto &segment) {
      return segment.is_object() && segment.value("type", "") == "image";
    });
  } catch (...) {
    return false;
  }
}

auto json_id(const nlohmann::json &data, const std::string_view key)
    -> std::string {
  const auto field = std::string{key};
  if (!data.contains(field)) {
    return {};
  }
  const auto &value = data.at(field);
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<std::int64_t>());
  }
  return {};
}

} // namespace

QQEventHandler::QQEventHandler(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<bridge::ReceivedMessageRepository>
        received_message_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : operations_(std::move(operations)), config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      blocking_executor_(std::move(blocking_executor)) {
  if (!operations_) {
    throw std::invalid_argument("QQEventHandler requires bot operations");
  }
}

auto QQEventHandler::handle_recall_event(obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  try {
    const auto notice = std::get<obcx::common::NoticeEvent>(event);
    if (notice.notice_type == "notify" &&
        notice.data.value("sub_type", "") == "poke") {
      co_await handle_poke_event(notice);
      co_return;
    }
    if (notice.notice_type != "group_recall" || !notice.group_id.has_value()) {
      co_return;
    }
    const auto qq_group_id = *notice.group_id;
    const auto recalled_id = json_id(notice.data, "message_id");
    if (recalled_id.empty()) {
      co_return;
    }
    const auto [telegram_group_id, topic_id] =
        config_->tg_group_and_topic_id(qq_group_id);
    const auto *route = config_->bridge_config(telegram_group_id);
    if (telegram_group_id.empty() || route == nullptr) {
      co_return;
    }

    std::optional<std::string> target_id;
    std::optional<storage::MessageInfo> original;
    if (state_repository_ || received_message_repository_) {
      std::tie(target_id, original) = co_await blocking_executor_->run(
          [state = state_repository_, received = received_message_repository_,
           recalled_id] {
            return std::pair{state ? state->get_target_message_id(
                                         "qq", recalled_id, "telegram")
                                   : std::optional<std::string>{},
                             received ? received->get_message("qq", recalled_id)
                                      : std::optional<storage::MessageInfo>{}};
          });
    }
    if (!target_id.has_value()) {
      co_return;
    }

    try {
      if (original.has_value() && is_picture_message(*original)) {
        co_await operations_->delete_telegram_message(telegram_group_id,
                                                      *target_id);
        co_return;
      }
      bool show_sender = route->show_qq_to_tg_sender;
      if (route->mode == BridgeMode::TOPIC_TO_GROUP) {
        const auto *topic = config_->topic_config(telegram_group_id, topic_id);
        show_sender = topic != nullptr && topic->show_qq_to_tg_sender;
      }
      std::string text = "~Message has been recalled~";
      if (show_sender && original.has_value()) {
        const auto sender =
            co_await fetch_user_display_name(original->user_id, qq_group_id);
        text = escape_markdown_v2(fmt::format("[{}]\t", sender)) + text;
      }
      co_await operations_->edit_telegram_message(telegram_group_id, *target_id,
                                                  text, "MarkdownV2");
    } catch (const std::exception &error) {
      OBCX_WARN("QQ recall propagation failed: {}", error.what());
    }
  } catch (const std::bad_variant_access &) {
    OBCX_DEBUG("QQ recall handler received a non-notice event");
  } catch (const std::exception &error) {
    OBCX_ERROR("处理QQ撤回事件时出错: {}", error.what());
  }
}

auto QQEventHandler::handle_poke_event(const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  try {
    if (!event.group_id.has_value()) {
      co_return;
    }
    const auto qq_group_id = *event.group_id;
    const auto user_id = json_id(event.data, "user_id");
    const auto target_id = json_id(event.data, "target_id");
    if (user_id.empty() || target_id.empty()) {
      co_return;
    }
    const int poke_type =
        event.data.value("poke_type", event.data.value("type", 1));
    const int poke_id = event.data.value("poke_id", event.data.value("id", -1));
    auto action = get_poke_action_name(poke_type, poke_id);
    if (event.data.contains("action_text") &&
        event.data["action_text"].is_string()) {
      action = event.data["action_text"].get<std::string>();
    } else if (event.data.contains("action") &&
               event.data["action"].is_string()) {
      action = event.data["action"].get<std::string>();
    }
    const auto suffix = event.data.value("suffix", std::string{});

    const auto [telegram_group_id, topic_id] =
        config_->tg_group_and_topic_id(qq_group_id);
    const auto *route = config_->bridge_config(telegram_group_id);
    if (telegram_group_id.empty() || route == nullptr) {
      co_return;
    }
    bool enabled = route->enable_qq_to_tg;
    bool show_names = route->show_qq_to_tg_sender;
    if (route->mode == BridgeMode::TOPIC_TO_GROUP) {
      const auto *topic = config_->topic_config(telegram_group_id, topic_id);
      enabled = topic != nullptr && topic->enable_qq_to_tg;
      show_names = topic != nullptr && topic->show_qq_to_tg_sender;
    }
    if (!enabled) {
      co_return;
    }

    std::string user = " ";
    std::string target = " ";
    if (show_names) {
      user = co_await fetch_user_display_name(user_id, qq_group_id);
      target = co_await fetch_user_display_name(target_id, qq_group_id);
    }
    const auto text =
        suffix.empty()
            ? fmt::format("[{}] {} [{}]", user, action, target)
            : fmt::format("[{}] {} [{}]{}", user, action, target, suffix);
    const obcx::common::Message message{
        {.type = "text", .data = {{"text", text}}}};
    if (route->mode == BridgeMode::TOPIC_TO_GROUP && topic_id > 0) {
      (void)co_await operations_->send_telegram_topic(telegram_group_id,
                                                      topic_id, message);
    } else {
      (void)co_await operations_->send_telegram_group(telegram_group_id,
                                                      message);
    }
  } catch (const std::exception &error) {
    OBCX_ERROR("处理戳一戳事件时出错: {}", error.what());
  }
}

auto QQEventHandler::get_poke_action_name(const int poke_type,
                                          const int poke_id) -> std::string {
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
  for (const char value : text) {
    if (std::string_view{"_*[]()~`>#+-=|{}.!"}.contains(value)) {
      result += '\\';
    }
    result += value;
  }
  return result;
}

auto QQEventHandler::fetch_user_display_name(const std::string &user_id,
                                             const std::string &group_id)
    -> boost::asio::awaitable<std::string> {
  std::optional<std::string> display_name;
  if (state_repository_) {
    display_name = co_await blocking_executor_->run(
        [repository = state_repository_, user_id, group_id] {
          return repository->query_user_display_name("qq", user_id, group_id);
        });
  }
  if (!display_name.has_value()) {
    co_await fetch_user_info(user_id, group_id);
    if (state_repository_) {
      display_name = co_await blocking_executor_->run(
          [repository = state_repository_, user_id, group_id] {
            return repository->query_user_display_name("qq", user_id, group_id);
          });
    }
  }
  co_return display_name.value_or(user_id);
}

auto QQEventHandler::fetch_user_info(const std::string &user_id,
                                     const std::string &group_id)
    -> boost::asio::awaitable<void> {
  try {
    const auto member = co_await operations_->get_onebot11_group_member(
        group_id, user_id, false);
    storage::UserInfo info;
    info.platform = "qq";
    info.user_id = user_id;
    info.group_id = group_id;
    info.nickname = !member.card.empty()    ? member.card
                    : !member.title.empty() ? member.title
                                            : member.nickname;
    info.title = member.title;
    info.last_updated = std::chrono::system_clock::now();
    if (state_repository_) {
      (void)co_await blocking_executor_->run(
          [repository = state_repository_, info] {
            return repository->save_or_update_user(info, true);
          });
    }
  } catch (const std::exception &error) {
    OBCX_DEBUG("获取QQ用户信息失败：{}", error.what());
  }
}

} // namespace bridge::qq
