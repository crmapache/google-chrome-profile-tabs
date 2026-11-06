# Build on Windows with Visual Studio or MinGW:
#   cmake -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(GoogleChromeProfileTabs LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/W4>)
  # The manifest comes from res/app.rc; a generated one would clash with it or replace it.
  add_link_options(/MANIFEST:NO)
else()
  add_link_options(-static -municode)
endif()

add_executable(GoogleChromeProfileTabs WIN32
  src/main.cpp src/strip.cpp src/chrome.cpp src/settings.cpp res/app.rc)
target_compile_definitions(GoogleChromeProfileTabs PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(GoogleChromeProfileTabs PRIVATE
  d2d1 dwrite windowscodecs dwmapi comctl32 shell32 ole32 oleaut32 propsys uuid uxtheme)
