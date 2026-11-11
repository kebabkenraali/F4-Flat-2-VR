cmake_minimum_required(VERSION 3.20)

# CommonLibF4 is a git submodule (external/commonlibf4) with patches/commonlibf4-winapi-msvc-14.44.patch applied.
# vcpkg: set VCPKG_ROOT (environment) or pass -DVCPKG_ROOT_DIR=...; keep paths short (e.g. C:/vcpkg).
set(COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/commonlibf4" CACHE PATH "")
if(DEFINED ENV{VCPKG_ROOT})
	set(VCPKG_ROOT_DIR "$ENV{VCPKG_ROOT}" CACHE PATH "")
else()
	set(VCPKG_ROOT_DIR "C:/vcpkg" CACHE PATH "")
endif()

set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(fo4flat_probe VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory("${COMMONLIB_DIR}/CommonLibF4" CommonLibF4)
target_compile_definitions(CommonLibF4 PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

find_package(fmt REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)

add_library(fo4flat_probe SHARED src/main.cpp)
target_compile_features(fo4flat_probe PRIVATE cxx_std_23)
target_compile_definitions(fo4flat_probe PRIVATE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(fo4flat_probe PRIVATE CommonLibF4::CommonLibF4 fmt::fmt spdlog::spdlog Xinput9_1_0)
target_include_directories(fo4flat_probe PRIVATE "${COMMONLIB_DIR}/CommonLibF4/include" "${COMMONLIB_DIR}/CommonLibF4/src")
set_target_properties(fo4flat_probe PROPERTIES OUTPUT_NAME "fo4flat_probe")
