cmake_minimum_required(VERSION 3.16.3)
cmake_policy(SET CMP0048 NEW)
project(
  pmp-library
  VERSION 3.0.0
  DESCRIPTION "The Polygon Mesh Processing Library"
  HOMEPAGE_URL "https://www.pmp-library.org")
cmake_policy(SET CMP0072 NEW)

option(PMP_BUILD_EXAMPLES "Build the PMP examples" ON)
option(PMP_BUILD_TESTS "Build the PMP test programs" OFF)
option(PMP_BUILD_DOCS "Build the PMP documentation" OFF)
option(PMP_BUILD_VIEWERS "Build the PMP viewers" ON)
option(PMP_BUILD_POLYGONAL "Build Polygonal" OFF)
option(PMP_INSTALL "Install the PMP library and headers" ON)
option(PMP_STRICT_COMPILATION "Treat compiler warnings as errors" ON)
option(PMP_BUILD_REGRESSIONS "Build the PMP regression test programs" OFF)
option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

# set output paths
set(PROJECT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR})
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

if(PMP_BUILD_VIEWERS)
  set(GLFW_SOURCE_DIR_INTERNAL "external/glfw-3.4")
  set(WGPU_SOURCE_DIR "external/wgpu-native")
  set(GLFW3WEBGPU_SOURCE_DIR "external/glfw3webgpu")
  set(IMGUI_SOURCE_DIR "external/imgui-1.92.6")
  set(STB_IMAGE_SOURCE_DIR "external/stb_image-2.28")
  set(STB_IMAGE_WRITE_SOURCE_DIR "external/stb_image_write-1.16")

  add_subdirectory(${STB_IMAGE_WRITE_SOURCE_DIR})
  add_subdirectory(${STB_IMAGE_SOURCE_DIR})

  # Building only the GLFW lib
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "")
  set(GLFW_BUILD_EXAMPLES
      OFF
      CACHE BOOL "")
  set(GLFW_BUILD_TESTS
      OFF
      CACHE BOOL "")
  set(GLFW_BUILD_DOCS
      OFF
      CACHE BOOL "")
  set(GLFW_INSTALL
      OFF
      CACHE BOOL "")

  # disable GLFW wayland support if scanner is not found. See #207
  find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
  if(NOT WAYLAND_SCANNER_EXECUTABLE)
    set(GLFW_BUILD_WAYLAND False)
  endif()

  # setup use of GLFW and wgpu-native (WebGPU implementation on top of
  # Vulkan / Metal / DX12)
  if(NOT EMSCRIPTEN)
    add_subdirectory(${GLFW_SOURCE_DIR_INTERNAL})

    # select the prebuilt wgpu-native binary for this platform
    if(WIN32)
      set(WGPU_PLATFORM "windows-x86_64")
    elseif(APPLE)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(WGPU_PLATFORM "macos-aarch64")
      else()
        set(WGPU_PLATFORM "macos-x86_64")
      endif()
    else()
      set(WGPU_PLATFORM "linux-x86_64")
    endif()
    set(WGPU_LIB_DIR "${PROJECT_SOURCE_DIR}/${WGPU_SOURCE_DIR}/lib/${WGPU_PLATFORM}")
    message(STATUS "Using wgpu-native binaries from ${WGPU_LIB_DIR}")

    add_library(wgpu_native SHARED IMPORTED GLOBAL)
    set_target_properties(
      wgpu_native
      PROPERTIES
        IMPORTED_LOCATION
        "${WGPU_LIB_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}wgpu_native${CMAKE_SHARED_LIBRARY_SUFFIX}"
        INTERFACE_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/${WGPU_SOURCE_DIR}/include")
    if(WIN32)
      set_target_properties(wgpu_native PROPERTIES
                            IMPORTED_IMPLIB "${WGPU_LIB_DIR}/wgpu_native.dll.lib")
    endif()

    # copy the wgpu-native DLL next to an executable (Windows only)
    function(pmp_copy_wgpu_dll target)
      if(WIN32)
        add_custom_command(
          TARGET ${target}
          POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:wgpu_native>
                  $<TARGET_FILE_DIR:${target}>)
      endif()
    endfunction()

  endif()

  # glfw3webgpu: create a WebGPU surface from a GLFW window (all platforms
  # including the HTML canvas under Emscripten)
  if(APPLE AND NOT EMSCRIPTEN)
    # the Cocoa/Metal path is Objective-C: compile through an .m wrapper
    enable_language(OBJC)
    file(WRITE "${CMAKE_BINARY_DIR}/glfw3webgpu_objc.m"
         "#include \"${PROJECT_SOURCE_DIR}/${GLFW3WEBGPU_SOURCE_DIR}/glfw3webgpu.c\"\n")
    set(GLFW3WEBGPU_SOURCES "${CMAKE_BINARY_DIR}/glfw3webgpu_objc.m")
  else()
    set(GLFW3WEBGPU_SOURCES ${GLFW3WEBGPU_SOURCE_DIR}/glfw3webgpu.c)
  endif()
  add_library(glfw3webgpu STATIC ${GLFW3WEBGPU_SOURCES}
              ${GLFW3WEBGPU_SOURCE_DIR}/glfw3webgpu.h)
  target_include_directories(glfw3webgpu
                             PUBLIC ${PROJECT_SOURCE_DIR}/${GLFW3WEBGPU_SOURCE_DIR})
  if(EMSCRIPTEN)
    target_compile_definitions(glfw3webgpu PUBLIC WEBGPU_BACKEND_EMDAWNWEBGPU)
    target_compile_options(glfw3webgpu PUBLIC --use-port=emdawnwebgpu)
  else()
    target_link_libraries(glfw3webgpu PUBLIC glfw wgpu_native)
    # glfw3webgpu selects the native surface type from GLFW's private
    # platform macros, so replicate them here
    if(GLFW_BUILD_X11)
      target_compile_definitions(glfw3webgpu PRIVATE _GLFW_X11)
    endif()
    if(GLFW_BUILD_WAYLAND)
      target_compile_definitions(glfw3webgpu PRIVATE _GLFW_WAYLAND)
    endif()
    if(WIN32)
      target_compile_definitions(glfw3webgpu PRIVATE _GLFW_WIN32)
    endif()
    if(APPLE)
      target_compile_definitions(glfw3webgpu PRIVATE _GLFW_COCOA)
    endif()
    if(APPLE)
      target_link_libraries(glfw3webgpu PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework Metal")
    endif()
  endif()

  # setup IMGUI
  add_subdirectory(${IMGUI_SOURCE_DIR})
endif(PMP_BUILD_VIEWERS)

if(NOT DEFINED EIGEN3_INCLUDE_DIR)
  set(EIGEN3_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/external/eigen-3.4.0")
endif()
find_package(Eigen3 3.4.0 REQUIRED)

include_directories(${PROJECT_SOURCE_DIR}/src/)

# setup for code coverage testing
if(CMAKE_BUILD_TYPE STREQUAL "Debug"
   AND ENABLE_COVERAGE
   AND NOT WIN32)
  enable_testing()

  # set compiler flags
  set(CMAKE_CXX_FLAGS "-g -O0 --coverage")

  # find required tools
  find_program(LCOV lcov REQUIRED)
  find_program(GENHTML genhtml REQUIRED)

  # add coverage target
  add_custom_target(
    coverage
    # gather data
    COMMAND
      ${LCOV} --directory . --capture --exclude '*/external/*' --exclude
      '*/tests/*' --exclude '/usr/*' --exclude '/Applications/*' --exclude
      'v1/*' --output-file coverage.info --ignore-errors unused
    # generate report
    COMMAND ${GENHTML} --demangle-cpp -o coverage coverage.info
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set default compiler flags for both gcc and clang
set(COMMON_CXX_FLAGS "-pedantic -Wall -Wextra -Wshadow")
if(PMP_STRICT_COMPILATION)
  set(COMMON_CXX_FLAGS "${COMMON_CXX_FLAGS} -Werror")
endif()

if((UNIX OR APPLE) AND CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} ${COMMON_CXX_FLAGS} -Wno-deprecated-copy")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER "7.0.0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-int-in-bool-context")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT EMSCRIPTEN)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} ${COMMON_CXX_FLAGS} --system-header-prefix=Eigen")
endif()

if(WIN32)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -D_USE_MATH_DEFINES -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS"
  )
endif()

if(MSVC)
  add_compile_options(/wd4267) # conversion from size_t
  add_compile_options(/wd4244) # conversion from double to float
  add_compile_options(/wd4305) # truncation from double to float
endif()

if(EMSCRIPTEN)
  # add_compile_options(--no-heap-copy)
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
  add_link_options(
    "SHELL:-sWASM=1 -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -sSTACK_SIZE=5MB -sEXPORTED_RUNTIME_METHODS=[ccall,FS,UTF8ToString]"
  )
  # WebGPU through the emdawnwebgpu port; ASYNCIFY lets us block on
  # WebGPU futures (adapter/device requests, buffer mapping)
  add_link_options("SHELL:--use-port=emdawnwebgpu -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=1048576")
  option(PMP_EMSCRIPTEN_ASSERTIONS "Enable Emscripten runtime assertions and heap checks" OFF)
  if(PMP_EMSCRIPTEN_ASSERTIONS)
    add_link_options("SHELL:-sASSERTIONS=2 -sSAFE_HEAP=1 -sSTACK_OVERFLOW_CHECK=2")
  endif()
  set(CMAKE_EXECUTABLE_SUFFIX ".html")
endif()

# make a release build by default
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type specified. Using 'Release' as default.")
  set(CMAKE_BUILD_TYPE
      "Release"
      CACHE
        STRING
        "The build type. Possible values: Debug Release RelWithDebInfo MinSizeRel."
        FORCE)
endif()

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug" AND NOT WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb")
endif()

# TU Dortmund color scheme
if(TUDO)
  add_definitions(-DTUDO)
endif()

# choose size of the scalar type
if(PMP_SCALAR_TYPE EQUAL 64)
  message(STATUS "Using 64-bit scalar type")
  add_definitions(-DPMP_SCALAR_TYPE_64)
endif()

# choose size of the index type
if(PMP_INDEX_TYPE EQUAL 64)
  message(STATUS "Using 64-bit index type")
  add_definitions(-DPMP_INDEX_TYPE_64)
endif()

# setup clang-tidy if program found
option(WITH_CLANG_TIDY "Run clang-tidy checks" OFF)
include(clang-tidy)

# which directories to process

add_subdirectory(src/pmp)

if(PMP_BUILD_POLYGONAL)
  add_subdirectory(src/polygonal)
endif()

if(EMSCRIPTEN)
  if(PMP_BUILD_EXAMPLES)
    add_subdirectory(examples)
  endif()
else()
  if(PMP_BUILD_DOCS)
    add_subdirectory(docs)
  endif()
  if(PMP_BUILD_EXAMPLES)
    add_subdirectory(examples)
  endif()
  if(PMP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()
endif()

if(NOT EMSCRIPTEN AND PMP_INSTALL)

  # Generate package configuration files
  include(CMakePackageConfigHelpers)

  set(PMP_CONFIG_PATH "lib${LIB_SUFFIX}/cmake/pmp")

  configure_package_config_file(
    cmake/pmpConfig.cmake.in pmpConfig.cmake
    INSTALL_DESTINATION "${PMP_CONFIG_PATH}"
    NO_CHECK_REQUIRED_COMPONENTS_MACRO)

  install(
    EXPORT pmpTargets
    FILE pmpTargets.cmake
    EXPORT_LINK_INTERFACE_LIBRARIES
    DESTINATION "${PMP_CONFIG_PATH}")

  export(TARGETS pmp FILE pmpTargets.cmake)

  write_basic_package_version_file(
    pmpConfigVersion.cmake
    VERSION ${PMP_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(FILES "${PROJECT_BINARY_DIR}/pmpConfig.cmake"
                "${PROJECT_BINARY_DIR}/pmpConfigVersion.cmake"
          DESTINATION "${PMP_CONFIG_PATH}")

endif()

# add uninstall target if none is defined
if(NOT TARGET uninstall AND PMP_INSTALL)

  configure_file("${CMAKE_MODULE_PATH}/cmake_uninstall.cmake.in"
                 cmake_uninstall.cmake IMMEDIATE @ONLY)

  add_custom_target(uninstall "${CMAKE_COMMAND}" -P
                              "${PROJECT_BINARY_DIR}/cmake_uninstall.cmake")

endif()

# add clang-format target if program is found
include(clang-format)
