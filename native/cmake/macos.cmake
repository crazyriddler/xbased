set(NC_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../../deps" CACHE PATH "native core third-party sources")

set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYCORE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/zydis" zydis-build EXCLUDE_FROM_ALL)
add_library(nc_zydis INTERFACE)
target_link_libraries(nc_zydis INTERFACE Zydis)

set(NC_XXHASH_DIR "${NC_DEPS}/xxhash")

add_compile_options(-fno-delete-null-pointer-checks -msse4.1 -mpopcnt -g1)
add_compile_definitions(_DARWIN_C_SOURCE)
set(NC_MACOS_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../compat/macos/include")
set(NC_LINUX_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../compat/linux/include")
set(NC_MACOS_EXE_LINK
    -Wl,-pagezero_size,0x10000
    -Wl,-image_base,0x200000000
    -Wl,-no_pie
    -Wl,-segaddr,__XBGUEST,0x10000
    -Wl,-segprot,__XBGUEST,rwx,---
    -Wl,-segaddr,__XBGUARD,0x100000000
    -Wl,-segprot,__XBGUARD,rwx,---)
function(nc_macos_compat)
  foreach(t ${ARGN})
    if(NOT TARGET ${t})
      continue()
    endif()
    target_include_directories(${t} BEFORE PRIVATE "${NC_MACOS_COMPAT_INC}" "${NC_LINUX_COMPAT_INC}")
    if(NOT t STREQUAL "nc_slirp")
      target_compile_options(${t} PRIVATE "SHELL:-include nc_macos.h")
    endif()
    get_target_property(type ${t} TYPE)
    if(type STREQUAL "EXECUTABLE")
      target_link_options(${t} PRIVATE ${NC_MACOS_EXE_LINK})
    endif()
  endforeach()
endfunction()

add_compile_definitions(NC_DESKTOP=1)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/SDL3" sdl3 EXCLUDE_FROM_ALL)

set(NC_MOLTENVK "" CACHE FILEPATH "libMoltenVK.dylib with an x86_64 slice")
if(NOT NC_MOLTENVK)
  foreach(c "${NC_DEPS}/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
            "${CMAKE_CURRENT_LIST_DIR}/../../../MoltenVK/Package/Latest/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
            "/usr/local/lib/libMoltenVK.dylib")
    if(EXISTS "${c}")
      execute_process(COMMAND lipo -archs "${c}" OUTPUT_VARIABLE nc_mvk_archs ERROR_QUIET)
      if(nc_mvk_archs MATCHES "x86_64")
        get_filename_component(c "${c}" REALPATH)
        set(NC_MOLTENVK "${c}" CACHE FILEPATH "libMoltenVK.dylib with an x86_64 slice" FORCE)
        break()
      endif()
    endif()
  endforeach()
endif()
if(NC_MOLTENVK)
  message(STATUS "MoltenVK: ${NC_MOLTENVK}")
else()
  message(WARNING "No libMoltenVK.dylib with an x86_64 slice found (tools/fetch_deps.sh fetches one; or "
                  "-DNC_MOLTENVK=<path>): the app will need the Vulkan SDK's loader at run time")
endif()
