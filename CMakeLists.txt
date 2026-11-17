cmake_minimum_required(VERSION 3.20)

# The version lives in the public header (so re-configure when it changes).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS include/misa77/misa77.h)
file(STRINGS include/misa77/misa77.h MISA77_VERSION_DEFINES
     REGEX "#define MISA77_VERSION_(MAJOR|MINOR|PATCH) ")
string(REGEX REPLACE "[^;]*_(MAJOR|MINOR|PATCH) +([0-9]+)[^;]*" "\\2"
       MISA77_VERSION "${MISA77_VERSION_DEFINES}")
string(REPLACE ";" "." MISA77_VERSION "${MISA77_VERSION}")

project(misa77 VERSION ${MISA77_VERSION} LANGUAGES CXX)

# compile_commands.json for clangd / IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type" FORCE)
endif()

# Pass -DMISA77_MARCH=native for a maximal build run on the same machine.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(MISA77_MARCH_DEFAULT "x86-64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(MISA77_MARCH_DEFAULT "armv8-a")
else()
    set(MISA77_MARCH_DEFAULT "")
endif()
set(MISA77_MARCH "${MISA77_MARCH_DEFAULT}" CACHE STRING "-march for the misa77 dispatchers/CLI")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(misa77
    src/codebook.h
    src/format.h
    src/util.h
    src/suffix/sais.h
    src/decompressor_zoo/light_unsafe_dimpl.h
    src/decompressor_zoo/light_safe_dimpl.h
    src/decompressor_zoo/heavy_unsafe_dimpl.h
    src/decompressor_zoo/heavy_safe_dimpl.h
    src/decompress_dispatch.h
    src/compressor_zoo/light_blitz_cimpl.h
    src/compressor_zoo/light_swift_cimpl.h
    src/compressor_zoo/light_loose_cimpl.h
    src/compressor_zoo/light_keen_cimpl.h
    src/compressor_zoo/light_optimal_cimpl.h
    src/compressor_zoo/heavy_optimal_cimpl.h
    src/compress_dispatch.h
    src/compress.cpp
    src/decompress.cpp
    src/isa/lib_avx2.h
    src/isa/lib_sse2.h
    src/isa/lib_neon.h
    src/isa/lib_portable.h
    src/isa/target_portable.cpp
    src/experimental/params.h
    src/experimental/ecompress_dispatch.h
    src/experimental/ecompress_impl.h
    src/experimental/ecompress.cpp
    src/experimental/isa/etarget_portable.cpp)

add_library(misa77::misa77 ALIAS misa77)

target_include_directories(misa77 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(misa77 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(misa77 PUBLIC cxx_std_20)
target_compile_options(misa77 PRIVATE -Wall -Wextra -Wpedantic)
if(MISA77_MARCH)
    target_compile_options(misa77 PRIVATE -march=${MISA77_MARCH})
endif()

# The AVX2/SSE2 codec TUs use x86 intrinsics, so they build only on 64-bit x86,
# each pinned to its own ISA. These pins are independent of MISA77_MARCH so the
# fallbacks stay genuine: a non-AVX2 CPU must be able to run the SSE2/portable
# path, so it must not inherit -mavx2/-march=native. (For the same reason, do NOT
# enable LTO on this library).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_sources(misa77 PRIVATE
        src/isa/target_avx2.cpp
        src/isa/target_sse2.cpp
        src/experimental/isa/etarget_avx2.cpp
        src/experimental/isa/etarget_sse2.cpp)
    set_source_files_properties(
        src/isa/target_avx2.cpp src/experimental/isa/etarget_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(
        src/isa/target_sse2.cpp src/isa/target_portable.cpp
        src/experimental/isa/etarget_sse2.cpp src/experimental/isa/etarget_portable.cpp
        PROPERTIES COMPILE_OPTIONS "-march=x86-64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    target_sources(misa77 PRIVATE src/isa/target_neon.cpp)
    set_source_files_properties(
        src/isa/target_neon.cpp src/isa/target_portable.cpp
        src/experimental/isa/etarget_portable.cpp
        PROPERTIES COMPILE_OPTIONS "-march=armv8-a")
endif()

option(MISA77_BUILD_CLI "Build the misa command-line tool" ON)
if(MISA77_BUILD_CLI)
    add_executable(misa77_cli app/misa77_cli.cpp)
    set_target_properties(misa77_cli PROPERTIES OUTPUT_NAME misa)
    target_link_libraries(misa77_cli PRIVATE misa77)
    if(MISA77_MARCH)
        target_compile_options(misa77_cli PRIVATE -march=${MISA77_MARCH})
    endif()
    install(TARGETS misa77_cli)
endif()

# Install rules, so that a consumer can do:
#   find_package(misa77 CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE misa77::misa77)
# misa77 has no dependencies, so the exported targets file is the whole package config.
install(TARGETS misa77 EXPORT misa77Config)
install(DIRECTORY include/misa77 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT misa77Config
        NAMESPACE misa77::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misa77)
# The formats are unstable before 1.0, so a request for 0.7 is not met by 0.8.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/misa77ConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/misa77ConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/misa77)

option(MISA77_BUILD_TESTS "Build misa77 tests" ON)
if(MISA77_BUILD_TESTS)
    enable_testing()
    add_executable(misa77_roundtrip tests/roundtrip.cpp)
    target_link_libraries(misa77_roundtrip PRIVATE misa77)
    # the hand-built level-4 streams need the codebooks
    target_include_directories(misa77_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME roundtrip COMMAND misa77_roundtrip)
endif()
