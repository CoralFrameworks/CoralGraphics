cmake_minimum_required(VERSION 3.19)
set(TARGET geometry)
set(PUBLIC_HDR_DIR include)

if (MSVC)
  set(OPTIMIZATION_FLAGS /fp:fast)
else()
  set(OPTIMIZATION_FLAGS -ffast-math -fno-finite-math-only -ffp-contract=fast)
endif()

# ==================================================================================================
# Sources and headers
# ==================================================================================================

set(PUBLIC_HDRS
    include/geometry/Arc.h
    include/geometry/Color.h
    include/geometry/compiler.h
    include/geometry/Formula.h
    include/geometry/Gradient.h
    include/geometry/Matrix.h
    include/geometry/MatrixDecomposition.h
    include/geometry/Point.h
    include/geometry/Quaternion.h
    include/geometry/Rational.h
    include/geometry/Rect.h
    include/geometry/RoundingRadii.h
    include/geometry/RoundRect.h
    include/geometry/Scalar.h
    include/geometry/Sigma.h
    include/geometry/Size.h
    include/geometry/type_traits.h
    include/geometry/Vector.h
)

set(SRCS
    src/Arc.cpp
    src/Color.cpp
    src/Formula.cpp
    src/Gradient.cpp
    src/Matrix.cpp
    src/Quaternion.cpp
    src/Rational.cpp
    src/RoundingRadii.cpp
    src/RoundRect.cpp
    src/Sigma.cpp
    src/Vector.cpp
)

# ==================================================================================================
# Includes & target definition
# ==================================================================================================
include_directories(${PUBLIC_HDR_DIR})

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})
target_compile_options(${TARGET} PRIVATE ${OPTIMIZATION_FLAGS})
set(_coral_public_include "${PUBLIC_HDR_DIR}")
if (NOT IS_ABSOLUTE "${_coral_public_include}")
  set(_coral_public_include "${CMAKE_CURRENT_SOURCE_DIR}/${_coral_public_include}")
endif ()
target_include_directories(${TARGET} PUBLIC $<BUILD_INTERFACE:${_coral_public_include}> $<INSTALL_INTERFACE:include>)
set_target_properties(${TARGET} PROPERTIES FOLDER Libs)

# ==================================================================================================
# Dependencies
# ==================================================================================================
target_link_libraries(${TARGET} PUBLIC utils)

# ==================================================================================================
# Installation
# ==================================================================================================
install(TARGETS ${TARGET} EXPORT CoralUITargets ARCHIVE DESTINATION lib)
install(DIRECTORY ${PUBLIC_HDR_DIR}/${TARGET} DESTINATION include)

# ==================================================================================================
# Test
# ==================================================================================================
if (CORAL_BUILD_TESTS)
# Required for the `add_test` at the end of this block to be reachable: CMake writes a directory's
# `CTestTestfile.cmake` only when `enable_testing()` has been called in that directory. See the longer note in
# CMakeLists.txt.
enable_testing()
add_executable(test_${TARGET}
    tests/test_asserts.h
    tests/test_Arc.cpp
    tests/test_Color.cpp
    tests/test_geometry.cpp
    tests/test_Matrix.cpp
    tests/test_Rational.cpp
    tests/test_Rect.cpp
    tests/test_RoundingRadii.cpp
    tests/test_RoundRect.cpp
    tests/test_saturated_math.cpp
    tests/test_Size.cpp
)
target_link_libraries(test_${TARGET} PRIVATE geometry gtest)
set_target_properties(test_${TARGET} PROPERTIES FOLDER Tests)

# Registered with CTest, which this suite was not -- see the longer note in CMakeLists.txt. Without it
# `ctest` in the build root ran fourteen tests and none of the graphics ones, while this executable was built and run by
# hand.
add_test(NAME test_${TARGET} COMMAND test_${TARGET})
set_tests_properties(test_${TARGET} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
endif ()
