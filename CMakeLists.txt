cmake_minimum_required(VERSION 3.24)
project(vgz2flac VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Every dependency is downloaded as source at a fixed version and compiled
# into the program, so the result is one self-contained executable and no
# package manager is needed.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Some dependencies declare old CMake minimums: keep their option() calls from
# overriding the values set here, and keep newer CMake versions accepting them.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
	set(VGZ2FLAC_EXCLUDE EXCLUDE_FROM_ALL)
else()
	set(VGZ2FLAC_EXCLUDE "")
endif()

# ---------------------------------------------------------------------------
# zlib (reads .vgz). Compiled directly from its sources; find_package(ZLIB)
# inside libvgm is redirected to it.
# ---------------------------------------------------------------------------
FetchContent_Declare(ZLIB
	URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
	URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
	SOURCE_SUBDIR do-not-build-with-zlibs-own-cmake
	OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(ZLIB)
add_library(vgz2flac_zlib STATIC
	${zlib_SOURCE_DIR}/adler32.c ${zlib_SOURCE_DIR}/compress.c ${zlib_SOURCE_DIR}/crc32.c
	${zlib_SOURCE_DIR}/deflate.c ${zlib_SOURCE_DIR}/gzclose.c ${zlib_SOURCE_DIR}/gzlib.c
	${zlib_SOURCE_DIR}/gzread.c ${zlib_SOURCE_DIR}/gzwrite.c ${zlib_SOURCE_DIR}/infback.c
	${zlib_SOURCE_DIR}/inffast.c ${zlib_SOURCE_DIR}/inflate.c ${zlib_SOURCE_DIR}/inftrees.c
	${zlib_SOURCE_DIR}/trees.c ${zlib_SOURCE_DIR}/uncompr.c ${zlib_SOURCE_DIR}/zutil.c
)
target_include_directories(vgz2flac_zlib PUBLIC ${zlib_SOURCE_DIR})
if(MSVC)
	target_compile_definitions(vgz2flac_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
else()
	target_compile_definitions(vgz2flac_zlib PRIVATE HAVE_UNISTD_H)
endif()
# An imported wrapper (not an alias), so libvgm's export rules accept it.
add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
set_target_properties(ZLIB::ZLIB PROPERTIES
	INTERFACE_LINK_LIBRARIES vgz2flac_zlib
	INTERFACE_INCLUDE_DIRECTORIES ${zlib_SOURCE_DIR})
set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIR ${zlib_SOURCE_DIR})
set(ZLIB_INCLUDE_DIRS ${zlib_SOURCE_DIR})
set(ZLIB_LIBRARIES ZLIB::ZLIB)

# ---------------------------------------------------------------------------
# libFLAC (encoder)
# ---------------------------------------------------------------------------
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(flac
	URL https://github.com/xiph/flac/releases/download/1.4.3/flac-1.4.3.tar.xz
	${VGZ2FLAC_EXCLUDE}
)
FetchContent_MakeAvailable(flac)

# ---------------------------------------------------------------------------
# libsoxr (resampler)
# ---------------------------------------------------------------------------
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LSR_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(WITH_LSR_BINDINGS OFF CACHE BOOL "" FORCE)
set(WITH_VR32 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(soxr
	URL https://downloads.sourceforge.net/project/soxr/soxr-0.1.3-Source.tar.xz
	SOURCE_SUBDIR added-below
)
FetchContent_MakeAvailable(soxr)
# soxr looks for its CMake modules under CMAKE_SOURCE_DIR (it assumes it is
# the top-level project), so point it at them and add it by hand.
list(APPEND CMAKE_MODULE_PATH ${soxr_SOURCE_DIR}/cmake/Modules)
add_subdirectory(${soxr_SOURCE_DIR} ${soxr_BINARY_DIR} EXCLUDE_FROM_ALL)
target_include_directories(soxr INTERFACE ${soxr_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# libvgm (sound chip emulation + VGM player), fetched at a fixed commit.
# To build against a local checkout instead, configure with
#   -DFETCHCONTENT_SOURCE_DIR_LIBVGM=<path to libvgm>
# ---------------------------------------------------------------------------
set(BUILD_LIBAUDIO OFF CACHE BOOL "" FORCE)	# no sound card output needed
set(BUILD_LIBEMU ON CACHE BOOL "" FORCE)
set(BUILD_LIBPLAYER ON CACHE BOOL "" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "" FORCE)
set(USE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
if(WIN32)
	# Use the Windows API for the UTF-16 -> UTF-8 tag conversion, not the
	# prebuilt libiconv import library that libvgm bundles.
	set(UTIL_CHARCNV_WINAPI ON CACHE BOOL "" FORCE)
	set(UTIL_CHARCNV_ICONV OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(libvgm
	GIT_REPOSITORY https://github.com/ValleyBell/libvgm.git
	GIT_TAG c5c03a2c2514d38d2ab7b7e8dbd021af0b8292cb
	${VGZ2FLAC_EXCLUDE}
)
FetchContent_MakeAvailable(libvgm)

# ---------------------------------------------------------------------------
# The converter
# ---------------------------------------------------------------------------
# The conversion engine, shared by the window and the command-line program.
add_library(vgz2flac-core STATIC src/converter.cpp src/converter.hpp)
target_include_directories(vgz2flac-core PUBLIC src)
target_compile_definitions(vgz2flac-core PUBLIC
	VGZ2FLAC_VERSION="${PROJECT_VERSION}"
	# must match how libvgm itself was compiled (type sizes and byte order)
	HAVE_STDINT_H
	VGM_LITTLE_ENDIAN
	FLAC__NO_DLL
)
target_link_libraries(vgz2flac-core PRIVATE vgm-player vgm-emu vgm-utils FLAC::FLAC soxr)
if(WIN32)
	target_compile_definitions(vgz2flac-core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
		_CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
endif()
if(MSVC)
	target_compile_options(vgz2flac-core PUBLIC /utf-8 /W3)
elseif(NOT MINGW)
	target_compile_options(vgz2flac-core PRIVATE -Wall -Wextra)
endif()

if(WIN32)
	# Windows: vgz2flac.exe is the window, vgz2flac-cli.exe the command-line tool.
	add_executable(vgz2flac WIN32 src/gui_win.cpp)
	target_link_libraries(vgz2flac PRIVATE vgz2flac-core comctl32 shell32 ole32)
	add_executable(vgz2flac-cli src/main.cpp)
	target_link_libraries(vgz2flac-cli PRIVATE vgz2flac-core)
	if(MINGW)
		target_link_options(vgz2flac PRIVATE -municode -static)
		target_link_options(vgz2flac-cli PRIVATE -municode -static)
	endif()
	install(TARGETS vgz2flac vgz2flac-cli RUNTIME DESTINATION bin)
else()
	add_executable(vgz2flac src/main.cpp)
	target_link_libraries(vgz2flac PRIVATE vgz2flac-core)
	install(TARGETS vgz2flac RUNTIME DESTINATION bin)
endif()
