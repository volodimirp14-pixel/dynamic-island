cmake_minimum_required(VERSION 3.21)
project(DynamicIsland LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ redistributable needed

if(NOT MSVC)
  message(FATAL_ERROR "Build with MSVC (Visual Studio 2022 / Build Tools) and the Windows SDK; it provides the C++/WinRT headers.")
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
add_compile_options(/utf-8 /permissive- /EHsc /W4 /Zc:__cplusplus /bigobj)

# ── DynamicIsland.exe (the island) ──────────────────────────────────────────
add_executable(DynamicIsland WIN32
  src/island/main.cpp
  src/island/IslandCore.cpp
  src/island/app/Control.cpp
  src/island/app/Paths.cpp
  src/island/app/SingleInstance.cpp
  src/island/diag/Diagnostics.cpp
  src/island/log/Log.cpp
  src/island/settings/Settings.cpp
  src/island/settings/SettingsWatcher.cpp
  res/island.rc
)
target_link_options(DynamicIsland PRIVATE /MANIFEST:NO)  # our own manifest comes from island.rc
# Same libraries the Windhawk mod linked (@compilerOptions), plus WinRT.
target_link_libraries(DynamicIsland PRIVATE
  windowsapp runtimeobject ole32 oleaut32 shcore d2d1 dwrite dwmapi gdi32 user32 shell32
  windowscodecs setupapi winhttp pdh winmm)
