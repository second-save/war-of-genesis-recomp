cmake_minimum_required(VERSION 3.20)
project(TheWarOfGenesis LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type" FORCE)
endif()

# Find SDL2 (support brew, standard system paths, and config mode)
if(APPLE AND EXISTS "/opt/homebrew")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(SDL2 REQUIRED CONFIG)

# Macro to configure recompiled/generated code targets
function(genesis_configure_generated TARGET_NAME)
    target_compile_features(${TARGET_NAME} PUBLIC cxx_std_20)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE
            /O2 /W3 /wd4101 /wd4102 /wd4189 /wd4244 /wd4267 /wd4702
        )
    else()
        target_compile_options(${TARGET_NAME} PRIVATE
            -O3
            -Wall -Wno-unused-variable -Wno-unused-label
            -Wno-unused-but-set-variable -Wno-tautological-compare
            -Wno-parentheses-equality -Wno-unused-function
        )
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Runtime library
# -----------------------------------------------------------------------------
add_library(genesis_runtime STATIC
    src/runtime/machine.cpp
    src/runtime/pm32.cpp
    src/runtime/settings.cpp
    src/subsystems/video.cpp
    src/subsystems/audio.cpp
    src/subsystems/input.cpp
    src/subsystems/dos.cpp
    src/subsystems/font_vwf.cpp
    src/subsystems/opl/opl3.c
)

target_include_directories(genesis_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(genesis_runtime PUBLIC
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

if(NOT MSVC)
    target_compile_options(genesis_runtime PRIVATE -O3 -Wall -Wextra)
endif()

# -----------------------------------------------------------------------------
# Generated lifted modules
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/generated/main/CMakeLists.txt")
    add_subdirectory(src/modules/generated/main)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/generated/chp/CMakeLists.txt")
    add_subdirectory(src/modules/generated/chp)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/generated/pack/CMakeLists.txt")
    add_subdirectory(src/modules/generated/pack)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/generated/ttl/CMakeLists.txt")
    add_subdirectory(src/modules/generated/ttl)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/generated/chrsys/CMakeLists.txt")
    add_subdirectory(src/modules/generated/chrsys)
endif()

# -----------------------------------------------------------------------------
# Main Executable
# -----------------------------------------------------------------------------
add_executable(genesis
    src/main.cpp
    src/runtime/text.cpp
)

target_link_libraries(genesis PRIVATE
    genesis_runtime
    $<$<TARGET_EXISTS:lifted_main>:lifted_main>
    $<$<TARGET_EXISTS:lifted_chp>:lifted_chp>
    $<$<TARGET_EXISTS:lifted_pack>:lifted_pack>
    $<$<TARGET_EXISTS:lifted_chrsys>:lifted_chrsys>
    $<$<TARGET_EXISTS:lifted_ttl>:lifted_ttl>
)

if(APPLE)
    # Ensure macOS bundle/rpath behavior is clean
    set_target_properties(genesis PROPERTIES
        MACOSX_RPATH TRUE
    )
endif()
