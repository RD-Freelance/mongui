cmake_minimum_required(VERSION 3.16)
project(mongui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(mongui
    src/main.cpp
    src/term.cpp
    src/mongo.cpp
    src/app.cpp
)

# ---------------------------------------------------------------------------
# Locate the MongoDB C driver. Two discovery paths:
#   1. CMake package config (mongoc-1.0) — used by vcpkg / Windows and by
#      source installs of the driver. Provides imported targets.
#   2. pkg-config — used by Homebrew (macOS) and most Linux distro packages.
# ---------------------------------------------------------------------------
find_package(mongoc-1.0 CONFIG QUIET)

if(mongoc-1.0_FOUND)
    # vcpkg / CMake-config install. Prefer shared, fall back to static targets.
    if(TARGET mongo::mongoc_shared)
        target_link_libraries(mongui PRIVATE mongo::mongoc_shared)
    else()
        target_link_libraries(mongui PRIVATE mongo::mongoc_static)
    endif()
    message(STATUS "mongui: using mongoc-1.0 CMake package")
else()
    # Help pkg-config find Homebrew's mongo-c-driver on Apple Silicon.
    if(APPLE AND EXISTS "/opt/homebrew/lib/pkgconfig")
        set(ENV{PKG_CONFIG_PATH}
            "/opt/homebrew/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()

    find_package(PkgConfig REQUIRED)
    # mongo-c-driver 2.x ships "mongoc2"; legacy 1.x ships "libmongoc-1.0".
    pkg_check_modules(MONGOC QUIET IMPORTED_TARGET mongoc2)
    if(NOT MONGOC_FOUND)
        pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0)
    endif()
    target_link_libraries(mongui PRIVATE PkgConfig::MONGOC)
    message(STATUS "mongui: using mongo-c-driver via pkg-config (${MONGOC_MODULE_NAME})")
endif()

target_compile_definitions(mongui PRIVATE
    MONGUI_VERSION="${PROJECT_VERSION}"
)

# ---------------------------------------------------------------------------
# Compiler-specific options.
# ---------------------------------------------------------------------------
if(MSVC)
    # /utf-8 makes the UTF-8 box-drawing string literals compile correctly.
    target_compile_options(mongui PRIVATE /W3 /utf-8 /EHsc)
    target_compile_definitions(mongui PRIVATE
        _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX)
else()
    target_compile_options(mongui PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
    # -Wno-align-mismatch is Clang-only; GCC doesn't know it.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(mongui PRIVATE -Wno-align-mismatch)
    endif()
endif()

# Copy dependent DLLs next to the .exe on Windows so it runs standalone.
if(WIN32)
    add_custom_command(TARGET mongui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E true
        COMMENT "mongui: built mongui.exe")
endif()

install(TARGETS mongui RUNTIME DESTINATION bin)
