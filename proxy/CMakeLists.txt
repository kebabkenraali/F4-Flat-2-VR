cmake_minimum_required(VERSION 3.20)
project(fo4stereo_proxy LANGUAGES CXX)

add_library(fo4stereo_proxy SHARED src/proxy.cpp src/d3d11.def)
target_compile_features(fo4stereo_proxy PRIVATE cxx_std_17)
target_link_libraries(fo4stereo_proxy PRIVATE d3d11 dxgi)
target_include_directories(fo4stereo_proxy PRIVATE external/openvr src)
set_target_properties(fo4stereo_proxy PROPERTIES
    OUTPUT_NAME "d3d11"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
