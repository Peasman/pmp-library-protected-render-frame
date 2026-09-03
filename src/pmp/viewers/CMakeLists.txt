file(GLOB SRCS ./*.cpp)
file(GLOB HDRS ./*.h)
set(HDRS ${HDRS} "../mat_vec.h")

if(EMSCRIPTEN)

  add_library(pmp_viewers STATIC ${SRCS} ${HDRS})
  target_include_directories(
    pmp_viewers PRIVATE ${PROJECT_SOURCE_DIR}/${STB_IMAGE_WRITE_SOURCE_DIR}
                        ${PROJECT_SOURCE_DIR}/${STB_IMAGE_SOURCE_DIR})
  target_link_libraries(pmp_viewers stb_image stb_image_write imgui pmp
                        glfw3webgpu)

else()

  if(TRUE)
    add_library(pmp_viewers STATIC ${SRCS} ${HDRS})
    target_include_directories(
      pmp_viewers
      PRIVATE ${PROJECT_SOURCE_DIR}/${STB_IMAGE_WRITE_SOURCE_DIR}
              ${PROJECT_SOURCE_DIR}/${STB_IMAGE_SOURCE_DIR})
    target_link_libraries(
      pmp_viewers
      PUBLIC pmp
      imgui
      glfw
      glfw3webgpu
      wgpu_native
      PRIVATE stb_image
      stb_image_write)
    if(PMP_INSTALL)
      install(TARGETS pmp_viewers DESTINATION lib)
      install(FILES ${HDRS} DESTINATION include/pmp/viewers)
    endif()

    if(WITH_CLANG_TIDY)
      set_target_properties(pmp_viewers PROPERTIES CXX_CLANG_TIDY
                                                   "${CLANG_TIDY_COMMAND}")
    endif()
  endif()

endif()
