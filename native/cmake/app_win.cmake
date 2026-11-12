# The PC executable (Windows, Linux): the SDL3 front end (app/win_main.c)
# on the core, with the presentation and the frame pacing of the Android
# app's (app/present.c, app/vsync.c), and the dashboard (app/ui/).
set(NC_APP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_mods.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/mod_fit.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_auto.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_aslr.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_frame.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/present.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vsync.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/ui.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/i18n.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/config.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/console_cfg.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xbox_data.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/library.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/input_map.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/sound.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_settings.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/cheat_text.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_memory.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_browse.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_find.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xemu_import.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/update.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/shortcut.c"
)
if(WIN32)
  enable_language(RC)
  add_executable(hakux WIN32 ${NC_APP_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/app/win/xbased.rc")
  # the linker's map: the profile's offsets resolved where there is no debug info (HAKUX_PROF)
  target_link_options(hakux PRIVATE "-Wl,-Map=$<TARGET_FILE_DIR:hakux>/xbased.map")
  target_link_libraries(hakux PRIVATE ole32 uuid shell32 mpr)   # the desktop shortcuts (app/ui/shortcut.c); mpr: a network drive's share (dash_browse.c)
elseif(APPLE)
  set(NC_ICNS "${CMAKE_BINARY_DIR}/xbased.icns")
  add_custom_command(OUTPUT "${NC_ICNS}"
    COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/app/win/icon256.png -DOUT=${NC_ICNS}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos_icns.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/win/icon256.png" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos_icns.cmake"
    VERBATIM)
  set_source_files_properties("${NC_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(hakux MACOSX_BUNDLE ${NC_APP_SOURCES} "${NC_ICNS}")
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/app/win_main.c" nc_ver REGEX "^#define XBASED_VERSION ")
  string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" nc_ver "${nc_ver}")
  set_target_properties(hakux PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/mac/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.crazyriddler.xbased"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${nc_ver}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${nc_ver}")
else()
  add_executable(hakux ${NC_APP_SOURCES})
endif()
# xbased (.exe), with its icon and version on Windows (app/win/xbased.rc)
set_target_properties(hakux PROPERTIES OUTPUT_NAME xbased)
target_link_libraries(hakux PRIVATE hakux_native hakux_hle SDL3::SDL3)
target_include_directories(hakux PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/stb")  # public domain, vendored
target_compile_options(hakux PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
# the mods and cheats the game folders hold (mods/README.md), and the
# dashboard's fonts, beside the executable
if(APPLE)
  set(nc_res "$<TARGET_BUNDLE_CONTENT_DIR:hakux>/Resources")
  add_custom_command(TARGET hakux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/mods" "${nc_res}/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/cheats" "${nc_res}/cheats"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/fonts" "${nc_res}/fonts")
  if(NC_MOLTENVK)
    add_custom_command(TARGET hakux POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:hakux>/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E copy "${NC_MOLTENVK}" "$<TARGET_BUNDLE_CONTENT_DIR:hakux>/Frameworks/libMoltenVK.dylib")
  endif()
  add_custom_command(TARGET hakux POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:hakux>"
    COMMENT "Signing xbased.app (ad hoc)")
else()
  add_custom_command(TARGET hakux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/mods" "$<TARGET_FILE_DIR:hakux>/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/cheats" "$<TARGET_FILE_DIR:hakux>/cheats"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/fonts" "$<TARGET_FILE_DIR:hakux>/fonts")
endif()
