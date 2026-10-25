# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 版本号故意抬高: 官方服务器会在启动时比对版本并自动 OTA (application.cc CheckNewVersion),
# 版本高于官方任何发布版即可防止定制固件被自动刷掉 (force=1 强推除外)
set(PROJECT_VER "99.0.0")
project(xiaozhi)
