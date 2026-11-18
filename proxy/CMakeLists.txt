cmake_minimum_required(VERSION 3.20)
project(fo4stereo_proxy LANGUAGES CXX)

add_library(fo4stereo_proxy SHARED src/proxy.cpp src/d3d11.def)
target_compile_features(fo4stereo_proxy PRIVATE cxx_std_17)
target_link_libraries(fo4stereo_proxy PRIVATE d3d11 dxgi d3dcompiler shell32 ole32)
target_include_directories(fo4stereo_proxy PRIVATE external/openvr src)
find_path(RENDERDOC_INCLUDE_DIR renderdoc_app.h HINTS "$ENV{ProgramFiles}/RenderDoc")
if(RENDERDOC_INCLUDE_DIR)
    target_include_directories(fo4stereo_proxy PRIVATE "${RENDERDOC_INCLUDE_DIR}")
    target_compile_definitions(fo4stereo_proxy PRIVATE FO4STEREO_HAS_RENDERDOC_API=1)
else()
    target_compile_definitions(fo4stereo_proxy PRIVATE FO4STEREO_HAS_RENDERDOC_API=0)
    message(STATUS "RenderDoc API header not found; in-game RenderDoc hotkey is disabled")
endif()
set_target_properties(fo4stereo_proxy PROPERTIES
    OUTPUT_NAME "d3d11"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
