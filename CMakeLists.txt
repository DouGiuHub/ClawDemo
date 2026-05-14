cmake_minimum_required(VERSION 3.16)
project(ClawStory VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

include(FetchContent)

# ixwebsocket
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.5
    GIT_SHALLOW    TRUE
)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

# ── Source ────────────────────────────────────────────────────────────────────
add_executable(clawstory
    src/main.cpp
    src/llm_client.cpp
    src/ws_server.cpp
    src/novel_agent.cpp
)

target_include_directories(clawstory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(clawstory PRIVATE
    CLAWSTORY_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
)

target_link_libraries(clawstory
    Threads::Threads
    CURL::libcurl
    ixwebsocket
    nlohmann_json::nlohmann_json
)

# Copy config.json to build directory
add_custom_command(TARGET clawstory POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/config.json
        $<TARGET_FILE_DIR:clawstory>/config.json
    COMMENT "Copying config.json to build directory"
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND UNIX)
    add_custom_command(TARGET clawstory POST_BUILD
        COMMAND strip --strip-all $<TARGET_FILE:clawstory>
        COMMENT "Stripping clawstory binary"
    )
endif()
