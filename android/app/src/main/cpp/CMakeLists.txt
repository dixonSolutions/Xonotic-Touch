# darkplaces as libmain.so, the shared object SDLActivity loads on Android.
#
# The engine's own build system is a makefile that assumes a POSIX host, so this
# reproduces just the SDL client target. The source list is *read out of*
# makefile.inc rather than duplicated: the Touch port adds engine files
# (touch_ui.c, vid_touchdetect.c) and a hand-copied list would rot.
cmake_minimum_required(VERSION 3.22)
project(xonotic-touch C)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
cmake_path(NORMAL_PATH REPO_ROOT)
set(DP_DIR "${REPO_ROOT}/engine/darkplaces")

if(NOT DEFINED XONOTIC_DEPS_PREFIX)
    message(FATAL_ERROR "XONOTIC_DEPS_PREFIX must point at the output of scripts/android-deps.sh")
endif()
if(NOT EXISTS "${DP_DIR}/makefile.inc")
    message(FATAL_ERROR "No engine sources at ${DP_DIR} — run scripts/fetch-sources.sh code")
endif()

##### Source list, straight from makefile.inc ##################################

file(READ "${DP_DIR}/makefile.inc" _mk)
# Fold the makefile's line continuations so each OBJ_ list is one line.
string(REPLACE "\\\n" " " _mk "${_mk}")

function(dp_objects var name)
    if(NOT _mk MATCHES "(^|\n)${name}=([^\n]*)")
        message(FATAL_ERROR "makefile.inc has no ${name} list")
    endif()
    string(REGEX MATCHALL "[A-Za-z0-9_]+\\.o" _objs "${CMAKE_MATCH_2}")
    set(_srcs "")
    foreach(obj IN LISTS _objs)
        string(REGEX REPLACE "\\.o$" ".c" src "${obj}")
        list(APPEND _srcs "${DP_DIR}/${src}")
    endforeach()
    set(${var} "${_srcs}" PARENT_SCOPE)
endfunction()

dp_objects(DP_COMMON OBJ_COMMON)
dp_objects(DP_MENU OBJ_MENU)
dp_objects(DP_SND OBJ_SND_COMMON)

# OBJ_SDL from makefile.inc, minus the pieces this build opts out of:
# OBJ_SND_XMP (no libxmp) and OBJ_VIDEO_CAPTURE — the capture path reads back
# frames through GL32-only pixel-pack buffers that GLES2 has no equivalent for.
set(DP_SDL
    "${DP_DIR}/builddate.c"
    "${DP_DIR}/sys_sdl.c"
    "${DP_DIR}/vid_sdl.c"
    "${DP_DIR}/thread_sdl.c"
    "${DP_DIR}/snd_sdl.c")

add_library(main SHARED ${DP_COMMON} ${DP_MENU} ${DP_SND} ${DP_SDL})

##### Prebuilt dependencies ####################################################

add_library(SDL2 SHARED IMPORTED)
set_target_properties(SDL2 PROPERTIES
    IMPORTED_LOCATION "${XONOTIC_DEPS_PREFIX}/lib/libSDL2.so")

foreach(lib jpeg freetype vorbisfile vorbis ogg)
    add_library(${lib} STATIC IMPORTED)
    set_target_properties(${lib} PROPERTIES
        IMPORTED_LOCATION "${XONOTIC_DEPS_PREFIX}/lib/lib${lib}.a")
endforeach()

target_include_directories(main PRIVATE
    "${DP_DIR}"
    "${XONOTIC_DEPS_PREFIX}/include"
    "${XONOTIC_DEPS_PREFIX}/include/SDL2")

target_compile_definitions(main PRIVATE
    CONFIG_MENU
    LINK_TO_LIBJPEG
    _FILE_OFFSET_BITS=64
    __KERNEL_STRICT_NAMES
    VCREVISION=${XONOTIC_VERSION}
    BUILDTYPE=release)

# sys.h already turns on USE_GLES2, USE_RWOPS, LINK_TO_ZLIB, LINK_TO_LIBVORBIS,
# DP_MOBILETOUCH and DP_FREETYPE_STATIC for __ANDROID__.
#
# SDLActivity calls SDL_main over JNI; darkplaces spells its entry point main().
set_source_files_properties("${DP_DIR}/sys_sdl.c" PROPERTIES
    COMPILE_DEFINITIONS "main=SDL_main")

target_compile_options(main PRIVATE
    -O3 -fno-math-errno -fno-trapping-math -fno-strict-aliasing
    -Wall -Wno-unused-but-set-variable)

# libpng is dlopen()ed at runtime rather than linked; scripts/android-build.sh
# stages it into jniLibs so it lands in the APK's lib/<abi>/ directory.
target_link_libraries(main
    SDL2
    jpeg freetype vorbisfile vorbis ogg
    z GLESv2 EGL OpenSLES android log m dl)
