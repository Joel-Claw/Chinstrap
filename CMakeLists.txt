# =========================================================================
# Chinstrap - A from-scratch web browser
# =========================================================================
# TEACHING NOTE: CMake is the most common build system for C++ projects.
# We use it here because it handles finding source files, setting compiler
# flags, and creating build targets. Real browsers like Chromium and
# Firefox use GN and ninja respectively, but CMake is the standard for
# most C++ projects and is easy to learn.
#
# This CMake config intentionally has ZERO external dependencies.
# No find_package for any library. No FetchContent. No ExternalProject.
# Just C++17 and POSIX. That is the Chinstrap way.
# =========================================================================

cmake_minimum_required(VERSION 3.10)

project(chinstrap
    VERSION 0.1.0
    DESCRIPTION "A from-scratch web browser with zero third-party libraries"
    LANGUAGES CXX
)

# -------------------------------------------------------------------------
# Compiler and linker settings
# -------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CHINSTRAP_WARNING_FLAGS
    -Wall
    -Wextra
    -Werror
    -Wpedantic
)

# -------------------------------------------------------------------------
# All source files for the browser library (everything except main.cpp)
# -------------------------------------------------------------------------
# TEACHING NOTE: We list every source file explicitly. This is the complete
# list of subsystems that make up the Chinstrap browser. Each subsystem is
# explained in detail in its source files and in docs/architecture.md.

set(CHINSTRAP_LIB_SOURCES
    # Core: URL parsing, HTTP client, HTML/CSS parsing, layout, rendering
    src/url.cpp
    src/http.cpp
    src/html_parser.cpp
    src/css_parser.cpp
    src/layout.cpp
    src/renderer.cpp
    src/json.cpp
    src/config.cpp

    # TLS: Encryption, certificates, secure connections
    src/sha256.cpp
    src/aes.cpp
    src/bigint.cpp
    src/x509.cpp
    src/tls.cpp

    # JavaScript engine: Lexer, parser, interpreter, GC, builtins, DOM
    src/js_lexer.cpp
    src/js_parser.cpp
    src/js_interpreter.cpp
    src/js_gc.cpp
    src/js_value.cpp
    src/js_builtins.cpp
    src/js_dom.cpp

    # GUI: Display, X11, fonts, widgets, input, images, zlib
    src/display.cpp
    src/x11.cpp
    src/font.cpp
    src/gui.cpp
    src/input.cpp
    src/image.cpp
    src/zlib.cpp

    # Networking: DNS, HTTP/2, cookies, cache, storage, history, downloads
    src/dns.cpp
    src/http2.cpp
    src/cookies.cpp
    src/cache.cpp
    src/storage.cpp
    src/history.cpp
    src/download.cpp
)

# Main binary sources (lib + entry point)
set(CHINSTRAP_SOURCES ${CHINSTRAP_LIB_SOURCES} src/main.cpp)

# The browser binary
add_executable(chinstrap ${CHINSTRAP_SOURCES})
target_include_directories(chinstrap PRIVATE src)
target_compile_options(chinstrap PRIVATE ${CHINSTRAP_WARNING_FLAGS})

# Link math library (part of C standard library, not third-party)
target_link_libraries(chinstrap PRIVATE m)

# -------------------------------------------------------------------------
# Test framework and tests
# -------------------------------------------------------------------------
enable_testing()

file(GLOB CHINSTRAP_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_url.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_http.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_html_parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_css_parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_json.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sha256.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aes.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_js_lexer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_js_parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_js_interpreter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_zlib.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_font.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_image.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dns.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cookies.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cache.cpp"
)

foreach(TEST_SOURCE ${CHINSTRAP_TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SOURCE} ${CHINSTRAP_LIB_SOURCES})
    target_include_directories(${TEST_NAME} PRIVATE src tests)
    target_compile_options(${TEST_NAME} PRIVATE ${CHINSTRAP_WARNING_FLAGS})
    target_link_libraries(${TEST_NAME} PRIVATE m)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# -------------------------------------------------------------------------
# Install targets
# -------------------------------------------------------------------------
install(TARGETS chinstrap DESTINATION bin)
install(DIRECTORY docs/ DESTINATION share/doc/chinstrap)

# -------------------------------------------------------------------------
# Status output
# -------------------------------------------------------------------------
message(STATUS "Chinstrap ${PROJECT_VERSION}")
message(STATUS "  Sources: ${CHINSTRAP_SOURCES}")
message(STATUS "  Tests: ${CHINSTRAP_TEST_SOURCES}")
message(STATUS "  C++ standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Warning flags: ${CHINSTRAP_WARNING_FLAGS}")