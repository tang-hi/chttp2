function(set_project_warnings project_name)
  if(MSVC)
    set(WARNINGS
      /W4
      /wd4100  # unreferenced formal parameter
    )

    if(chttp2_WARNINGS_AS_ERRORS)
      list(APPEND WARNINGS /WX)
    endif()
  else()
    set(WARNINGS
      -Wall
      -Wextra
      -Wshadow
      -Wnon-virtual-dtor
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wnull-dereference
      -Wdouble-promotion
    )

    # Clang 22+ warns about C2Y extensions used by Catch2; suppress if supported.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wno-c2y-extensions HAS_WNO_C2Y_EXTENSIONS)
    if(HAS_WNO_C2Y_EXTENSIONS)
      list(APPEND WARNINGS -Wno-c2y-extensions)
    endif()

    # GCC-specific warnings
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      list(APPEND WARNINGS
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wuseless-cast
      )
    endif()

    if(chttp2_WARNINGS_AS_ERRORS)
      list(APPEND WARNINGS -Werror)
    endif()
  endif()

  target_compile_options(${project_name} PRIVATE ${WARNINGS})
endfunction()
