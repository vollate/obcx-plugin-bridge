add_library(
  bridge_core SHARED
  config.cpp
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
  # telegram
  telegram/command_handler.cpp
  telegram/event_handler.cpp
  telegram/media_processor/animation.cpp
  telegram/media_processor/dispatch.cpp
  telegram/media_processor/files.cpp
  telegram/media_processor/sticker.cpp
  telegram/message_formatter.cpp
  telegram/handler.cpp
  # database
  database/manager.cpp
  database/manager_utils.cpp
  database/manager_create.cpp
  database/manager_insert.cpp
  database/manager_query.cpp
  database/manager_update.cpp
  database/manager_delete.cpp)

target_link_libraries(bridge_core PUBLIC obcx_core )
if ("$ENV{IN_NIX_SHELL}" STREQUAL "")
    target_link_libraries(bridge_core PUBLIC unofficial::sqlite3::sqlite3 )
else()
    target_link_libraries(bridge_core PUBLIC SQLite::SQLite3)
endif()
