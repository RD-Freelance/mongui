cmake_minimum_required(VERSION 3.16)
project(mongui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Help pkg-config find Homebrew's mongo-c-driver on Apple Silicon by default.
if(APPLE AND EXISTS "/opt/homebrew/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH}
        "/opt/homebrew/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

find_package(PkgConfig REQUIRED)

# mongo-c-driver 2.x is "mongoc2"; legacy 1.x is "libmongoc-1.0". Try new first.
pkg_check_modules(MONGOC QUIET IMPORTED_TARGET mongoc2)
if(NOT MONGOC_FOUND)
    pkg_check_modules(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0)
endif()

add_executable(mongui
    src/main.cpp
    src/term.cpp
    src/mongo.cpp
    src/app.cpp
)

target_link_libraries(mongui PRIVATE PkgConfig::MONGOC)

target_compile_definitions(mongui PRIVATE
    MONGUI_VERSION="${PROJECT_VERSION}"
)

target_compile_options(mongui PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-align-mismatch
)
