# Sherclawk stages a Certainly clone (patches/ applied, see build.sh) and
# vendors its HTTP transport under vendor/.
cmake_minimum_required(VERSION 3.12)
project(sherclawk C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Shared native tool sources; read-only inspection lives in inspect.c.
set(TOOLS_SRC tools.c inspect.c mcp_guard.c)
# Certainly is fetch-only; build.sh expects it next to this repo. Override
# with -DCERTAINLY_DIR=<path> when it is cloned elsewhere.
if(NOT CERTAINLY_DIR)
    set(CERTAINLY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Certainly")
endif()
add_subdirectory("${CERTAINLY_DIR}" certainly-src)

# Every application target shares the same include paths, transport library
# and compile options. Wrapping add_application() keeps that list in one
# place; source sets used by several targets are variables below, so a new
# module cannot reach only some of the targets (the drift behind #148).
macro(sherclawk_app name)
    add_application(${name} ${ARGN})
    target_include_directories(${name} PRIVATE . vendor)
    target_link_libraries(${name} certainly FontManager)
    target_compile_options(${name} PRIVATE -Wall -Wextra -ffunction-sections)
    set_target_properties(${name} PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
    # The vendored vendor/http.c keeps HelloChat's request bytes unchanged
    # unless this marker selects Sherclawk's own User-Agent product token.
    target_compile_definitions(${name} PRIVATE SHERCLAWK_APP=1)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.local.h")
        target_compile_definitions(${name} PRIVATE SHERCLAWK_HAS_LOCAL_CONFIG=1)
    endif()
endmacro()

set(MCP_CORE mcp_config.c mcp_protocol.c mcp_stream.c mcp_client.c)
sherclawk_app(SherclawkMCPCheck tools/mcp-check.c ${MCP_CORE} json.c text.c vendor/http.c hello.r)
# Editor-side MCP code (config validation, text rules, staged store); the app
# does not link the protocol client yet.
set(MCP_EDITOR_SRC mcp_config.c mcp_editor.c mcp_store.c)
set(CHAT_CORE display.c chat.c json.c text.c network.c timing.c vendor/http.c)
# The shipping app and the diagnostics that #include main.c themselves.
# Add a module here once and every one of those targets gets it.
set(APP_SRC agent.c session.c preferences.c ${MCP_EDITOR_SRC} ${TOOLS_SRC}
    view_image.c build_project.c selfbuild.c toolserver.c ae_dispatch.c
    run_application.c application_process.c jobs.c ${CHAT_CORE} hello.r
    build/art.r CREATOR ShCk)
sherclawk_app(Sherclawk main.c ${APP_SRC})
# Full-app acceptance target; shipping Sherclawk retains its verified 4 KiB cap.
sherclawk_app(SherclawkLargeTextCheck main.c ${APP_SRC})
target_compile_definitions(SherclawkLargeTextCheck PRIVATE SHERCLAWK_LARGE_TEXT_CHECK=1)
sherclawk_app(SherclawkProbe tools/probe.c agent.c ${TOOLS_SRC} ${CHAT_CORE} hello.r)
sherclawk_app(SherclawkWriteCheck tools/write-check.c ${TOOLS_SRC} json.c text.c hello.r)
sherclawk_app(SherclawkSearchCheck tools/search-check.c ${TOOLS_SRC} json.c text.c hello.r)
sherclawk_app(SherclawkEditCheck tools/edit-check.c ${TOOLS_SRC} json.c text.c hello.r)
sherclawk_app(SherclawkProjectCheck tools/project-check.c ${TOOLS_SRC} json.c text.c hello.r)
# Compile the exact model-facing starter as well as the app that embeds it.
set(TEMPLATE_DIR templates/ppc-toolbox)
add_application(SherclawkTemplate ${TEMPLATE_DIR}/main.c ${TEMPLATE_DIR}/scene.c ${TEMPLATE_DIR}/io.c ${TEMPLATE_DIR}/png.c ${TEMPLATE_DIR}/selfrender.c ${TEMPLATE_DIR}/app.r CREATOR SHTP)
target_include_directories(SherclawkTemplate PRIVATE ${TEMPLATE_DIR})
target_compile_options(SherclawkTemplate PRIVATE -Wall -Wextra)
# The build/run/self-build/quit diagnostics share this source set.
set(BUILD_CHECK_SRC tools/build-check.c build_project.c selfbuild.c toolserver.c
    ae_dispatch.c run_application.c application_process.c ${TOOLS_SRC} jobs.c
    json.c text.c hello.r)
sherclawk_app(SherclawkBuildCheck ${BUILD_CHECK_SRC})
sherclawk_app(SherclawkRunCheck ${BUILD_CHECK_SRC})
target_compile_definitions(SherclawkRunCheck PRIVATE RUN_CHECK=1)
sherclawk_app(SherclawkQuitCheck ${BUILD_CHECK_SRC})
target_compile_definitions(SherclawkQuitCheck PRIVATE RUN_CHECK=1 QUIT_CHECK=1)
sherclawk_app(SherclawkSelfBuildCheck ${BUILD_CHECK_SRC})
target_compile_definitions(SherclawkSelfBuildCheck PRIVATE RUN_CHECK=1 SELF_BUILD_CHECK=1)
sherclawk_app(SherclawkSelfBuildStopCheck ${BUILD_CHECK_SRC})
target_compile_definitions(SherclawkSelfBuildStopCheck PRIVATE RUN_CHECK=1 SELF_BUILD_CHECK=1 SELF_BUILD_STOP_CHECK=1)
sherclawk_app(SherclawkToolServerCheck tools/toolserver-check.c tools/toolserver-check.r)
set(NATIVE_BUILD_CHECK_SRC tools/native-build-check.c toolserver.c ae_dispatch.c tools/toolserver-check.r)
sherclawk_app(SherclawkNativeBuildCheck ${NATIVE_BUILD_CHECK_SRC})
sherclawk_app(SherclawkNativeBuildErrorCheck ${NATIVE_BUILD_CHECK_SRC})
target_compile_definitions(SherclawkNativeBuildErrorCheck PRIVATE NATIVE_BUILD_ERROR=1)
sherclawk_app(SherclawkInspectCheck tools/inspect-check.c ${TOOLS_SRC} json.c text.c hello.r)
sherclawk_app(SherclawkViewImageCheck tools/view-image-check.c ${TOOLS_SRC} view_image.c json.c text.c hello.r)
sherclawk_app(SherclawkTrashCheck tools/trash-check.c ${TOOLS_SRC} json.c text.c hello.r)
sherclawk_app(SherclawkMCPGuardCheck tools/mcp-guard-check.c ${TOOLS_SRC} json.c text.c hello.r)
# These two diagnostics #include ../main.c themselves, so APP_SRC carries the
# app modules they do not name directly.
sherclawk_app(SherclawkScrollCheck tools/scroll-check.c ${APP_SRC})
sherclawk_app(SherclawkHandoffCheck tools/handoff-check.c ${APP_SRC})

# Main-window controls use the OS 9 Appearance Manager's default-button ring.
foreach(app SherclawkLargeTextCheck Sherclawk SherclawkScrollCheck SherclawkHandoffCheck)
    target_link_libraries(${app} AppearanceLib)
endforeach()

foreach(app SherclawkEditCheck SherclawkBuildCheck SherclawkRunCheck SherclawkQuitCheck SherclawkSelfBuildCheck SherclawkSelfBuildStopCheck)
    target_compile_definitions(${app} PRIVATE SHERCLAWK_LARGE_TEXT_CHECK=1)
endforeach()
