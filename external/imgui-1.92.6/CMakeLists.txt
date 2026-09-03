file(GLOB SOURCES ./*.cpp)
file(GLOB HEADERS ./*.h)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(imgui STATIC ${SOURCES} ${HEADERS})
target_link_libraries(imgui ${CMAKE_DL_LIBS})
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="pmp_imconfig.h")

target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)
if(EMSCRIPTEN)
  target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
  target_compile_options(imgui PUBLIC --use-port=emdawnwebgpu)
else()
  target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_WGPU)
endif()
if(NOT EMSCRIPTEN)
  target_include_directories(
    imgui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../${GLFW_SOURCE_DIR_INTERNAL}/include")
  target_link_libraries(imgui wgpu_native)
  if(APPLE)
    # the WebGPU backend's surface helper uses Cocoa: compile as Objective-C++
    set_source_files_properties(
      ${CMAKE_CURRENT_SOURCE_DIR}/imgui_impl_wgpu.cpp
      PROPERTIES COMPILE_OPTIONS "-x;objective-c++")
    target_link_libraries(imgui "-framework Cocoa" "-framework QuartzCore")
  endif()
else()
  target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()

set_property(TARGET imgui PROPERTY POSITION_INDEPENDENT_CODE ON)
