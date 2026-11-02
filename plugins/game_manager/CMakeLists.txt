# game_manager — Plugify C++ port of the SwiftlyS2 plugin "GameManager" by criskkky (command and
# HUD message blocking, kill feed / corpse / sound controls, convar toggles).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no hudmenu and no shared wrapper library.
# The map gate, handle/slot validation, command ownership and the complete teardown live in
# src/plugin.cpp; the engine-free decisions live in game_manager_core.
#
#   game_manager_core       engine-free core (config parsing + defaults, command-block decision,
#                           kill-feed plan, corpse alpha curve, ragdoll match, HUD text filters;
#                           also used by tests/game_manager_tests.cpp)
#   plugify-game_manager    the plugin shared library (entry "bin/game_manager")

set(GAME_MANAGER_VERSION "1.1.0")

# --- s2sdk binding globals ----------------------------------------------------------------
# The root CMakeLists defines s2sdk_bindings before it globs plugins/* (the glob is sorted, and the
# root target is configured first), so every in-tree build reuses it. An exported repository or a
# build of this directory alone still has sdk/include; a private OBJECT library with a
# plugin-unique name is the fallback then, so it can never collide with another consumer's target.
set(GAME_MANAGER_S2SDK_BINDINGS "")
if(TARGET s2sdk_bindings)
    set(GAME_MANAGER_S2SDK_BINDINGS "$<TARGET_OBJECTS:s2sdk_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    add_library(game_manager_s2sdk_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    target_include_directories(game_manager_s2sdk_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(game_manager_s2sdk_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(GAME_MANAGER_S2SDK_BINDINGS "$<TARGET_OBJECTS:game_manager_s2sdk_bindings_obj>")
else()
    message(FATAL_ERROR "game_manager needs the generated s2sdk binding globals (sdk/include/s2sdk/s2sdk.cpp)")
endif()

# --- core ---------------------------------------------------------------------
add_library(game_manager_core STATIC src/game_manager_core.cpp)
target_include_directories(game_manager_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the vendored nlohmann/json must not fail -Werror on our sources.
target_include_directories(game_manager_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(game_manager_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(game_manager_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-game_manager SHARED
        src/plugin.cpp
        # Binding globals (__s2sdk_*) filled by the plugify module from the s2sdk plugin declared
        # as a dependency. Nothing else is needed: the plugin uses no menu/permissions/polyhook API.
        ${GAME_MANAGER_S2SDK_BINDINGS}
)
target_include_directories(plugify-game_manager PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror here.
target_include_directories(plugify-game_manager SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-game_manager PRIVATE
        game_manager_core
        -static-libstdc++ -static-libgcc)
target_compile_options(plugify-game_manager PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-game_manager PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="game_manager"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-game_manager PROPERTIES OUTPUT_NAME game_manager)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-game_manager PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/game_manager.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/game_manager.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Pure logic: config parsing/defaults against the production layout and the checked-in production
# asset, the command-block decision, the kill-feed plan, the corpse alpha curve and the HUD text
# filters. No engine involved.
add_executable(game_manager_tests tests/game_manager_tests.cpp)
target_link_libraries(game_manager_tests PRIVATE game_manager_core -static-libstdc++ -static-libgcc)
target_compile_options(game_manager_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(game_manager_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        GM_PRODUCTION_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/config.jsonc")
set_target_properties(game_manager_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Engine test: the plugin TU is compiled in (every handler lives in its anonymous namespace) and
# the __s2sdk_* binding globals are filled with recording fakes - proves the map gate, the Pre
# Continue contract, the slot/handle rules, config parity, command ownership and the exact
# teardown (the plg-reload contract), with no pisexcompat involved.
add_executable(game_manager_engine_tests tests/game_manager_engine_test.cpp)
target_include_directories(game_manager_engine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(game_manager_engine_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(game_manager_engine_tests PRIVATE
        game_manager_core
        ${GAME_MANAGER_S2SDK_BINDINGS}
        -static-libstdc++ -static-libgcc)
target_compile_options(game_manager_engine_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(game_manager_engine_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="game_manager"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        GM_PRODUCTION_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/config.jsonc"
)
set_target_properties(game_manager_engine_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
