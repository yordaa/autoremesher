cmake_minimum_required(VERSION 3.16)

cmake_policy(SET CMP0091 NEW)
project(AutoRemesherCli VERSION 1.0.1 LANGUAGES CXX)

if(WIN32)
    set(AUTOREMESHER_DEFAULT_USE_BUNDLED_TBB ON)
else()
    set(AUTOREMESHER_DEFAULT_USE_BUNDLED_TBB OFF)
endif()
option(AUTOREMESHER_USE_BUNDLED_TBB "Build the bundled TBB instead of using find_package" ${AUTOREMESHER_DEFAULT_USE_BUNDLED_TBB})

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(AUTOREMESHER_USE_BUNDLED_TBB)
    set(TBB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(TBB_BUILD_TBBMALLOC OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_TBBMALLOC_PROXY OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/tbb EXCLUDE_FROM_ALL)
    set(TBB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tbb/include")
    set(TBB_LIBRARY tbb_static)
else()
    find_package(TBB CONFIG REQUIRED)
    set(TBB_LIBRARY TBB::tbb)
endif()

add_library(autoremesher-core STATIC
    src/AutoRemesher/autoremesher.cpp
    src/AutoRemesher/constrainedleastsquares.cpp
    src/AutoRemesher/framefield.cpp
    src/AutoRemesher/isotropicremesher.cpp
    src/AutoRemesher/meshseparator.cpp
    src/AutoRemesher/mixedintegerleastsquares.cpp
    src/AutoRemesher/parameterizer.cpp
    src/AutoRemesher/positionkey.cpp
    src/AutoRemesher/quadextractor.cpp
    src/AutoRemesher/quadparameterizer.cpp
    src/AutoRemesher/singularitysimplifier.cpp
    src/AutoRemesher/surfacemesh.cpp
    thirdparty/isotropicremesher/axisalignedboundingboxtree.cpp
    thirdparty/isotropicremesher/isotropichalfedgemesh.cpp
    thirdparty/isotropicremesher/isotropicremesher.cpp)
target_compile_features(autoremesher-core PUBLIC cxx_std_14)
target_compile_definitions(autoremesher-core PUBLIC EIGEN_MPL2_ONLY)
if(WIN32)
    target_compile_definitions(autoremesher-core PUBLIC
        NOMINMAX
        TBB_PREVIEW_GLOBAL_CONTROL=1
        _USE_MATH_DEFINES
        __TBB_NO_IMPLICIT_LINKAGE=1)
endif()
target_include_directories(autoremesher-core
    PUBLIC
        include
        thirdparty/eigen
    PRIVATE
        src
        thirdparty/isotropicremesher
        ${TBB_INCLUDE_DIR})
target_link_libraries(autoremesher-core PUBLIC ${TBB_LIBRARY})
if(APPLE)
    target_compile_definitions(autoremesher-core PRIVATE AUTO_REMESHER_USE_ACCELERATE=1)
    target_link_libraries(autoremesher-core PUBLIC "-framework Accelerate")
endif()

add_executable(autoremesher-cli src/cli_main.cpp)
target_compile_features(autoremesher-cli PRIVATE cxx_std_14)
target_include_directories(autoremesher-cli PRIVATE src ${TBB_INCLUDE_DIR})
target_link_libraries(autoremesher-cli PRIVATE autoremesher-core)
if(APPLE)
    set_target_properties(autoremesher-cli PROPERTIES INSTALL_RPATH "@loader_path")
endif()
install(TARGETS autoremesher-cli RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(coordinate-round-trip-test tests/coordinate_round_trip.cpp)
    target_link_libraries(coordinate-round-trip-test PRIVATE autoremesher-core)
    add_test(NAME coordinate-round-trip COMMAND coordinate-round-trip-test)

    add_executable(quadextractor-interpolation-test tests/quadextractor_interpolation.cpp)
    target_include_directories(quadextractor-interpolation-test PRIVATE ${TBB_INCLUDE_DIR})
    target_link_libraries(quadextractor-interpolation-test PRIVATE autoremesher-core)
    add_test(NAME quadextractor-interpolation COMMAND quadextractor-interpolation-test)

    add_executable(quadextractor-hole-filling-test tests/quadextractor_hole_filling.cpp)
    target_link_libraries(quadextractor-hole-filling-test PRIVATE autoremesher-core)
    add_test(NAME quadextractor-hole-filling COMMAND quadextractor-hole-filling-test)

    add_executable(output-validation-test tests/output_validation.cpp)
    target_link_libraries(output-validation-test PRIVATE autoremesher-core)
    add_test(NAME output-validation COMMAND output-validation-test)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME cli-black-box
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_black_box.py $<TARGET_FILE:autoremesher-cli>)
endif()
