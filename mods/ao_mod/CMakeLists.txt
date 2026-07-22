cmake_minimum_required(VERSION 3.25)
project(ao_mod CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to dusk source root")
    option(DUSK_MOD_USE_FULL_TREE "Use full build instead of the minimal mod SDK" OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if (DUSK_MOD_USE_FULL_TREE)
        add_subdirectory("${DUSK_DIR}" dusk EXCLUDE_FROM_ALL)
    else ()
        add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
    endif ()
endif ()

add_mod(ao_mod
    FEATURES webgpu
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
    BUNDLE
)

# The root project already includes ao_mod in its aggregate in-tree mod build. Register the
# cel-shading companion there as well without affecting standalone ao_mod builds.
if (TARGET dusklight_mods AND NOT TARGET celshade_mod)
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../celshade_mod"
        "${CMAKE_CURRENT_BINARY_DIR}/../celshade_mod"
    )
endif ()
