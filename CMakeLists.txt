cmake_minimum_required(VERSION 4.0)


# >>> Project
set(NAME "Heisenberg_F4VR")
set(FRIENDLY_NAME "Heisenberg F4VR")
set(VERSION 0.8.14)

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


# >>> Locate/pin F4VRCommonFramework.
# ROCK's known-good framework checkout remains the fast developer path. A clean
# checkout fetches that exact public revision and applies our small tracked
# compatibility patch, so the embedded ROCK snapshot sees the CommonLib API it
# was built against without making that separate mod a runtime dependency.
set(F4VR_COMMON_FRAMEWORK_PATH "${ROOT_DIR}/../../ROCK/fw-latest" CACHE STRING "")
set(HEISENBERG_FRAMEWORK_BASE_REVISION
  "5f67adf35c7d8f6d60ee5b353524d32047ed235f"
  CACHE STRING "Pinned public F4VRCommonFramework base revision")
set(HEISENBERG_FRAMEWORK_LOCAL_REVISION
  "5f67adf35c7d8f6d60ee5b353524d32047ed235f"
  CACHE STRING "Accepted sibling revision containing the tracked compatibility changes")
option(HEISENBERG_ALLOW_UNPINNED_FRAMEWORK
  "Allow a sibling F4VRCommonFramework checkout at an unverified revision"
  OFF)
set(_heisenberg_fetch_framework OFF)
if(NOT EXISTS "${F4VR_COMMON_FRAMEWORK_PATH}/src/ModBase.h")
  set(_heisenberg_fetch_framework ON)
  include(FetchContent)
  find_package(Git REQUIRED)
  FetchContent_Declare(
    heisenberg_f4vr_common_framework
    GIT_REPOSITORY https://github.com/ArthurHub/F4VRCommonFramework.git
    GIT_TAG "${HEISENBERG_FRAMEWORK_BASE_REVISION}"
    GIT_SHALLOW FALSE
    GIT_SUBMODULES_RECURSE TRUE
    PATCH_COMMAND
      "${GIT_EXECUTABLE}" apply --whitespace=nowarn
      "${ROOT_DIR}/cmake/patches/F4VRCommonFramework-heisenberg.patch"
  )
  message(">>> F4VRCommonFramework sibling not found; using pinned FetchContent fallback")
else()
  find_package(Git QUIET)
  if(Git_FOUND AND EXISTS "${F4VR_COMMON_FRAMEWORK_PATH}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${F4VR_COMMON_FRAMEWORK_PATH}" rev-parse HEAD
      OUTPUT_VARIABLE _heisenberg_framework_revision
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(NOT "${_heisenberg_framework_revision}" STREQUAL "${HEISENBERG_FRAMEWORK_LOCAL_REVISION}" AND
       NOT "${_heisenberg_framework_revision}" STREQUAL "${HEISENBERG_FRAMEWORK_BASE_REVISION}")
      if(HEISENBERG_ALLOW_UNPINNED_FRAMEWORK)
        message(WARNING
          ">>> Unpinned F4VRCommonFramework revision '${_heisenberg_framework_revision}' allowed by override")
      else()
        message(FATAL_ERROR
          ">> F4VRCommonFramework revision '${_heisenberg_framework_revision}' is not pinned. "
          "Use revision ${HEISENBERG_FRAMEWORK_LOCAL_REVISION}, remove the sibling to use the "
          "reproducible fetch fallback, or explicitly set HEISENBERG_ALLOW_UNPINNED_FRAMEWORK=ON.")
      endif()
    endif()
  endif()
  message(">>> Using F4VRCommonFramework: '${F4VR_COMMON_FRAMEWORK_PATH}'")
endif()


# >>> Create Project
project(${NAME} VERSION ${VERSION} LANGUAGES CXX)
message(">>> Building project '${PROJECT_NAME}' v:${PROJECT_VERSION}")


# >>> Build flags
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")
set(Boost_USE_STATIC_LIBS ON)
option(HEISENBERG_ENABLE_IPO
  "Enable whole-program optimization (/GL + /LTCG). Disabled by default because it greatly increases peak build memory."
  OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION "${HEISENBERG_ENABLE_IPO}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(HEISENBERG_MSVC_COMPILE_PROCESSES "2" CACHE STRING
  "Bounded concurrent cl.exe compilations per target (/MP); 2 keeps useful throughput without the /MP4 memory spike")
set(HEISENBERG_MSVC_CODEGEN_PROCESSES "1" CACHE STRING
  "Maximum concurrent MSVC LTCG code-generation threads (/CGTHREADS)")


# >>> Bring in the framework (it brings CommonLibF4 transitively)
if(_heisenberg_fetch_framework)
  FetchContent_MakeAvailable(heisenberg_f4vr_common_framework)
  set(F4VR_COMMON_FRAMEWORK_PATH
    "${heisenberg_f4vr_common_framework_SOURCE_DIR}"
    CACHE STRING "" FORCE)
else()
  add_subdirectory(${F4VR_COMMON_FRAMEWORK_PATH} "F4VRCommonFramework" EXCLUDE_FROM_ALL)
endif()

# The dependency enables IPO and /Zi unconditionally. Override both after its
# targets exist: release DLLs do not ship symbols, and /GL+/LTCG was the major
# peak-memory/link-time source in this combined build.
foreach(_heisenberg_dependency_target IN ITEMS F4VRCommonFramework CommonLibF4)
  if(TARGET ${_heisenberg_dependency_target})
    set_target_properties(${_heisenberg_dependency_target} PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION OFF
      INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    get_target_property(
      _heisenberg_dependency_compile_options
      ${_heisenberg_dependency_target}
      COMPILE_OPTIONS)
    if(_heisenberg_dependency_compile_options)
      list(FILTER _heisenberg_dependency_compile_options EXCLUDE REGEX "^/Zi$")
      set_property(
        TARGET ${_heisenberg_dependency_target}
        PROPERTY COMPILE_OPTIONS "${_heisenberg_dependency_compile_options}")
    endif()
  endif()
endforeach()


# >>> Vendored ROCK engine (Route A: pristine source compiled as a static lib).
# Source lives in external/ROCK/src (pinned — see external/ROCK/VENDORED_FROM.txt).
# ROCKMain.cpp's F4SEPlugin_Query/Load exports have been stripped and replaced by
# rock::HostLoad() (embedded-host entry) — Heisenberg owns the F4SE entry point and calls
# HostLoad after its own init, gated on bUseRockEngineArchitecture. So ROCKMain.cpp is now
# COMPILED INTO rock_engine (no longer filtered out).
set(ROCK_DIR "${ROOT_DIR}/external/ROCK")
set(ROCK_COMMONLIB_COMPAT_DIR "${ROCK_DIR}/src/compat")
if(NOT EXISTS "${ROCK_DIR}/src/PCH.h")
  message(FATAL_ERROR
    ">> The vendored ROCK tree is incomplete: '${ROCK_DIR}/src/PCH.h' is missing. "
    "All files under external/ROCK/src must be present in the checkout.")
elseif(NOT EXISTS "${ROCK_COMMONLIB_COMPAT_DIR}/RE/Havok/hknpWorld.h"
    OR NOT EXISTS "${ROCK_COMMONLIB_COMPAT_DIR}/RE/Bethesda/BSSkin.h")
  message(FATAL_ERROR
    ">> The vendored ROCK CommonLibF4VR compatibility headers are incomplete in "
    "'${ROCK_COMMONLIB_COMPAT_DIR}'. A clean checkout must not depend on local "
    "untracked headers from a sibling FRIK checkout.")
else()
  file(GLOB_RECURSE rock_sources CONFIGURE_DEPENDS "${ROCK_DIR}/src/*.cpp")
  file(GLOB_RECURSE rock_headers CONFIGURE_DEPENDS "${ROCK_DIR}/src/*.h")
  file(GLOB_RECURSE rock_inls    CONFIGURE_DEPENDS "${ROCK_DIR}/src/*.inl")

  add_library(rock_engine STATIC EXCLUDE_FROM_ALL ${rock_sources} ${rock_headers} ${rock_inls})
  target_compile_features(rock_engine PRIVATE cxx_std_23)
  target_compile_definitions(rock_engine PRIVATE _UNICODE)
  target_include_directories(rock_engine BEFORE PRIVATE
    "${ROCK_COMMONLIB_COMPAT_DIR}"
    "${ROCK_DIR}/generated"
    "${ROCK_DIR}/src")
  find_package(cpptrace CONFIG REQUIRED)
  target_link_libraries(rock_engine PRIVATE F4VRCommon::framework cpptrace::cpptrace d3d11.lib dxgi.lib d3dcompiler.lib)
  target_precompile_headers(rock_engine PRIVATE "${ROCK_DIR}/src/PCH.h")
  # Vendored code: relax warnings (no /WX), mirror ROCK's essential flags.
  target_compile_options(rock_engine PRIVATE
    /utf-8 /permissive- /Zc:preprocessor /EHsc /bigobj /W1
    "$<$<CXX_COMPILER_ID:MSVC>:/MP${HEISENBERG_MSVC_COMPILE_PROCESSES}>"
  )
  # Disable LTO/LTCG for rock_engine so its codegen matches the known-good STANDALONE ROCK build
  # (which builds this identical source with IPO OFF). Keep this explicit even when
  # HEISENBERG_ENABLE_IPO is opted into for the host DLL.
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
  if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(rock_engine PROPERTIES
      VS_GLOBAL_CL_MPCount "${HEISENBERG_MSVC_COMPILE_PROCESSES}")
  endif()
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
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
  set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_GLOBAL_CL_MPCount "${HEISENBERG_MSVC_COMPILE_PROCESSES}")
endif()


# >>> C++ standard & defs
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PROJECT_NAME} PRIVATE _UNICODE HEISENBERG_F4VR_EXPORTS)


# >>> Includes
target_include_directories(${PROJECT_NAME}
  BEFORE PRIVATE
    ${ROCK_COMMONLIB_COMPAT_DIR}
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
if(MSVC AND HEISENBERG_ENABLE_IPO)
  target_link_options(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release>:/LTCG>"
    "/CGTHREADS:${HEISENBERG_MSVC_CODEGEN_PROCESSES}")
endif()


# >>> PCH
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)


# >>> Compile / Link options
target_compile_options(${PROJECT_NAME}
  PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/MP${HEISENBERG_MSVC_COMPILE_PROCESSES}>"
    /sdl
    /utf-8
    "$<$<CONFIG:DEBUG>:/Zi>"
    "$<$<CONFIG:RELWITHDEBINFO>:/Zi>"
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
    /Zc:threadSafeInit
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
    "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:NONE>"
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

  add_executable(HeisenbergFrikCompatibilityPolicyTests
    tests/FrikCompatibilityPolicyTests.cpp
    external/ROCK/src/physics-interaction/visual/FrikCompatibilityPolicy.h
  )
  target_compile_features(HeisenbergFrikCompatibilityPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergFrikCompatibilityPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergFrikCompatibilityPolicyTests COMMAND HeisenbergFrikCompatibilityPolicyTests)

  add_executable(HeisenbergRockConfigFileWatchPolicyTests
    tests/RockConfigFileWatchPolicyTests.cpp
    external/ROCK/src/RockConfigFileWatchPolicy.h
  )
  target_compile_features(HeisenbergRockConfigFileWatchPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergRockConfigFileWatchPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergRockConfigFileWatchPolicyTests COMMAND HeisenbergRockConfigFileWatchPolicyTests)

  add_executable(HeisenbergFrikFingerPoseBridgeTests
    tests/FrikFingerPoseBridgeTests.cpp
    external/ROCK/src/api/FRIKApi.h
    external/ROCK/src/physics-interaction/visual/FrikVisualAuthorityBridge.h
  )
  target_compile_features(HeisenbergFrikFingerPoseBridgeTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergFrikFingerPoseBridgeTests PRIVATE "${ROCK_DIR}/src")
  target_link_libraries(HeisenbergFrikFingerPoseBridgeTests PRIVATE F4VRCommon::framework)
  add_test(NAME HeisenbergFrikFingerPoseBridgeTests COMMAND HeisenbergFrikFingerPoseBridgeTests)

  add_executable(HeisenbergTwoHandedWeaponPolicyTests
    tests/TwoHandedWeaponPolicyTests.cpp
    external/ROCK/src/physics-interaction/weapon/TwoHandedWeaponPolicy.h
    external/ROCK/src/physics-interaction/weapon/EquippedWeaponDropPolicy.h
    external/ROCK/src/physics-interaction/weapon/WeaponInteractionProbeFramePolicy.h
    external/ROCK/src/physics-interaction/TransformMath.h
  )
  target_compile_features(HeisenbergTwoHandedWeaponPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergTwoHandedWeaponPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergTwoHandedWeaponPolicyTests COMMAND HeisenbergTwoHandedWeaponPolicyTests)

  add_executable(HeisenbergWeaponWallSweepPolicyTests
    tests/WeaponWallSweepPolicyTests.cpp
    external/ROCK/src/physics-interaction/weapon/WeaponWallSweepPolicy.h
  )
  target_compile_features(HeisenbergWeaponWallSweepPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergWeaponWallSweepPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergWeaponWallSweepPolicyTests COMMAND HeisenbergWeaponWallSweepPolicyTests)

  add_executable(HeisenbergMenuPhysicsPolicyTests
    tests/MenuPhysicsPolicyTests.cpp
    external/ROCK/src/physics-interaction/core/MenuPhysicsPolicy.h
  )
  target_compile_features(HeisenbergMenuPhysicsPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergMenuPhysicsPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergMenuPhysicsPolicyTests COMMAND HeisenbergMenuPhysicsPolicyTests)

  add_executable(HeisenbergPipboyMeshContactPolicyTests
    tests/PipboyMeshContactPolicyTests.cpp
    src/PipboyMeshContactPolicy.h
  )
  target_compile_features(HeisenbergPipboyMeshContactPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergPipboyMeshContactPolicyTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergPipboyMeshContactPolicyTests COMMAND HeisenbergPipboyMeshContactPolicyTests)

  add_executable(HeisenbergGrabPosePolicyTests
    tests/GrabPosePolicyTests.cpp
    src/GrabPosePolicy.h
  )
  target_compile_features(HeisenbergGrabPosePolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergGrabPosePolicyTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergGrabPosePolicyTests COMMAND HeisenbergGrabPosePolicyTests)

  add_executable(HeisenbergGrabOwnershipPolicyTests
    tests/GrabOwnershipPolicyTests.cpp
    src/GrabOwnershipPolicy.h
    src/StorageZoneWeaponPolicy.h
    external/ROCK/src/physics-interaction/input/HostGrabInputEdgePolicy.h
    external/ROCK/src/physics-interaction/input/InputRemapPolicy.h
  )
  target_compile_features(HeisenbergGrabOwnershipPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergGrabOwnershipPolicyTests PRIVATE "${SOURCE_DIR}" "${ROCK_DIR}/src")
  add_test(NAME HeisenbergGrabOwnershipPolicyTests COMMAND HeisenbergGrabOwnershipPolicyTests)

  add_executable(HeisenbergEmbeddedOffsetsTests
    tests/EmbeddedOffsetsTests.cpp
    src/EmbeddedOffsets.h
  )
  target_compile_features(HeisenbergEmbeddedOffsetsTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergEmbeddedOffsetsTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergEmbeddedOffsetsTests COMMAND HeisenbergEmbeddedOffsetsTests)

  add_executable(HeisenbergHeldCollisionBodySetPolicyTests
    tests/HeldCollisionBodySetPolicyTests.cpp
    src/HeldCollisionBodySetPolicy.h
  )
  target_compile_features(HeisenbergHeldCollisionBodySetPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergHeldCollisionBodySetPolicyTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergHeldCollisionBodySetPolicyTests COMMAND HeisenbergHeldCollisionBodySetPolicyTests)

  add_executable(HeisenbergRockProviderApiPolicyTests
    tests/RockProviderApiPolicyTests.cpp
    external/ROCK/src/api/ROCKProviderApi.h
    external/ROCK/src/api/WeaponPartDriveLeasePolicy.h
    external/ROCK/src/physics-interaction/collision/ContactActivityTracker.h
    external/ROCK/src/physics-interaction/weapon/WeaponPartContactAcquisitionPolicy.h
    external/ROCK/src/physics-interaction/weapon/WeaponSemantics.h
  )
  target_compile_features(HeisenbergRockProviderApiPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergRockProviderApiPolicyTests PRIVATE "${ROCK_DIR}/src" "${ROCK_DIR}/src/api")
  target_link_libraries(HeisenbergRockProviderApiPolicyTests PRIVATE F4VRCommon::framework)
  add_test(NAME HeisenbergRockProviderApiPolicyTests COMMAND HeisenbergRockProviderApiPolicyTests)

  add_executable(HeisenbergWeaponPartMotionConstraintTests
    tests/WeaponPartMotionConstraintPolicyTests.cpp
    external/ROCK/src/physics-interaction/weapon/WeaponPartMotionConstraintPolicy.h
  )
  target_compile_features(HeisenbergWeaponPartMotionConstraintTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergWeaponPartMotionConstraintTests PRIVATE "${ROCK_DIR}/src")
  target_link_libraries(HeisenbergWeaponPartMotionConstraintTests PRIVATE F4VRCommon::framework)
  add_test(NAME HeisenbergWeaponPartMotionConstraintTests COMMAND HeisenbergWeaponPartMotionConstraintTests)

  add_executable(HeisenbergCollisionLayerPolicyTests
    tests/CollisionLayerPolicyTests.cpp
    external/ROCK/src/physics-interaction/collision/CollisionLayerPolicy.h
    external/ROCK/src/physics-interaction/collision/ContactPipelinePolicy.h
    external/ROCK/src/physics-interaction/NativeMeleeSuppressionPolicy.h
  )
  target_compile_features(HeisenbergCollisionLayerPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergCollisionLayerPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergCollisionLayerPolicyTests COMMAND HeisenbergCollisionLayerPolicyTests)

  add_executable(HeisenbergHandCollisionSuppressionTopologyTests
    tests/HandCollisionSuppressionTopologyTests.cpp
    external/ROCK/src/physics-interaction/collision/CollisionSuppressionRegistry.h
    external/ROCK/src/physics-interaction/hand/HandLifecycle.h
  )
  target_compile_features(HeisenbergHandCollisionSuppressionTopologyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergHandCollisionSuppressionTopologyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergHandCollisionSuppressionTopologyTests COMMAND HeisenbergHandCollisionSuppressionTopologyTests)

  add_executable(HeisenbergDynamicHandCollisionTopologyTests
    tests/DynamicHandCollisionTopologyTests.cpp
    external/ROCK/src/physics-interaction/hand/DynamicHandCollisionAuthorityPolicy.h
    external/ROCK/src/physics-interaction/hand/DynamicHandCollisionFeedbackPolicy.h
    external/ROCK/src/physics-interaction/hand/DynamicHandCollisionStabilityPolicy.h
    external/ROCK/src/physics-interaction/hand/DynamicHandCollisionTelemetry.h
    external/ROCK/src/physics-interaction/hand/DynamicHandTwinTargets.h
    external/ROCK/src/physics-interaction/hand/HandColliderTypes.h
    external/ROCK/src/physics-interaction/visual/PreAuthorityHandFramePolicy.h
  )
  target_compile_features(HeisenbergDynamicHandCollisionTopologyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergDynamicHandCollisionTopologyTests PRIVATE "${ROCK_DIR}/src")
  target_link_libraries(HeisenbergDynamicHandCollisionTopologyTests PRIVATE F4VRCommon::framework)
  add_test(NAME HeisenbergDynamicHandCollisionTopologyTests COMMAND HeisenbergDynamicHandCollisionTopologyTests)

  add_executable(HeisenbergGeneratedContactBufferPolicyTests
    tests/GeneratedContactBufferPolicyTests.cpp
    external/ROCK/src/physics-interaction/grab/GeneratedContactBufferPolicy.h
  )
  target_compile_features(HeisenbergGeneratedContactBufferPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergGeneratedContactBufferPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergGeneratedContactBufferPolicyTests COMMAND HeisenbergGeneratedContactBufferPolicyTests)

  add_executable(HeisenbergHeldLocomotionTransportPolicyTests
    tests/HeldLocomotionTransportPolicyTests.cpp
    external/ROCK/src/physics-interaction/grab/HeldLocomotionTransportPolicy.h
  )
  target_compile_features(HeisenbergHeldLocomotionTransportPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergHeldLocomotionTransportPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergHeldLocomotionTransportPolicyTests COMMAND HeisenbergHeldLocomotionTransportPolicyTests)

  add_executable(HeisenbergHeldPlayerSpacePolicyTests
    tests/HeldPlayerSpacePolicyTests.cpp
    external/ROCK/src/physics-interaction/grab/GrabHeldObject.h
    external/ROCK/src/physics-interaction/grab/HeldLocomotionTransportPolicy.h
    external/ROCK/src/physics-interaction/grab/HeldPlayerSpaceRegistry.h
    external/ROCK/src/physics-interaction/grab/HeldPlayerSpaceWarpPolicy.h
  )
  target_compile_features(HeisenbergHeldPlayerSpacePolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergHeldPlayerSpacePolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergHeldPlayerSpacePolicyTests COMMAND HeisenbergHeldPlayerSpacePolicyTests)

  add_executable(HeisenbergDynamicGrabPolicyTests
    tests/DynamicGrabPolicyTests.cpp
    external/ROCK/src/physics-interaction/grab/DynamicGrabPolicy.h
    external/ROCK/src/physics-interaction/grab/GrabHeldObject.h
    external/ROCK/src/physics-interaction/hand/HandVisual.h
  )
  target_compile_features(HeisenbergDynamicGrabPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergDynamicGrabPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergDynamicGrabPolicyTests COMMAND HeisenbergDynamicGrabPolicyTests)

  add_executable(HeisenbergNpcInjectionPolicyTests
    tests/NpcInjectionPolicyTests.cpp
    src/ConsumableUsePolicy.h
    src/NpcInjectionPolicy.h
    src/PostConsumeActivationPolicy.h
  )
  target_compile_features(HeisenbergNpcInjectionPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergNpcInjectionPolicyTests PRIVATE "${SOURCE_DIR}")
  add_test(NAME HeisenbergNpcInjectionPolicyTests COMMAND HeisenbergNpcInjectionPolicyTests)

  add_executable(HeisenbergConsumeCommitPolicyTests
    tests/ConsumeCommitPolicyTests.cpp
    external/ROCK/src/physics-interaction/consume/ConsumeCommitPolicy.h
  )
  target_compile_features(HeisenbergConsumeCommitPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergConsumeCommitPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergConsumeCommitPolicyTests COMMAND HeisenbergConsumeCommitPolicyTests)

  add_executable(HeisenbergViewCasterSelectionPolicyTests
    tests/ViewCasterSelectionPolicyTests.cpp
    external/ROCK/src/physics-interaction/object/ViewCasterSelectionPolicy.h
  )
  target_compile_features(HeisenbergViewCasterSelectionPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergViewCasterSelectionPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergViewCasterSelectionPolicyTests COMMAND HeisenbergViewCasterSelectionPolicyTests)

  add_executable(HeisenbergPullCatchRetryPolicyTests
    tests/PullCatchRetryPolicyTests.cpp
    external/ROCK/src/physics-interaction/grab/PullCatchRetryPolicy.h
  )
  target_compile_features(HeisenbergPullCatchRetryPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergPullCatchRetryPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergPullCatchRetryPolicyTests COMMAND HeisenbergPullCatchRetryPolicyTests)

  add_executable(HeisenbergPullMotionPolicyTests
    tests/PullMotionPolicyTests.cpp
    external/ROCK/src/physics-interaction/grab/PullMotionPolicy.h
  )
  target_compile_features(HeisenbergPullMotionPolicyTests PRIVATE cxx_std_23)
  target_include_directories(HeisenbergPullMotionPolicyTests PRIVATE "${ROCK_DIR}/src")
  add_test(NAME HeisenbergPullMotionPolicyTests COMMAND HeisenbergPullMotionPolicyTests)
endif()

# >>> Install/package manifest
# `cmake --install` now produces a complete mod payload instead of a bare DLL.
install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION "F4SE/Plugins")
install(DIRECTORY "${ROOT_DIR}/Data/"
  DESTINATION "."
  FILES_MATCHING
    PATTERN "*.ini"
    PATTERN "*.json")


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
