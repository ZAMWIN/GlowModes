cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(PerGamemodeGlow VERSION 1.0.0)

# All sources under src/ (recursive)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

# ---------------------------------------------------------------------------
# Geometry Dash target version.
#
# The installed game is GD 2.2091. The Geode 5.11.2 SDK defaults to 2.2081, so
# the generated bindings would carry 2.2081 addresses and signatures. Those do
# not match a 2.2091 binary: PlayerObject::update lives at 0x4e6fc0 (not
# 0x388d80), a few cocos2d exports changed signature (for example
# DS_Dictionary::loadRootSubDictFromCompressedFile gained a bool, and
# CCTouchDispatcher::registerForcePrio gained a third int), and the member
# offsets of the game classes can drift. Building against the 2.2091 binding
# data fixes all of that, so pin it here instead of relying on the SDK default.
#
# GEODE_COMP_GD_VERSION is the same version as a plain integer
# ("2.2091" -> 22091). The SDK only sets both values together inside an
# `if (NOT DEFINED GEODE_GD_VERSION)` guard, so when GEODE_GD_VERSION is set
# from the outside GEODE_COMP_GD_VERSION must be set here as well - otherwise
# the bindings headers see `#if GEODE_COMP_GD_VERSION < 22080` with an empty
# macro and MSVC rejects it with C1017.
# ---------------------------------------------------------------------------
if (NOT DEFINED GEODE_GD_VERSION)
    set(GEODE_GD_VERSION 2.2091)
endif()
if (NOT DEFINED GEODE_COMP_GD_VERSION)
    set(GEODE_COMP_GD_VERSION 22091)
endif()

# The SDK compiles mod targets with a precompiled header that contains
# <Geode/Geode.hpp> (CMakeLists.txt of the SDK, target_precompile_headers near the
# end). That forces Geode's UI headers into every translation unit, and Geode
# 5.11.2's UI classes declare `scrollWheel` as 2.2081's `void` while the 2.2091
# cocos2d declarations in sdk-compat have `bool`, which MSVC rejects with C2555.
# This mod does not use Geode's UI at all, so the PCH is switched off and
# src/main.cpp includes the umbrellas it needs explicitly. Has to be set before
# add_subdirectory() because the SDK only applies its default under
# `if (NOT DEFINED GEODE_DISABLE_PRECOMPILED_HEADERS)`.
set(GEODE_DISABLE_PRECOMPILED_HEADERS ON)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define the GEODE_SDK environment variable to point to the Geode SDK.")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# ---------------------------------------------------------------------------
# Local build workaround for the generated Geode bindings.
#
# The files the bindings codegen emits under build/bindings/bindings/Geode only
# include the headers of their *base classes*. They do not include the headers of
# types they reference in other ways, for example a by-value member such as
#
#     SequenceTriggerState m_sequenceState;      // binding/SequenceTriggerGameObject.hpp
#
# or a tulip wrapper parameter such as
#
#     void loadFromState(EffectManagerState& state);   // source/GJEffectManager.cpp
#
# Those only compile when <Geode/Bindings.hpp> has been seen first, which is what
# the SDK arranges with
#
#     target_precompile_headers(GeodeBindings INTERFACE .../Geode/Bindings.hpp)
#
# That is an INTERFACE property, so it reaches targets that *link* GeodeBindings
# (this mod) but never GeodeBindings' own generated sources, which is where the
# errors come from. Forcing the same header in for GeodeBindings itself fixes it.
# <Geode/Bindings.hpp> is already on the include path, so /FI can use its
# relative name (which also avoids quoting problems from spaces in the SDK path).
if (TARGET GeodeBindings)
    if (MSVC)
        target_compile_options(GeodeBindings PRIVATE "/FIGeode/Bindings.hpp")
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(GeodeBindings PRIVATE "-include" "Geode/Bindings.hpp")
    endif()
endif()

# ---------------------------------------------------------------------------
# GD 2.2091 cocos2d declarations, staged by tools/sync-2.2091-headers.py.
#
# The mod is compiled against the *released* Geode SDK: every `GEODE_DLL` symbol
# the mod imports has to exist in the installed Geode.dll, because that is the
# loader the user runs. That SDK's cocos2d headers, however, still describe GD
# 2.2081, while the binding data for this game version (2.2091) needs the 2.2091
# declarations: two new mixins used as base classes, changed signatures, extra
# fields and extra virtuals (the script's docstring lists them).
#
# sdk-compat/ therefore holds every cocos2d header that differs between the SDK
# and Geode's 2.2091 branch ("v6"), and is put first on the include path so the
# compiler prefers them over the SDK's own copies. Those headers use no
# v6-only APIs, so mixing them with the 5.11.2 loader headers is sound.
# ---------------------------------------------------------------------------
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdk-compat/Geode/cocos/base_nodes/CCNode.h")
    message(FATAL_ERROR
        "sdk-compat/ is missing or incomplete. Recreate it with:\n"
        "    python tools/sync-2.2091-headers.py --sdk %GEODE_SDK% --v6 <geode v6 checkout>")
endif()

# These directories have to come *first* on the compiler's include search path.
# The SDK adds its own through target_include_directories on GeodeBindings, and
# because that happens inside add_subdirectory() (above) a plain
# target_include_directories() from here would only ever be appended, leaving the
# SDK's 2.2081 copies to win. Prepending to the property instead puts sdk-compat
# ahead of them. No directory-level include_directories() is in play (the SDK
# uses none), so nothing re-orders this later.
#
# Besides the root (for <Geode/cocos/...>) the SDK also puts
# loader/include/Geode/cocos/include and .../cocos/extensions on the path so that
# headers can be included without the Geode/ prefix (<ccTypes.h>); those get a
# 2.2091 counterpart too, or an unprefixed include would still reach 2.2081.
set(PGG_2091_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk-compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk-compat/Geode/cocos/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk-compat/Geode/cocos/extensions"
)

function(pgg_prepend_include_dirs target)
    if (NOT TARGET ${target})
        return()
    endif()
    get_property(_pgg_current TARGET ${target} PROPERTY INCLUDE_DIRECTORIES)
    set_property(TARGET ${target} PROPERTY INCLUDE_DIRECTORIES
        ${PGG_2091_INCLUDES} ${_pgg_current})
endfunction()

pgg_prepend_include_dirs(${PROJECT_NAME})
pgg_prepend_include_dirs(GeodeBindings)

# The 2.2091 cocos2d headers only replace the *game's* ABI description; the Geode
# *loader* API has to stay the released one. A 2.2091-development (v6-style)
# checkout exports different symbols -- e.g. utils::file::readJson taking
# asp::fs::PathView instead of std::filesystem::path -- which the installed
# Geode.dll does not have, so the mod would fail to load with unresolved imports.
if (EXISTS "$ENV{GEODE_SDK}/loader/include/Geode/utils/file.hpp")
    file(READ "$ENV{GEODE_SDK}/loader/include/Geode/utils/file.hpp" PGG_SDK_FILE_HPP)
    if (PGG_SDK_FILE_HPP MATCHES "asp::fs")
        message(FATAL_ERROR
            "This SDK is a Geode 2.2091 development checkout (v6-style), whose loader API\n"
            "does not match the released Geode loader the mod has to run on. Build against\n"
            "the released SDK instead (the one `geode sdk install` provides); the 2.2091\n"
            "cocos2d declarations come from sdk-compat/ regardless.")
    endif()
endif()

setup_geode_mod(${PROJECT_NAME})

# ---------------------------------------------------------------------------
# Import library for the installed 2.2091 libcocos2d.dll.
#
# The SDK links loader/include/link/win64/libcocos2d.lib, an import library built
# from the libcocos2d.dll of the GD version Geode targets (2.2081). Six symbols
# that the 2.2091 declarations in sdk-compat and the 2.2091 bindings reference do
# not exist there, so the mod fails to link with LNK2019/LNK2001:
#
#     cocos2d::CCNode::applyScale(float)                              (new virtual)
#     cocos2d::CCNode::offsetPosition(float, float)                   (new virtual)
#     cocos2d::CCKeyboardActionManager::handleGlobalAction(int, bool)
#     cocos2d::CCTouchDispatcher::registerForcePrio(CCObject*, int, int)  (+1 int)
#     DS_Dictionary::loadRootSubDictFromCompressedFile(char const*, bool)  (+1 bool)
#     DS_Dictionary::saveRootSubDictToCompressedFile(char const*, bool)    (+1 bool)
#
# All six *are* exported by the installed libcocos2d.dll, only the shipped import
# library is out of date, so
# tools/make-2091-cocos-importlib.py regenerates one from the real DLL. Both
# libraries are linked: unchanged symbols resolve from either (they name the same
# DLL and the same mangled names), the changed ones only from ours. The DLL is
# imported by name, so the loader binds it to the libcocos2d.dll next to the game
# exe at load time.
# ---------------------------------------------------------------------------
if (WIN32)
    set(PGG_COCOS_IMPORTLIB "${CMAKE_CURRENT_SOURCE_DIR}/sdk-compat/libcocos2d-2091.lib")
    if (NOT EXISTS "${PGG_COCOS_IMPORTLIB}")
        message(FATAL_ERROR
            "sdk-compat/libcocos2d-2091.lib is missing. Regenerate it from the installed\n"
            "GD 2.2091 libcocos2d.dll with:\n"
            "    python tools/make-2091-cocos-importlib.py --dll \"<GD install>\\libcocos2d.dll\"")
    endif()
    # Plain (keyword-less) signature: the SDK's GeodeFile.cmake uses the plain
    # form for this target, and CMake refuses to mix the two.
    target_link_libraries(${PROJECT_NAME} "${PGG_COCOS_IMPORTLIB}")
endif()
