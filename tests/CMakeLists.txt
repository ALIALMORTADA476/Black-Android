cmake_minimum_required(VERSION 3.22)
project(black_settings_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

# Minimal portable stub providing app::Settings / app::GameInfo headers without SDL/Vulkan/Android NDK.
add_executable(test_settings_core
    test_settings_core.cpp
    ${REPO_ROOT}/src/common/mod_schema.cpp
    ${REPO_ROOT}/src/common/presets.cpp
    ${REPO_ROOT}/src/game/arabic_strings.cpp
)

target_include_directories(test_settings_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/src
)

target_link_libraries(test_settings_core PRIVATE tomlplusplus::tomlplusplus)

enable_testing()
add_test(NAME SettingsCoreTests COMMAND test_settings_core)

