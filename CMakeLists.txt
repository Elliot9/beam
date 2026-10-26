cmake_minimum_required(VERSION 3.20)
project(Beam VERSION 1.2.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BEAM_BUILD_TESTS "Build the unit tests" OFF)

if(WIN32)
  enable_language(RC)
endif()

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
endif()

# ---- portable core (no OS dependencies; unit-tested on any OS) -------------
add_library(beam_core STATIC
  src/core/calc.cpp
  src/core/edit_buffer.cpp
  src/core/favorites.cpp
  src/core/fuzzy.cpp
  src/core/hotkey.cpp
  src/core/json.cpp
  src/core/plugin_protocol.cpp
  src/core/query.cpp
  src/core/settings.cpp
  src/core/text.cpp
  src/core/usage.cpp
)
target_include_directories(beam_core PUBLIC src)
if(WIN32)
  target_compile_definitions(beam_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# ---- plugin host: shared logic + the platform's process runner -------------
add_library(beam_plugin STATIC
  src/plugin/plugin_manager.cpp
  src/plugin/plugin_templates.cpp
)
target_link_libraries(beam_plugin PUBLIC beam_core)

# Files compiled into the Linux binaries (Windows embeds PLUGINS.md through the .rc files).
function(beam_embed input symbol out_var)
  get_filename_component(name "${input}" NAME)
  set(output "${CMAKE_BINARY_DIR}/generated/${symbol}.cpp")
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output} -DSYMBOL=${symbol}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS "${input}" ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding ${name}" VERBATIM)
  set(${out_var} "${output}" PARENT_SCOPE)
endfunction()

if(WIN32)
  target_sources(beam_plugin PRIVATE src/win/plugin_process.cpp src/win/shell.cpp src/win/log.cpp)
  target_link_libraries(beam_plugin PUBLIC ole32 oleaut32 uuid shell32 shlwapi user32 advapi32 powrprof)
  # Files the .rc scripts embed: rebuild the resources when they change.
  set_source_files_properties(res/beam.rc res/beam-plugin.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/PLUGINS.md;${CMAKE_SOURCE_DIR}/res/beam.ico;${CMAKE_SOURCE_DIR}/res/beam.manifest;${CMAKE_SOURCE_DIR}/res/beam-plugin.manifest")
else()
  beam_embed(${CMAKE_SOURCE_DIR}/PLUGINS.md kBeamPluginsDoc BEAM_DOC_CPP)
  target_sources(beam_plugin PRIVATE src/linux/plugin_process.cpp ${BEAM_DOC_CPP})
  find_package(Threads REQUIRED)
  target_link_libraries(beam_plugin PUBLIC Threads::Threads)
endif()

# ---- beam-plugin: command-line tool for plugin authors (both platforms) ----
add_executable(beam-plugin src/plugin/plugin_cli.cpp)
target_link_libraries(beam-plugin PRIVATE beam_plugin)

# ---- Windows application ---------------------------------------------------
if(WIN32)
  target_sources(beam-plugin PRIVATE res/beam-plugin.rc)
  # The source root is on the include path so the .rc files can embed PLUGINS.md.
  target_include_directories(beam-plugin PRIVATE src res ${CMAKE_SOURCE_DIR})

  add_executable(Beam WIN32
    src/win/app.cpp
    src/win/apps.cpp
    src/win/icons.cpp
    src/win/launcher.cpp
    src/win/main.cpp
    src/win/renderer.cpp
    src/win/search.cpp
    src/win/settings_dialog.cpp
    src/win/tray.cpp
    src/win/winlist.cpp
    res/beam.rc
  )
  target_include_directories(Beam PRIVATE src res ${CMAKE_SOURCE_DIR})
  target_link_libraries(Beam PRIVATE beam_plugin
    d2d1 dwrite windowscodecs ole32 oleaut32 uuid shell32 shlwapi comctl32 dwmapi imm32
    user32 gdi32 advapi32 shcore powrprof version)
  if(MINGW)
    target_link_options(Beam PRIVATE -static -municode -s)
    target_link_options(beam-plugin PRIVATE -static -municode -s)
  endif()
  if(MSVC)
    # The manifests are embedded through the .rc files.
    target_link_options(Beam PRIVATE /MANIFEST:NO)
    target_link_options(beam-plugin PRIVATE /MANIFEST:NO)
    set_property(TARGET Beam beam-plugin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

# ---- Linux application (GTK 4) ---------------------------------------------
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.14 gio-unix-2.0)
  include(GNUInstallDirs)

  beam_embed(${CMAKE_SOURCE_DIR}/data/linux/gnome-shell/beam-windows@local.beam/extension.js
             kBeamExtensionJs BEAM_EXT_JS_CPP)
  beam_embed(${CMAKE_SOURCE_DIR}/data/linux/gnome-shell/beam-windows@local.beam/metadata.json
             kBeamExtensionMetadata BEAM_EXT_META_CPP)

  add_executable(beam
    src/linux/app.cpp
    src/linux/apps.cpp
    src/linux/hotkey.cpp
    src/linux/keys.cpp
    src/linux/launcher.cpp
    src/linux/main.cpp
    src/linux/search.cpp
    src/linux/system.cpp
    src/linux/windows.cpp
    ${BEAM_EXT_JS_CPP}
    ${BEAM_EXT_META_CPP}
  )
  target_link_libraries(beam PRIVATE beam_plugin PkgConfig::GTK4)
  target_compile_definitions(beam PRIVATE BEAM_VERSION="${PROJECT_VERSION}")
  foreach(target beam beam-plugin)
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
  endforeach()

  install(TARGETS beam beam-plugin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES data/linux/local.beam.Launcher.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(size 48 64 128 256)
    install(FILES data/linux/icons/${size}/local.beam.Launcher.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
  endforeach()
  # System-wide copy of the window-switcher extension (Beam enables it per user on request).
  install(DIRECTORY data/linux/gnome-shell/beam-windows@local.beam
          DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/extensions)
  install(FILES README.md README.zh-TW.md PLUGINS.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/beam-launcher)
endif()

# ---- tests -----------------------------------------------------------------
if(BEAM_BUILD_TESTS)
  add_executable(beam_tests tests/core_tests.cpp)
  target_link_libraries(beam_tests PRIVATE beam_core)
  if(NOT WIN32)
    # End-to-end runs of real plugin processes (bash / python3 scripts).
    add_executable(beam_plugin_tests tests/plugin_tests.cpp)
    target_link_libraries(beam_plugin_tests PRIVATE beam_plugin)
  endif()
endif()
