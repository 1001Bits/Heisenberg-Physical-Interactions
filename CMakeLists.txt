cmake_minimum_required(VERSION 3.23)


# >>> Project
set(NAME "Heisenberg_F4VR")
set(FRIENDLY_NAME "Heisenberg F4VR")
set(VERSION 0.8.1)

message(">>> Start build for '${NAME}' v${VERSION}")


# >>> Folders
set(ROOT_DIR  "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")


# >>> Include guards
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR ">> In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.")
endif()


# >>> Set variable from environment by same name
macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()


# >>> Setup vcpkg
message(">>> Setup vcpkg")
set_from_environment(VCPKG_ROOT)
if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
else()
  message(FATAL_ERROR ">> Variable VCPKG_ROOT is not set!")
endif()


# >>> Locate F4VRCommonFramework (use the one from frik)
set(F4VR_COMMON_FRAMEWORK_PATH "${ROOT_DIR}/../frik/external/F4VRCommonFramework" CACHE STRING "")
message(">>> Using F4VRCommonFramework: '${F4VR_COMMON_FRAMEWORK_PATH}'")
if(NOT EXISTS "${F4VR_COMMON_FRAMEWORK_PATH}/src/ModBase.h")
  message(FATAL_ERROR ">> F4VRCommonFramework path not found in '${F4VR_COMMON_FRAMEWORK_PATH}'!")
endif()


# >>> Create Project
project(${NAME} VERSION ${VERSION} LANGUAGES CXX)
message(">>> Building project '${PROJECT_NAME}' v:${PROJECT_VERSION}")


# >>> Build flags
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")
set(Boost_USE_STATIC_LIBS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)


# >>> Bring in the framework (it brings CommonLibF4 transitively)
add_subdirectory(${F4VR_COMMON_FRAMEWORK_PATH} "F4VRCommonFramework" EXCLUDE_FROM_ALL)


# >>> Vendored ROCK engine (Route A: pristine source compiled as a static lib).
# Source lives in external/ROCK/src (pinned — see external/ROCK/VENDORED_FROM.txt).
# ROCKMain.cpp's F4SEPlugin_Query/Load exports have been stripped and replaced by
# rock::HostLoad() (embedded-host entry) — Heisenberg owns the F4SE entry point and calls
# HostLoad after its own init, gated on bUseRockEngineArchitecture. So ROCKMain.cpp is now
# COMPILED INTO rock_engine (no longer filtered out).
set(ROCK_DIR "${ROOT_DIR}/external/ROCK")
if(EXISTS "${ROCK_DIR}/src/PCH.h")
  file(GLOB_RECURSE rock_sources CONFIGURE_DEPENDS "${ROCK_DIR}/src/*.cpp")
  file(GLOB_RECURSE rock_headers CONFIGURE_DEPENDS "${ROCK_DIR}/src/*.h")
  file(GLOB_RECURSE rock_inls    CONFIGURE_DEPENDS "${ROCK_DIR}/src/*.inl")

  add_library(rock_engine STATIC EXCLUDE_FROM_ALL ${rock_sources} ${rock_headers} ${rock_inls})
  target_compile_features(rock_engine PRIVATE cxx_std_23)
  target_compile_definitions(rock_engine PRIVATE _UNICODE)
  target_include_directories(rock_engine PRIVATE "${ROCK_DIR}/generated" "${ROCK_DIR}/src")
  find_package(cpptrace CONFIG REQUIRED)
  target_link_libraries(rock_engine PRIVATE F4VRCommon::framework cpptrace::cpptrace d3d11.lib dxgi.lib d3dcompiler.lib)
  target_precompile_headers(rock_engine PRIVATE "${ROCK_DIR}/src/PCH.h")
  # Vendored code: relax warnings (no /WX), mirror ROCK's essential flags.
  target_compile_options(rock_engine PRIVATE /utf-8 /permissive- /Zc:preprocessor /EHsc /bigobj /W1)
  # Disable LTO/LTCG for rock_engine so its codegen matches the known-good STANDALONE ROCK build
  # (which builds this identical source with IPO OFF). The directory-scope
  # CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON (line 61) is otherwise inherited by this target.
  # NOTE: LTCG was investigated as, but PROVEN NOT to be, the cause of the g_rockConfig config-load
  # CTD (a minimal MSVC repro showed heap-std::string NSDMIs compile to a valid non-null buffer
  # under /GL+/LTCG too). The real cause of that "unkillable" crash was a STALE BUILD (a 3-week-old
  # main-target PCH surviving incremental builds) plus a STALE RESIDENT DLL image (F4SE loads DLLs
  # only at launch, so a save-reload kept the old pre-fix binary running). The source fixes that
  # actually matter live in RockConfig.{h,cpp}: `extern` single-definition of g_rockConfig + empty
  # NSDMIs on the 3 heap strings (set at runtime in resetToDefaults). This IPO-OFF line is kept as
  # harmless parity-with-standalone insurance, not as the fix. See memory
  # project_rock_embed_build_milestone_jun13.md ("FOURTH-CRASH REAL ROOT CAUSE").
  set_target_properties(rock_engine PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
  message(">>> rock_engine target defined (vendored ROCK source, compile-proof; LTO OFF = standalone parity)")
endif()


# >>> Version & resource files
configure_file(${ROOT_DIR}/cmake/version.rc.in  ${BUILD_DIR}/version.rc  @ONLY)
configure_file(${ROOT_DIR}/cmake/Version.h.in   ${BUILD_DIR}/include/Version.h @ONLY)
source_group(TREE ${BUILD_DIR} FILES ${BUILD_DIR}/include/Version.h ${BUILD_DIR}/version.rc)


# >>> Sources
file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${SOURCE_DIR}/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${SOURCE_DIR}/*.cpp")
list(FILTER sources EXCLUDE REGEX "HIGGS\\.cpp$")

# >>> ODR isolation vs the embedded rock_engine (second architecture).
# The in-house rock_integration ports formerly reused the engine's bare `rock::` namespace,
# which silently merged their weak/COMDAT symbols with the vendored engine's DIVERGENT copies
# (~54 sub-namespaces + direct types collided — the generalization of the old g_rockConfig ODR
# crash; integration audit H3). All in-house ports are now namespace-isolated under
# `heisenberg::rock_core::` (references to genuine engine symbols — rock::api, rock::HostLoad,
# rock::g_rockConfig/RockConfig, rock::push_assist, rock::grab_finger_pose_math,
# rock::selection_query_policy — deliberately stay `rock::`). Distinct namespaces = no COMDAT
# merge, so the three strong-symbol ports below no longer collide and are compiled normally;
# their consumers (RockGrabCoreManager etc.) now resolve against the in-house copy they were
# written against, not an arbitrary COMDAT winner.
source_group(TREE ${ROOT_DIR} FILES ${headers} ${sources})


# >>> Target (DLL)
add_library(${PROJECT_NAME} SHARED
  ${headers}
  ${sources}
  ${BUILD_DIR}/include/Version.h
  ${BUILD_DIR}/version.rc
)


# >>> C++ standard & defs
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE _UNICODE HEISENBERG_F4VR_EXPORTS)


# >>> Includes
target_include_directories(${PROJECT_NAME}
  PRIVATE
    ${BUILD_DIR}/include
    ${SOURCE_DIR}
    ${ROCK_DIR}/src
)


# >>> Dependencies
target_link_libraries(${PROJECT_NAME}
  PRIVATE
    F4VRCommon::framework
    rock_engine   # embedded ROCK grab/physics engine (second architecture; gated at runtime)
)


# >>> PCH
target_precompile_headers(${PROJECT_NAME} PRIVATE backups/0.7.2/PCH.h)


# >>> Compile / Link options
target_compile_options(${PROJECT_NAME}
  PRIVATE
    /sdl
    /utf-8
    /Zi
    /permissive-
    /Zc:alignedNew
    /Zc:auto
    /Zc:char8_t
    /Zc:__cplusplus
    /Zc:externC
    /Zc:externConstexpr
    /Zc:forScope
    /Zc:hiddenFriend
    /Zc:implicitNoexcept
    /Zc:lambda
    /Zc:noexceptTypes
    /Zc:preprocessor
    /Zc:referenceBinding
    /Zc:rvalueCast
    /Zc:sizedDealloc
    /Zc:strictStrings
    /Zc:ternary
    # threadSafeInit- : use a simple init-flag for function-local statics instead of the
    # locking guard. The locking guard (_Init_thread_wait_v2) DEADLOCKS when a static's
    # initialization is unwound by an SEH __except (e.g. HookEndUpdate's "tape deck skipped"
    # guard interrupting a lazy static init in the holotape path) — the guard is left locked
    # and the NEXT access waits forever (the second-holotape-insert freeze). With '-', an
    # interrupted init simply retries on next access instead of hanging.
    /Zc:threadSafeInit-
    /Zc:tlsGuards
    /Zc:trigraphs
    /Zc:wchar_t
    /external:anglebrackets
    /external:W0
    /W4
    "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
)

target_link_options(${PROJECT_NAME}
  PRIVATE
    /WX
    "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
    "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)

string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")


# >>> Focused host-side tests
option(BUILD_HEISENBERG_TESTS "Build small standalone Heisenberg policy tests" OFF)
if(BUILD_HEISENBERG_TESTS)
  enable_testing()
  add_executable(HeisenbergFrikIniReconcilerTests
    tests/FrikIniCompatibilityTests.cpp
    src/FrikIniCompatibility.cpp
    src/FrikIniCompatibility.h
  )
  target_compile_features(HeisenbergFrikIniReconcilerTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergFrikIniReconcilerTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergFrikIniReconcilerTests COMMAND HeisenbergFrikIniReconcilerTests)

  add_executable(HeisenbergTwoHandedWeaponPolicyTests
    tests/TwoHandedWeaponPolicyTests.cpp
    external/ROCK/src/physics-interaction/weapon/TwoHandedWeaponPolicy.h
    external/ROCK/src/physics-interaction/TransformMath.h
  )
  target_compile_features(HeisenbergTwoHandedWeaponPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergTwoHandedWeaponPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergTwoHandedWeaponPolicyTests COMMAND HeisenbergTwoHandedWeaponPolicyTests)

  add_executable(HeisenbergPipboyMeshContactPolicyTests
    tests/PipboyMeshContactPolicyTests.cpp
    src/PipboyMeshContactPolicy.h
  )
  target_compile_features(HeisenbergPipboyMeshContactPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergPipboyMeshContactPolicyTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergPipboyMeshContactPolicyTests COMMAND HeisenbergPipboyMeshContactPolicyTests)

  add_executable(HeisenbergRockProviderApiPolicyTests
    tests/RockProviderApiPolicyTests.cpp
    external/ROCK/src/api/ROCKProviderApi.h
    external/ROCK/src/api/WeaponPartDriveLeasePolicy.h
  )
  target_compile_features(HeisenbergRockProviderApiPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergRockProviderApiPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergRockProviderApiPolicyTests COMMAND HeisenbergRockProviderApiPolicyTests)
endif()


# >>> Post build command to copy plugin
if(DEFINED COPY_PLUGIN_PATH)
  message(">>> Create post build command: copy targets to: ${COPY_PLUGIN_PATH}/F4SE/Plugins/")
  add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMENT "Copying ${PROJECT_NAME}.dll to plugins folder"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COPY_PLUGIN_PATH}/F4SE/Plugins/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${COPY_PLUGIN_PATH}/F4SE/Plugins/
  )
else()
  message(">>> No post build copy (set COPY_PLUGIN_PATH to enable)")
endif()
