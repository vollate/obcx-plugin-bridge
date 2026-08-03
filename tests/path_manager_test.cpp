#include "path_manager.hpp"

#include <gtest/gtest.h>

TEST(PathManagerTest, NormalizesContainerPathsWithoutFilesystemAccess) {
  const bridge::PathManager manager{"/tmp/bridge-host/./files/",
                                    "/root/llonebot/./cache/../bridge_files/"};

  EXPECT_EQ(manager.get_host_base(), "/tmp/bridge-host/files");
  EXPECT_EQ(manager.get_container_base(), "/root/llonebot/bridge_files");
  EXPECT_EQ(manager.to_container_path("temp/../images/photo.jpg"),
            "/root/llonebot/bridge_files/images/photo.jpg");
  EXPECT_EQ(manager.container_to_host_absolute(
                "/root/llonebot/bridge_files/images/../photo.jpg"),
            "/tmp/bridge-host/files/photo.jpg");
}
