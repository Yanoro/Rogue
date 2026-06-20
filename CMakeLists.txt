cmake_minimum_required(VERSION 3.14) # FetchContent requires 3.11+, 3.14 recommended

project(AIRogue)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# CURL
pkg_check_modules(CURL REQUIRED libcurl)

# JSON (Header only)
pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)

# ------------------------------------------------------------------------------
# Dependencies (FetchContent)
# ------------------------------------------------------------------------------
include(FetchContent)

# Prevent FetchContent from checking for updates on active branches (like master/main)
# This stops massive rebuilds from happening when cmake re-configures
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable FetchContent updates" FORCE)

# Fetch Raylib (Base Library)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
)
FetchContent_MakeAvailable(raylib)

# Fetch Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
endif()

# Fetch rlImGui
FetchContent_Declare(
    rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG        main
)
FetchContent_GetProperties(rlimgui)
if(NOT rlimgui_POPULATED)
    FetchContent_Populate(rlimgui)
    
    add_library(rlimgui STATIC
        ${rlimgui_SOURCE_DIR}/rlImGui.cpp
    )
    target_include_directories(rlimgui PUBLIC ${rlimgui_SOURCE_DIR})
    
    # Link against imgui and Raylib
    # Note: 'raylib' target is available from FetchContent above
    target_link_libraries(rlimgui PUBLIC imgui raylib)
    
    # Force include path for IDE for rlimgui and consumers
    # We use $<TARGET_PROPERTY:raylib,INTERFACE_INCLUDE_DIRECTORIES> to get raylib's include path
    target_include_directories(rlimgui PUBLIC 
        $<TARGET_PROPERTY:raylib,INTERFACE_INCLUDE_DIRECTORIES>
    )
endif()

# Fetch Flecs
FetchContent_Declare(
    flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG        v3.2.11
)
FetchContent_MakeAvailable(flecs)

# Use local raylib-cpp
add_subdirectory(raylib-cpp)

# ------------------------------------------------------------------------------
# Build
# ------------------------------------------------------------------------------

# Define sources
set(SOURCES
    src/main.cpp
    src/Game.cpp
    src/GameDraw.cpp
    src/GameECS.cpp
    src/Map.cpp
    src/AI.cpp
    src/NPC.cpp
    src/Character.cpp
    src/AIChatWindow.cpp
    src/AgentContextWindow.cpp
    src/EntityInfoWindow.cpp
    src/Command.cpp
    src/InputHandler.cpp
    src/PathFinding.cpp
    src/DebugWindowState.cpp
    src/DebugLog.cpp
    src/MapReloader.cpp
    src/DrawAsciiDebug.cpp
    src/DebugWindows.cpp
)

add_executable(AIRogue ${SOURCES})

# Include Directories
target_include_directories(AIRogue PRIVATE 
    include
    raylib-cpp/include
    ${flecs_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
    ${imgui_SOURCE_DIR}
    ${rlimgui_SOURCE_DIR}
)

# Link Libraries
target_link_libraries(AIRogue PRIVATE 
    flecs
    ${CURL_LIBRARIES}
    ${NLOHMANN_JSON_LIBRARIES}
    rlimgui
    raylib_cpp
    raylib # Ensure main raylib is linked if raylib-cpp doesn't propagate it fully
)

if(UNIX AND NOT APPLE)
    target_link_libraries(AIRogue PRIVATE pthread dl atomic)
endif()

# ------------------------------------------------------------------------------
# AStar Visualizer
# ------------------------------------------------------------------------------
add_executable(AStarVisualizer src/AStarVisualizer.cpp)
target_include_directories(AStarVisualizer PRIVATE raylib-cpp/include)
target_link_libraries(AStarVisualizer PRIVATE raylib_cpp raylib)
if(UNIX AND NOT APPLE)
    target_link_libraries(AStarVisualizer PRIVATE pthread dl atomic)
endif()

# Compiler Options
target_compile_options(AIRogue PRIVATE 
    ${CURL_CFLAGS_OTHER}
    ${RAYLIB_CFLAGS_OTHER}
    -Wall
    -Wextra
)

# Compiler Options
target_compile_options(AIRogue PRIVATE 
    ${CURL_CFLAGS_OTHER}
    ${RAYLIB_CFLAGS_OTHER}
    -Wall
    -Wextra
)
