# as_maps — Plugify C++ plugin: the "Maps" module of the Pisex admin_system (Metamod plugin
# as_maps) on top of the admin_system plugin and the s2sdk API.
#
#   plugify-as_maps  the plugin shared library (entry "bin/as_maps" -> bin/libas_maps.so)
#
# Standalone: no pisexcompat and no helper library (docs/PLUGIN_STANDARD.md §7). The optional
# hudmenu plugin is declared as a dependency only for the player's own "!menu" choice: when it is
# installed, its GetClientMenuType() selects the per-player menu backend; when it is not, the
# plugin falls back to the same default pisexcompat used ("hud", then "wasd", then "button").
# The guards of handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself — the B1/B2 map gate,
# the C2 client checks, the C9 timer rules, the C10 menu ownership and the C11 command ownership.
# Like every admin_system module the .so compiles the binding globals the plugify module fills at
# load time:
#   * s2sdk (__s2sdk_*),
#   * the generated admin_system consumer bindings (__admin_system_*) plus the hand-written
#     consumer wrapper plugins/admin_system/include/admin_api.hpp,
#   * the hudmenu bindings (__hudmenu_*) for the optional per-player menu type.

set(AS_MAPS_VERSION "1.1.0")

# The generated admin_system binding globals (__admin_system_*) have to be compiled into every
# consumer plugin. plugins/admin_system builds them as admin_system_bindings_obj; when this
# directory is built on its own (or that target does not exist yet) the fallback below is used.
# Its name is unique per plugin so it can never collide with another plugin's fallback in the same
# configure run.
if(TARGET admin_system_bindings_obj)
    set(AS_MAPS_ADMIN_SYSTEM_BINDINGS_TARGET admin_system_bindings_obj)
else()
    add_library(as_maps_admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(as_maps_admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_maps_admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_MAPS_ADMIN_SYSTEM_BINDINGS_TARGET as_maps_admin_system_bindings_obj)
endif()

# The optional hudmenu binding globals (__hudmenu_*). The top-level CMakeLists defines
# hudmenu_bindings unconditionally; the fallback keeps a standalone build of this directory
# working. Its name is unique per plugin.
if(TARGET hudmenu_bindings)
    set(AS_MAPS_HUDMENU_BINDINGS_TARGET hudmenu_bindings)
else()
    add_library(as_maps_hudmenu_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(as_maps_hudmenu_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_maps_hudmenu_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_MAPS_HUDMENU_BINDINGS_TARGET as_maps_hudmenu_bindings_obj)
endif()

add_library(plugify-as_maps SHARED
        src/plugin.cpp
        # Binding globals (__s2sdk_* / __admin_system_* / __hudmenu_*) filled by the plugify module
        # from the plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:${AS_MAPS_ADMIN_SYSTEM_BINDINGS_TARGET}>
        $<TARGET_OBJECTS:${AS_MAPS_HUDMENU_BINDINGS_TARGET}>
)
target_include_directories(plugify-as_maps PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated admin_system/s2sdk/plg headers and admin_api.hpp are not ours to fix;
# their warnings must not fail -Werror on our sources.
target_include_directories(plugify-as_maps SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_maps PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_maps PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_maps PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_maps"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_maps PROPERTIES OUTPUT_NAME as_maps)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_maps PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_maps.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_maps.pplugin"
)

# --- tests ---------------------------------------------------------------------
# as_maps_config_tests: engine-free. The KV1 reader, the maps.ini candidate order (raw Metamod
# path first, then <configs>/admin_system, then <configs>/as_maps), the display-name mapping, the
# std::map ordering of the menu and the colour table.
# as_maps_tests: the plugin TU is compiled into the test and every __s2sdk_* / __admin_system_*
# binding slot is a recording fake: the map gate, the server-only mm_map command, the delayed
# changelevel (recorded, never executed), the map menu, slot identity/reuse, teardown and the
# fail-soft starts. Written into build/tests so ../test.sh runs them with the rest (*_tests).
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_maps_config_tests tests/as_maps_config_tests.cpp)
    target_include_directories(as_maps_config_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(as_maps_config_tests PRIVATE plugify_sdk -static-libstdc++ -static-libgcc)
    target_compile_options(as_maps_config_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_maps_config_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            AS_MAPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    set_target_properties(as_maps_config_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    add_executable(as_maps_tests
            tests/as_maps_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            $<TARGET_OBJECTS:${AS_MAPS_ADMIN_SYSTEM_BINDINGS_TARGET}>
            $<TARGET_OBJECTS:${AS_MAPS_HUDMENU_BINDINGS_TARGET}>
    )
    target_include_directories(as_maps_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_maps_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(as_maps_tests PRIVATE plugify_sdk -static-libstdc++ -static-libgcc)
    target_compile_options(as_maps_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_maps_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_maps"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            AS_MAPS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    set_target_properties(as_maps_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
