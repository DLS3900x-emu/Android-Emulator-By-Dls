cmake_minimum_required(VERSION 3.20)
project(Aetheron LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Aetheron targets Windows x64 (Windows Hypervisor Platform).")
endif()

file(GLOB_RECURSE AETHERON_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Aetheron WIN32 ${AETHERON_SOURCES})
target_compile_definitions(Aetheron PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(Aetheron PRIVATE winhvplatform winhvemulation d3d12 dxgi ws2_32 gdiplus dwmapi)
if(MSVC)
    target_compile_options(Aetheron PRIVATE /W3 /permissive- /utf-8)
else()
    target_link_options(Aetheron PRIVATE -static -municode)
endif()
