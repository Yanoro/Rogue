cmake_minimum_required(VERSION 3.14) # FetchContent requires 3.11+, 3.14 recommended

project(AIRogue)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------------------------------
# Build type
# ------------------------------------------------------------------------------
# Nix's gcc wrapper appends -O2 to every compile through NIX_CFLAGS_COMPILE.
# With no CMAKE_BUILD_TYPE set, nothing overrides it, so the project compiled at
# -O2 + AddressSanitizer + -g -- the slowest possible combination. Measured on
# this machine for src/GameECS.cpp:
#
#   -O2 -g (what an unset build type produced)  231 s   3.5 GiB peak RSS
#   -O1 -g                                      148 s   3.3 GiB
#   -O0 -g (Debug)                               55 s   2.7 GiB
#
# The same ratio holds for every translation unit, so the optimizer level is by
# far the biggest lever on build time (an unset build type was ~4x slower than
# intended: the -O0 below only applies when the config is actually Debug).
# Default to Debug for the edit/build/test loop; configure with
# -DCMAKE_BUILD_TYPE=Release when you care how fast the game runs.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

option(AIROGUE_ASAN "Build with AddressSanitizer" ON)

# Caps per-compiler-process memory with GCC's GC parameters. Measured on
# src/GameECS.cpp: 55 s / 2.7 GiB normally vs 59 s / 1.5 GiB here -- ~45% less
# RAM for ~6% more wall time, which is what makes a high -j safe on a 15 GiB
# box. -g1 keeps line numbers (ASan stack traces stay readable) but drops
# local-variable info, so `print` on locals in gdb stops working.
option(AIROGUE_LOW_MEMORY "Use much less compiler RAM at slightly longer build time" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# CURL
pkg_check_modules(CURL REQUIRED libcurl)

# JSON (Header only)
pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)

# Audio backends.
#
# raylib's raudio dlopen()s ALSA/PulseAudio at runtime (MA_NO_JACK is the only
# backend it hard-disables), so neither appears in the link line raylib hands out
# -- and neither is on the loader path on NixOS, where there is no ld.so.cache
# entry for them. The failure is the dangerous kind: miniaudio falls back to its
# NULL backend, InitAudioDevice() reports success, IsAudioDeviceReady() returns
# true, PlaySound() runs, IsSoundPlaying() goes true and then false after the
# right duration, and no sound is produced. Verified here: without the ALSA
# library loadable the backend is "miniaudio | Null".
#
# Linking them explicitly (with --no-as-needed, so they are not dropped as
# unused) both loads them at startup and puts their store directories into the
# executable's RUNPATH, which is what miniaudio's dlopen then searches. ALSA
# alone works; PulseAudio is preferred by miniaudio when both are present and
# routes through PipeWire's pulse server, so both are linked.
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(PULSEAUDIO REQUIRED libpulse)

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
# Pinned to a specific commit so that the local patch below keeps applying
# cleanly. When bumping this tag, re-create patches/rlimgui-*.patch against the
# new revision.
set(RLIMGUI_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/rlimgui-multi-monitor-display-fix.patch")
set(RLIMGUI_APPLY_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply-patch.cmake")
FetchContent_Declare(
    rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG        ef129d1858373b6fe332c45f85a1dfc1421bebae
    # Keep the ImGui overlay aligned with rayLib's real window/render size when
    # more than one monitor is connected. apply-patch.cmake makes this
    # idempotent, so re-running the patch step is always safe.
    PATCH_COMMAND  ${CMAKE_COMMAND}
                   -DSRC_DIR=<SOURCE_DIR>
                   -DPATCH_FILE=${RLIMGUI_PATCH}
                   -P "${RLIMGUI_APPLY_PATCH}"
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
    src/CombatHud.cpp
    src/GameECS.cpp
    src/Map.cpp
    src/OllamaAI.cpp
    src/GeminiAI.cpp
    src/OpenRouterAI.cpp
    src/Character.cpp
    src/AIChatWindow.cpp
    src/VisionChatWindow.cpp
    src/ImageEncode.cpp
    src/AgentContextWindow.cpp
    src/EntityInfoWindow.cpp
    src/StorageWindow.cpp
    src/CharacterStatusWindow.cpp
    src/NavigationDebugWindow.cpp
  src/LogPaths.cpp
  src/NavigatorLog.cpp
    src/Command.cpp
    src/InteractionRegistry.cpp
    src/GlobalCommandRegistry.cpp
    src/InputHandler.cpp
    src/PathFinding.cpp
    src/DebugWindowState.cpp
    src/Settings.cpp
    src/SettingsSchema.cpp
    src/SettingsReference.cpp
    src/SettingsWindow.cpp
    src/DebugLog.cpp
    src/MapReloader.cpp
    src/MapDesigner.cpp
    src/DrawAsciiDebug.cpp
    src/DebugWindows.cpp
    src/AgentBrain.cpp
    src/ObjectFactory.cpp
    src/ResourceManager.cpp
    src/SpritePaths.cpp
    src/RecipeRegistry.cpp
    src/GuideBuilder.cpp
    src/ItemRegistry.cpp
    src/RaceRegistry.cpp
    src/SkillRegistry.cpp
    src/StatRegistry.cpp
    src/PortraitRegistry.cpp
    src/ProjectileRegistry.cpp
    src/SkillshotRegistry.cpp
    src/TileRegistry.cpp
    src/MemoryDigestJob.cpp
  src/PlaceCategoryRegistry.cpp
    src/TaskKindRegistry.cpp
    src/TaskVerification.cpp
    src/TileDesigner.cpp
    src/ObjectDesigner.cpp
    src/SpriteKinds.cpp
    src/DesignerDomains.cpp
    src/SpriteReply.cpp
    src/SpriteRouter.cpp
    src/TileArt.cpp
    src/TileArtQueue.cpp
    src/PixelDSL.cpp
    src/BlueprintSnapshot.cpp
    src/StyleDescriber.cpp
    src/SpriteGapOffer.cpp
    src/PromptRewriter.cpp
    src/RewriteInputBox.cpp
    src/RewriteBriefs.cpp
    src/BlueprintRegistry.cpp
    src/ContentFiles.cpp
    src/BlueprintExpansion.cpp
    src/GapLog.cpp
    src/ActorSpriteRegistry.cpp
    src/RigRegistry.cpp
    src/RigDesigner.cpp
    src/NpcTemplateRegistry.cpp
    src/BrainTemplateRegistry.cpp
    src/ArtRequestsPanel.cpp
    src/ContentKinds.cpp
    src/ContentEditorWindow.cpp
    src/DesignerSession.cpp
    src/DesignerWindow.cpp
    src/SpeechQueue.cpp
    src/PiperSpeech.cpp
    src/EffectParsing.cpp
    src/TrainingParsing.cpp
    src/Trading.cpp
    src/SystemOneRecorder.cpp
    src/JevClassifier.cpp
    src/SystemOneProfiles.cpp
    src/SystemOneWindow.cpp
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
    ${imgui_SOURCE_DIR}/misc/cpp
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

# The audio backends raylib only dlopens, so that they are DT_NEEDED (loaded
# before main() runs, which makes miniaudio's later dlopen find them immediately)
# and so their directories land in the RUNPATH. --no-as-needed is required: the
# game references no snd_*/pa_* symbol itself, so an --as-needed linker would drop
# both libraries and put nothing on the search path. See the pkg_check_modules
# block near the top for why this is not optional.
target_link_options(AIRogue PRIVATE "-Wl,--no-as-needed")
target_link_libraries(AIRogue PRIVATE ${ALSA_LIBRARIES} ${PULSEAUDIO_LIBRARIES})
target_link_directories(AIRogue PRIVATE ${ALSA_LIBRARY_DIRS} ${PULSEAUDIO_LIBRARY_DIRS})
set_target_properties(AIRogue PROPERTIES
    BUILD_RPATH "${ALSA_LIBRARY_DIRS}:${PULSEAUDIO_LIBRARY_DIRS}")

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
    $<$<BOOL:${AIROGUE_ASAN}>:-fsanitize=address>
    -g -fno-omit-frame-pointer
    -Wall
    -Wno-maybe-uninitialized
    -Wextra
    # Nix hardening (fortify) injects -O2 into every compile, and CMake's Debug
    # config only adds -g -- so without this a "Debug" build is optimized and
    # compiles like a release build. Option order is preserved, so -O0 wins over
    # the injected -O2 (verified: __OPTIMIZE__ is undefined afterwards).
    $<$<CONFIG:Debug>:-O0>
    # At -O0 Nix's forced _FORTIFY_SOURCE=3 is inert and emits a #warning in
    # every TU; silence it for Debug only.
    $<$<CONFIG:Debug>:-Wno-cpp>
    # See AIROGUE_LOW_MEMORY above. -g1 must come after -g to win.
    $<$<BOOL:${AIROGUE_LOW_MEMORY}>:-g1>
    $<$<BOOL:${AIROGUE_LOW_MEMORY}>:--param=ggc-min-expand=20>
    $<$<BOOL:${AIROGUE_LOW_MEMORY}>:--param=ggc-min-heapsize=32768>
)

if(AIROGUE_ASAN)
    target_link_options(AIRogue PRIVATE -fsanitize=address)
endif()


# ------------------------------------------------------------------------------
# Spatial memory derivation benchmark
# ------------------------------------------------------------------------------
# Same derivation code as the game, driven over a real map without the window.
# Used to keep the once-per-sample pass inside a frame budget. See
# bench/spatial_memory_bench.cpp.
add_executable(SpatialMemoryBench
    bench/spatial_memory_bench.cpp
    src/PlaceCategoryRegistry.cpp
    src/DebugLog.cpp
    src/InteractionRegistry.cpp
)
target_include_directories(SpatialMemoryBench PRIVATE
    include
    raylib-cpp/include
    ${flecs_SOURCE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/misc/cpp
    ${rlimgui_SOURCE_DIR}
)
target_link_libraries(SpatialMemoryBench PRIVATE
    flecs
    ${NLOHMANN_JSON_LIBRARIES}
    raylib_cpp
    raylib
)
target_compile_options(SpatialMemoryBench PRIVATE
    -g -fno-omit-frame-pointer
    -Wall
    -Wno-maybe-uninitialized
    -Wextra
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-Wno-cpp>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(SpatialMemoryBench PRIVATE pthread dl atomic)
endif()

