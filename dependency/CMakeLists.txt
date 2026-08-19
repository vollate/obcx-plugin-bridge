add_library(
  bridge_core STATIC
  config.cpp
  bridge_bot_operations.cpp
	  bridge_db_runtime.cpp
	  bridge_forwarding_runtime.cpp
	  bridge_hash.cpp
	  bridge_message_event_adapter.cpp
  bridge_state_repository.cpp
  received_message_repository.cpp
  media_converter.cpp
  media_processor.cpp
  retry_queue_manager.cpp
  path_manager.cpp
  # qq
  qq/command_handler.cpp
  qq/event_handler.cpp
  qq/handler.cpp
  qq/image_url_validator.cpp
  qq/media_processor/dispatch.cpp
  qq/media_processor/image.cpp
  qq/media_processor/file.cpp
  qq/media_processor/at.cpp
  qq/media_processor/simple_segments.cpp
  qq/media_processor/miniapp.cpp
  qq/message_formatter.cpp
  qq/photo_normalizer.cpp
  # telegram
  telegram/command_handler.cpp
  telegram/event_handler.cpp
  telegram/media_processor/animation.cpp
  telegram/media_processor/dispatch.cpp
  telegram/media_processor/files.cpp
  telegram/media_processor/sticker.cpp
  telegram/media_group_buffer.cpp
	  telegram/message_formatter.cpp
	  telegram/handler.cpp)

target_link_libraries(bridge_core PUBLIC obcx::obcx_core)
set_target_properties(bridge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
