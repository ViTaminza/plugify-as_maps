# as_maps — Plugify C++ plugin: the "Maps" module of the Pisex admin_system (Metamod plugin
# as_maps) on top of the admin_system plugin and pisexcompat.
#
#   plugify-as_maps  the plugin shared library (entry "bin/as_maps" -> bin/libas_maps.so)

set(AS_MAPS_VERSION "1.0.0")

# Binding globals filled by the plugify module from our manifest dependencies:
#   __s2sdk_*        (s2sdk_bindings, compiled into every plugin)
#   __admin_system_* (generated consumer bindings of the admin_system plugin)
set(AS_MAPS_BINDING_OBJECTS "")
if(TARGET admin_system_bindings_obj)
    list(APPEND AS_MAPS_BINDING_OBJECTS $<TARGET_OBJECTS:admin_system_bindings_obj>)
else()
    message(WARNING "as_maps: admin_system_bindings_obj is missing (build plugins/admin_system first)")
endif()

add_library(plugify-as_maps SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${AS_MAPS_BINDING_OBJECTS}
)
target_include_directories(plugify-as_maps PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The consumer side of the admin_system API: admin_api.hpp plus the generated bindings. SYSTEM:
# warnings from the generated/third-party headers must not fail -Werror on our sources.
target_include_directories(plugify-as_maps SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_maps PRIVATE
        pisexcompat
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
