cmake_minimum_required(VERSION 3.20)

project(TinyX VERSION 1.3 LANGUAGES C)

# TinyX retains K&R-style definitions that newer C modes reject.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS YES)

include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)
include(TestBigEndian)

option(TINYX_BUILD_TESTS "Build TinyX embedding tests" ${BUILD_TESTING})
option(TINYX_BUILD_EXAMPLE "Build the native embedding example" ON)
option(TINYX_BUILD_WASM "Build the Emscripten module" ${EMSCRIPTEN})
set(TINYX_XORGPROTO_INCLUDE_DIR "" CACHE PATH
    "Directory containing the X11 protocol headers (X11/Xproto.h)")
set(TINYX_XTRANS_INCLUDE_DIR "" CACHE PATH
    "Directory containing Xtrans headers (X11/Xtrans/Xtrans.h)")

if(NOT TINYX_XORGPROTO_INCLUDE_DIR)
    find_path(TINYX_XORGPROTO_INCLUDE_DIR X11/Xproto.h)
endif()
if(NOT TINYX_XORGPROTO_INCLUDE_DIR)
    message(FATAL_ERROR
        "X.Org protocol headers were not found. Set TINYX_XORGPROTO_INCLUDE_DIR "
        "to the xorgproto include directory.")
endif()

# Xtrans is needed only for native socket support. Emscripten builds contain
# descriptor-free clients and do not inspect native pkg-config metadata.
if(NOT EMSCRIPTEN)
    if(NOT TINYX_XTRANS_INCLUDE_DIR)
        find_path(TINYX_XTRANS_INCLUDE_DIR X11/Xtrans/Xtrans.h)
    endif()
    if(NOT TINYX_XTRANS_INCLUDE_DIR)
        message(FATAL_ERROR
            "Xtrans headers were not found. Set TINYX_XTRANS_INCLUDE_DIR to "
            "their include directory.")
    endif()
endif()

set(CMAKE_REQUIRED_QUIET TRUE)
check_include_file(alloca.h HAVE_ALLOCA_H)
check_include_file(dirent.h HAVE_DIRENT_H)
check_include_file(fcntl.h HAVE_FCNTL_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(memory.h HAVE_MEMORY_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(stdlib.h HAVE_STDLIB_H)
check_include_file(strings.h HAVE_STRINGS_H)
check_include_file(string.h HAVE_STRING_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_symbol_exists(getdtablesize "unistd.h" HAS_GETDTABLESIZE)
check_symbol_exists(getifaddrs "ifaddrs.h" HAS_GETIFADDRS)
check_symbol_exists(geteuid "unistd.h" HAVE_GETEUID)
check_symbol_exists(getuid "unistd.h" HAVE_GETUID)
check_symbol_exists(getopt "unistd.h" HAVE_GETOPT)
check_symbol_exists(getopt_long "getopt.h" HAVE_GETOPT_LONG)
check_symbol_exists(link "unistd.h" HAVE_LINK)
check_symbol_exists(memmove "string.h" HAVE_MEMMOVE)
check_symbol_exists(memset "string.h" HAVE_MEMSET)
check_symbol_exists(mkstemp "stdlib.h" HAVE_MKSTEMP)
check_symbol_exists(strcasecmp "strings.h" HAVE_STRCASECMP)
check_symbol_exists(strchr "string.h" HAVE_STRCHR)
check_symbol_exists(strrchr "string.h" HAVE_STRRCHR)
check_symbol_exists(strtol "stdlib.h" HAVE_STRTOL)
check_symbol_exists(vprintf "stdio.h" HAVE_VPRINTF)
check_symbol_exists(vsnprintf "stdio.h" HAVE_VSNPRINTF)
check_symbol_exists(mmap "sys/mman.h" HAS_MMAP)
check_symbol_exists(clock_gettime "time.h" HAVE_CLOCK_GETTIME)
check_type_size("unsigned long" SIZEOF_UNSIGNED_LONG LANGUAGE C)
test_big_endian(TINYX_BIG_ENDIAN)

if(HAVE_CLOCK_GETTIME)
    set(MONOTONIC_CLOCK 1)
endif()
if(SIZEOF_UNSIGNED_LONG EQUAL 8)
    set(_XSERVER64 1)
endif()
if(NOT HAVE_STRCASECMP)
    set(NEED_STRCASECMP 1)
endif()
if(TINYX_BIG_ENDIAN)
    set(X_BYTE_ORDER X_BIG_ENDIAN)
else()
    set(X_BYTE_ORDER X_LITTLE_ENDIAN)
endif()

set(BUILDERADDR "xorg@lists.freedesktop.org")
set(OSNAME "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
set(COMPILEDDEFAULTFONTPATH "built-ins")
set(BASE_FONT_PATH "")
set(XVENDORNAME "TinyCore Linux")
set(XVENDORNAMESHORT "TinyCore")
set(XORG_RELEASE "Release 1.0")
set(XORG_DATE "22 Sep 2015")
set(XORG_MAN_VERSION "Version 1.0")
set(PACKAGE_STRING "tinyx ${PROJECT_VERSION}")
set(PROJECTROOT "${CMAKE_INSTALL_PREFIX}")
set(TINYX_EMBEDDED_FONTS 1)
if(NOT EMSCRIPTEN)
    set(SMART_SCHEDULE 1)
endif()
set(KDRIVESERVER 1)
set(KDRIVEDDXACTIONS 1)
set(DBE 1)
set(RES 1)
set(SCREENSAVER 1)
if(NOT EMSCRIPTEN)
    set(XF86BIGFONT 1)
    set(TCPCONN 1)
    set(UNIXCONN 1)
    set(IPv6 1)
else()
    # Emscripten has no SysV shared memory or descriptor transports.
    set(HAS_GETDTABLESIZE "")
endif()
set(XResExtension 1)
# The legacy DEBUG macro enables incomplete XKB debugging code in this tree;
# do not infer it from CMake's optimization/debug-info configuration.
set(NDEBUG 1)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file(cmake/dix-config.h.in include/dix-config.h)
configure_file(cmake/kdrive-config.h.in include/kdrive-config.h)

set(TINYX_INCLUDE_DIRS
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/dix"
    "${CMAKE_CURRENT_SOURCE_DIR}/fb"
    "${CMAKE_CURRENT_SOURCE_DIR}/mi"
    "${CMAKE_CURRENT_SOURCE_DIR}/Xext"
    "${CMAKE_CURRENT_SOURCE_DIR}/render"
    "${CMAKE_CURRENT_SOURCE_DIR}/randr"
    "${CMAKE_CURRENT_SOURCE_DIR}/xfixes"
    "${CMAKE_CURRENT_SOURCE_DIR}/damageext"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbe"
    "${CMAKE_CURRENT_SOURCE_DIR}/miext/damage"
    "${CMAKE_CURRENT_SOURCE_DIR}/miext/shadow"
    "${CMAKE_CURRENT_SOURCE_DIR}/kdrive/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/kdrive/memory"
    "${CMAKE_CURRENT_SOURCE_DIR}/os"
    "${TINYX_XORGPROTO_INCLUDE_DIR}"
    "${TINYX_XTRANS_INCLUDE_DIR}")

set(TINYX_DIX_SOURCES
    dix/atom.c dix/colormap.c dix/cursor.c dix/devices.c dix/dispatch.c
    dix/dixfonts.c dix/fontstubs.c dix/embedded-font.c
    dix/embedded-font-data.c dix/dixutils.c dix/events.c dix/extension.c
    dix/ffs.c dix/gc.c dix/globals.c dix/glyphcurs.c dix/grabs.c
    dix/initatoms.c dix/lifecycle.c dix/main.c dix/main-entry.c dix/pixmap.c
    dix/privates.c dix/property.c dix/resource.c dix/swaprep.c dix/swapreq.c
    dix/tables.c dix/window.c dix/strcasecmp.c)

set(TINYX_MI_SOURCES
    mi/miarc.c mi/mibank.c mi/mibitblt.c mi/micmap.c mi/micursor.c
    mi/midash.c mi/midispcur.c mi/mieq.c mi/miexpose.c mi/mifillarc.c
    mi/mifillrct.c mi/mifpolycon.c mi/migc.c mi/miglblt.c mi/mipointer.c
    mi/mipoly.c mi/mipolycon.c mi/mipolygen.c mi/mipolypnt.c
    mi/mipolyrect.c mi/mipolyseg.c mi/mipolytext.c mi/mipolyutil.c
    mi/mipushpxl.c mi/miregion.c mi/miscrinit.c mi/mispans.c mi/misprite.c
    mi/mivaltree.c mi/miwideline.c mi/miwindow.c mi/mizerarc.c
    mi/mizerclip.c mi/mizerline.c)

set(TINYX_FB_SOURCES
    fb/fb24_32.c fb/fballpriv.c fb/fbarc.c fb/fbbits.c fb/fbblt.c
    fb/fbbltone.c fb/fbcompose.c fb/fbcopy.c fb/fbedge.c fb/fbfill.c
    fb/fbfillrect.c fb/fbfillsp.c fb/fbgc.c fb/fbgetsp.c fb/fbglyph.c
    fb/fbimage.c fb/fbline.c fb/fboverlay.c fb/fbpict.c fb/fbpixmap.c
    fb/fbpoint.c fb/fbpush.c fb/fbscreen.c fb/fbseg.c fb/fbsetsp.c
    fb/fbsolid.c fb/fbstipple.c fb/fbtile.c fb/fbtrap.c fb/fbutil.c
    fb/fbwindow.c)

set(TINYX_EXTENSION_SOURCES
    Xext/bigreq.c Xext/shape.c Xext/sleepuntil.c Xext/sync.c
    Xext/xcmisc.c Xext/xtest.c Xext/xres.c Xext/saver.c
    dbe/dbe.c dbe/midbe.c
    render/animcur.c render/filter.c render/glyph.c render/miglyph.c
    render/miindex.c render/mipict.c render/mirect.c render/mitrap.c
    render/mitri.c render/picture.c render/render.c render/renderedge.c
    randr/mirandr.c randr/randr.c randr/rrcrtc.c randr/rrdispatch.c
    randr/rrinfo.c randr/rrmode.c randr/rroutput.c randr/rrpointer.c
    randr/rrproperty.c randr/rrscreen.c randr/rrsdispatch.c randr/rrxinerama.c
    xfixes/cursor.c xfixes/region.c xfixes/saveset.c xfixes/select.c
    xfixes/xfixes.c damageext/damageext.c miext/damage/damage.c)
if(NOT EMSCRIPTEN)
    list(APPEND TINYX_EXTENSION_SOURCES Xext/shm.c Xext/xf86bigfont.c)
endif()

set(TINYX_SHADOW_SOURCES
    miext/shadow/shadow.c miext/shadow/shalloc.c miext/shadow/shpacked.c
    miext/shadow/shplanar8.c miext/shadow/shplanar.c
    miext/shadow/shrot16pack_180.c miext/shadow/shrot16pack_270.c
    miext/shadow/shrot16pack_270YX.c miext/shadow/shrot16pack_90.c
    miext/shadow/shrot16pack_90YX.c miext/shadow/shrot16pack.c
    miext/shadow/shrot32pack_180.c miext/shadow/shrot32pack_270.c
    miext/shadow/shrot32pack_90.c miext/shadow/shrot32pack.c
    miext/shadow/shrot8pack_180.c miext/shadow/shrot8pack_270.c
    miext/shadow/shrot8pack_90.c miext/shadow/shrot8pack.c
    miext/shadow/shrotate.c)

set(TINYX_OS_SOURCES
    os/WaitFor.c os/connection.c os/host.c os/io.c os/memory.c
    os/transport.c os/oscolor.c os/osinit.c os/utils.c os/log.c)
if(EMSCRIPTEN)
    list(APPEND TINYX_OS_SOURCES os/embedded-security.c)
else()
    list(APPEND TINYX_OS_SOURCES
        os/access.c os/auth.c os/mitauth.c os/xdmauth.c os/xstrans.c
        os/xauembed.c)
endif()

set(TINYX_MEMORY_HOST_SOURCES
    kdrive/src/kcmap.c kdrive/src/kdrive.c kdrive/src/kinfo.c
    kdrive/src/kinput.c kdrive/src/kmap.c kdrive/src/kmode.c
    kdrive/src/kshadow.c mi/miinitext.c fb/fbcmap.c
    kdrive/memory/api.c kdrive/memory/memory.c kdrive/memory/meminit.c)

add_library(tinyx-core OBJECT
    ${TINYX_DIX_SOURCES} ${TINYX_MI_SOURCES} ${TINYX_FB_SOURCES}
    ${TINYX_EXTENSION_SOURCES} ${TINYX_SHADOW_SOURCES} ${TINYX_OS_SOURCES})
add_library(tinyx-host-memory OBJECT ${TINYX_MEMORY_HOST_SOURCES})

foreach(target tinyx-core tinyx-host-memory)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${target} PRIVATE
        HAVE_DIX_CONFIG_H HAVE_KDRIVE_CONFIG_H
        VENDOR_STRING=\"TinyCore_Linux\" VENDOR_RELEASE=10000000)
    if(EMSCRIPTEN)
        target_compile_definitions(${target} PRIVATE
            TINYX_MEMORY_ONLY TINYX_NO_MITSHM)
    endif()
    target_include_directories(${target} PRIVATE ${TINYX_INCLUDE_DIRS})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wno-unused-parameter -fvisibility=hidden)
    endif()
endforeach()

add_library(tinyx STATIC
    $<TARGET_OBJECTS:tinyx-core>
    $<TARGET_OBJECTS:tinyx-host-memory>)
add_library(TinyX::tinyx ALIAS tinyx)
set_target_properties(tinyx PROPERTIES OUTPUT_NAME tinyx)
target_include_directories(tinyx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(NOT EMSCRIPTEN)
    target_link_libraries(tinyx PUBLIC m)
endif()

if(TINYX_BUILD_EXAMPLE AND NOT EMSCRIPTEN)
    add_executable(tinyx-embed-example kdrive/memory/embed-example.c)
    target_link_libraries(tinyx-embed-example PRIVATE tinyx)
endif()

if(TINYX_BUILD_TESTS)
    add_executable(tinyx-api-test kdrive/memory/api-test.c)
    target_link_libraries(tinyx-api-test PRIVATE tinyx)
    add_test(NAME tinyx-api COMMAND tinyx-api-test)

    add_executable(tinyx-big-endian-test kdrive/memory/big-endian-test.c)
    target_link_libraries(tinyx-big-endian-test PRIVATE tinyx)
    add_test(NAME tinyx-big-endian COMMAND tinyx-big-endian-test)

    add_executable(tinyx-embedded-font-data-test
        dix/tests/embedded-font-data-test.c)
    target_include_directories(tinyx-embedded-font-data-test PRIVATE
        ${TINYX_INCLUDE_DIRS})
    target_compile_definitions(tinyx-embedded-font-data-test PRIVATE
        HAVE_DIX_CONFIG_H)
    target_link_libraries(tinyx-embedded-font-data-test PRIVATE tinyx)
    add_test(NAME tinyx-embedded-font-data COMMAND tinyx-embedded-font-data-test)
endif()

if(TINYX_BUILD_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "TINYX_BUILD_WASM requires the Emscripten toolchain")
    endif()
    add_executable(tinyx-wasm cmake/wasm-entry.c)
    target_link_libraries(tinyx-wasm PRIVATE tinyx)
    set_target_properties(tinyx-wasm PROPERTIES SUFFIX ".js")
    target_link_options(tinyx-wasm PRIVATE
        "--no-entry"
        "-sALLOW_TABLE_GROWTH=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction,HEAPU8"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createTinyXModule"
        "-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/cmake/wasm-exports.json")
endif()

set(TINYX_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/TinyX")
configure_package_config_file(cmake/TinyXConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/TinyXConfig.cmake"
    INSTALL_DESTINATION "${TINYX_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TinyXConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(TARGETS tinyx EXPORT TinyXTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES include/tinyx.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TinyXConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TinyXConfigVersion.cmake"
    DESTINATION "${TINYX_CMAKE_INSTALL_DIR}")
install(EXPORT TinyXTargets NAMESPACE TinyX:: DESTINATION
    "${TINYX_CMAKE_INSTALL_DIR}")
