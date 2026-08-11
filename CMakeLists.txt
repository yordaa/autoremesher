cmake_minimum_required(VERSION 3.16)

cmake_policy(SET CMP0091 NEW)
project(AutoRemesherCli LANGUAGES CXX)

if(APPLE)
    set(AUTOREMESHER_DEFAULT_TBB_ROOT "/opt/homebrew/opt/tbb")
else()
    set(AUTOREMESHER_DEFAULT_TBB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tbb")
endif()
set(AUTOREMESHER_TBB_ROOT "${AUTOREMESHER_DEFAULT_TBB_ROOT}" CACHE PATH "TBB installation used by the Qt-free CLI")

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
    set(TBB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(TBB_BUILD_TBBMALLOC OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_TBBMALLOC_PROXY OFF CACHE BOOL "" FORCE)
    set(TBB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/tbb EXCLUDE_FROM_ALL)
    set(TBB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tbb/include")
    set(TBB_LIBRARY tbb_static)
else()
    find_path(TBB_INCLUDE_DIR tbb/parallel_for.h
        HINTS ${AUTOREMESHER_TBB_ROOT}
        PATH_SUFFIXES include
        REQUIRED)
endif()

if(APPLE)
    find_package(TBB 2023.0.0 EXACT CONFIG REQUIRED
        HINTS ${AUTOREMESHER_TBB_ROOT}/lib/cmake/TBB
        NO_DEFAULT_PATH)
    find_file(TBB_LIBRARY libtbb.a
        HINTS ${AUTOREMESHER_TBB_ROOT}
        PATH_SUFFIXES lib
        REQUIRED)
elseif(NOT WIN32)
    find_library(TBB_LIBRARY tbb
        HINTS ${AUTOREMESHER_TBB_ROOT}
        PATH_SUFFIXES lib build2/Release
        REQUIRED)
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

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME cli-black-box
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_black_box.py $<TARGET_FILE:autoremesher-cli>)
endif()
