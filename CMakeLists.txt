cmake_minimum_required(VERSION 3.23)


# >>> Project
set(NAME "Heisenberg_F4VR")
set(FRIENDLY_NAME "Heisenberg F4VR")
set(VERSION 0.7.0)

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


# >>> Version & resource files
configure_file(${ROOT_DIR}/cmake/version.rc.in  ${BUILD_DIR}/version.rc  @ONLY)
configure_file(${ROOT_DIR}/cmake/Version.h.in   ${BUILD_DIR}/include/Version.h @ONLY)
source_group(TREE ${BUILD_DIR} FILES ${BUILD_DIR}/include/Version.h ${BUILD_DIR}/version.rc)


# >>> Sources
file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${SOURCE_DIR}/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${SOURCE_DIR}/*.cpp")
list(FILTER sources EXCLUDE REGEX "HIGGS\\.cpp$")
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
)


# >>> Dependencies
target_link_libraries(${PROJECT_NAME}
  PRIVATE
    F4VRCommon::framework
)


# >>> PCH
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)


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
    "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
)

string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")


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
