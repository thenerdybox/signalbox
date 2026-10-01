# SignalBox - OBS Studio plugin
# Build system layout follows github.com/obsproject/obs-plugintemplate.
# The dependency-fetching machinery under cmake/common and cmake/windows is
# vendored near-verbatim from that template (GPL-2.0-or-later) because it is
# generic OBS build plumbing, not plugin behavior; see THIRD-PARTY-NOTICES.md.

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  # Widgets: dock + settings dialog + prompt surface. That is the whole of
  # our Qt use.
  #
  # Qt6::Network is deliberately NOT linked. Qt's HTTPS stack needs a TLS
  # backend plugin that OBS does not ship, so every QNetworkAccessManager
  # request inside the OBS process dies with "No functional TLS backend was
  # found" before a single byte leaves the machine. Every outbound request
  # this plugin makes - OAuth, Helix, and the GitHub update check - goes
  # through HttpTransport (twitch/HttpTransport.h) on libcurl below instead.
  # Leaving Network unlinked is what keeps that a build error rather than a
  # runtime failure the user discovers mid-stream.
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  # libcurl: the same HTTPS transport OBS itself ships (bin/64bit/libcurl.dll)
  # and uses for its own traffic - see twitch/HttpTransport.h. Linked only
  # against the import lib; the plugin resolves to whichever libcurl.dll is
  # already loaded in the OBS process at runtime, never bundles its own.
  find_package(CURL REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl_shared)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    # --- core: entry point, config, timing constants, detection state machine ---
    src/core/plugin-main.cpp
    src/core/PluginConfig.h
    src/core/PluginConfig.cpp
    src/core/UserOverrideStore.h
    src/core/CategoryResolver.h
    src/core/CategoryResolver.cpp
    src/core/CategorySwitchCoordinator.h
    src/core/CategorySwitchCoordinator.cpp
    src/core/TimingConstants.h
    src/core/DetectionStateMachine.h
    src/core/DetectionStateMachine.cpp
    src/core/DetectionEngine.h
    src/core/DetectionEngine.cpp
    # --- detection: install index, process scanning, per-store providers ---
    src/detection/DetectedGame.h
    src/detection/IGameProvider.h
    src/detection/ProcessScanner.h
    src/detection/ProcessScanner.cpp
    src/detection/InstallIndex.h
    src/detection/InstallIndex.cpp
    src/detection/IndexExclusions.h
    src/detection/TextMatch.h
    src/detection/providers/SteamProvider.h
    src/detection/providers/SteamProvider.cpp
    src/detection/providers/EpicProvider.h
    src/detection/providers/EpicProvider.cpp
    src/detection/providers/GogProvider.h
    src/detection/providers/GogProvider.cpp
    src/detection/providers/UbisoftProvider.h
    src/detection/providers/UbisoftProvider.cpp
    src/detection/providers/GenericUninstallProvider.h
    src/detection/providers/GenericUninstallProvider.cpp
    # --- twitch: device code auth, Helix client, token storage ---
    src/twitch/HttpTransport.h
    src/twitch/HttpTransport.cpp
    src/twitch/TwitchAuth.h
    src/twitch/TwitchAuth.cpp
    src/twitch/TwitchClient.h
    src/twitch/TwitchClient.cpp
    src/twitch/TwitchCategoryLookup.h
    src/twitch/TwitchCategoryLookup.cpp
    src/twitch/TwitchChannelClient.h
    src/twitch/TwitchChannelClient.cpp
    src/twitch/TokenStore.h
    src/twitch/TokenStore.cpp
    # --- ui: dock, non-modal prompt surface, settings dialog ---
    src/ui/CategoryDock.h
    src/ui/CategoryDock.cpp
    src/ui/PromptWidget.h
    src/ui/PromptWidget.cpp
    src/ui/PromptToast.h
    src/ui/PromptToast.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsDialog.cpp
    src/ui/GameOverrideDialog.h
    src/ui/GameOverrideDialog.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# ---------------------------------------------------------------------------
# Standalone test harness - NO OBS, NO obs-frontend-api, NO libobs at all.
# Proves CategoryResolver + DetectionStateMachine work end to end (a
# synthetic detection result -> resolver -> state machine -> decision)
# without a running OBS process. Deliberately links only Qt6::Core (for
# CategoryResolver's alias-table JSON parsing via QJsonDocument - a plain
# value-type parser with no event loop/GUI requirement, the same choice
# HelperDenylist.h and InstallIndex.cpp already make and explicitly
# sanction for "a standalone test program"). See tests/harness/main.cpp.
# ---------------------------------------------------------------------------
if(ENABLE_QT)
  add_executable(signalbox-harness
    tests/harness/main.cpp
    src/core/DetectionStateMachine.h
    src/core/DetectionStateMachine.cpp
    src/core/CategoryResolver.h
    src/core/CategoryResolver.cpp
    src/core/CategorySwitchCoordinator.h
    src/core/CategorySwitchCoordinator.cpp
    src/core/UserOverrideStore.h
    src/core/TimingConstants.h
    src/detection/DetectedGame.h
    src/detection/ProcessScanner.h
    src/detection/IGameProvider.h
    src/detection/InstallIndex.h
    src/detection/InstallIndex.cpp
    src/detection/IndexExclusions.h
    src/detection/TextMatch.h
    src/detection/HelperDenylist.h
  )

  # Lets the harness open the SHIPPED data/ files by absolute path. Those
  # files are not inert documentation - aliases.json now carries policy
  # ("prompt": true) and helpers.json carries the exclusion rules, so a
  # typo in either changes runtime behavior while every C++ test still
  # passes. Pointing the harness at the real files is what closes that.
  target_compile_definitions(signalbox-harness PRIVATE SIGNALBOX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

  # The harness links Qt6::Core (JSON parsing) but is a plain console exe,
  # so nothing puts Qt's bin directory on its search path the way the OBS
  # process does for the plugin. Whether it starts then depends entirely on
  # which shell you launch it from, which is a miserable way to discover
  # that your tests "fail". Copy what it needs next to it instead.
  add_custom_command(
    TARGET signalbox-harness POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Core> $<TARGET_FILE_DIR:signalbox-harness>
    VERBATIM)
  target_include_directories(signalbox-harness PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(signalbox-harness PRIVATE Qt6::Core)
  set_target_properties(signalbox-harness PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON AUTOMOC OFF)
endif()

# The installer needs the same version the plugin reports. Rather than repeat it
# in installer.iss - where it drifted to 1.0.0 while the plugin said 0.1.0 - emit
# it here so buildspec.json stays the only place it is written.
file(
  WRITE "${CMAKE_CURRENT_SOURCE_DIR}/installer-version.iss"
  "; Generated by CMake from buildspec.json - do not edit.\n#define AppVersion \"${CMAKE_PROJECT_VERSION}\"\n"
)

# Editing buildspec.json must re-run CMake, or the version above is stale.
#
# Without this, bumping the version and rebuilding produces an installer
# still named for the OLD version while containing the NEW code - CMake does
# not re-configure just because a file it once read has changed. That is the
# same failure the build id had, in the one place where it is most
# misleading: the version number is what tells you which build you are
# holding, so a stale one actively misinforms. Caught by bumping 0.2.0 to
# 0.2.1 and watching SignalBox-Setup-0.2.0.exe come out of the build.
set_property(
  DIRECTORY APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json")

# ---------------------------------------------------------------------------
# BUILD IDENTITY: which build is this, exactly?
#
# A version number alone cannot answer that. Most builds during development
# are the same version as the one before them, so "v0.2.0" is shown by the
# installed plugin whether it is today's compile or last week's - and the
# only way to tell them apart was a SHA-256 of the DLL, which nothing in the
# UI shows. That ambiguity has already cost real time: a fix gets installed,
# the old behavior persists, and the first suspicion is the fix rather than
# the install.
#
# So every build stamps the commit it came from. A working tree with
# uncommitted changes gets a "+" suffix, because a build made from unsaved
# edits is precisely the one nobody can reconstruct later.
#
# COMPUTED AT BUILD TIME, NOT CONFIGURE TIME. The obvious implementation -
# run git here and pass -DSIGNALBOX_BUILD_ID=... - is wrong, and wrong in the
# worst available way. CMake does not re-configure because someone committed,
# so the value is captured once and then goes stale: the first build after
# this was written produced a DLL stamped with the PREVIOUS commit, marked
# dirty, from a clean tree one commit ahead. A missing build id is a small
# gap; one that reports the wrong commit confidently is worse than none,
# because it gets believed. Hence the script and the custom target - see
# cmake/build-id.cmake.
#
# Deliberately tolerant: no git, no repo, or git failing for any reason
# yields "unknown" and the build proceeds. A version stamp is diagnostic
# information, never a build requirement.
set(SIGNALBOX_BUILD_ID_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/signalbox-build-id.h")

# Once at configure time, so a freshly configured tree has a header to compile
# against before the custom target has ever run.
execute_process(
  COMMAND ${CMAKE_COMMAND} "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT_FILE=${SIGNALBOX_BUILD_ID_HEADER}" -P
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-id.cmake")

# And again before every build of the plugin. The script rewrites the header
# only when the id actually changes, so this does not force a recompile on
# every no-op build.
add_custom_target(
  signalbox-build-id ALL
  COMMAND ${CMAKE_COMMAND} "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT_FILE=${SIGNALBOX_BUILD_ID_HEADER}" -P
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-id.cmake"
  COMMENT "Stamping SignalBox build id"
  VERBATIM)
add_dependencies(${CMAKE_PROJECT_NAME} signalbox-build-id)

# Not added to plugin-support.c.in: that file is vendored from the OBS plugin
# template (see this file's header) and is not ours to extend.
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
