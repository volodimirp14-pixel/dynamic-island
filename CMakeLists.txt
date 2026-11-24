cmake_minimum_required(VERSION 3.21)
project(DynamicIsland LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ redistributable needed

if(NOT MSVC)
  message(FATAL_ERROR "Build with MSVC (Visual Studio Build Tools) and the Windows SDK; it provides the C++/WinRT headers.")
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
add_compile_options(/utf-8 /permissive- /EHsc /W4 /Zc:__cplusplus /bigobj)
# Security hardening (standalone 0.9): Control Flow Guard, extra checks, and at
# link time CET shadow stacks + static imports resolved from System32 only.
add_compile_options(/guard:cf /sdl)
add_link_options(/guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /DEPENDENTLOADFLAG:0x800)
include_directories(src third_party)

# ── Shared by both programs: paths, log, settings, island commands ──────────
add_library(di_common STATIC
  src/common/IpcClient.cpp
  src/common/Log.cpp
  src/common/Paths.cpp
  src/common/Security.cpp
  src/common/Settings.cpp
)
target_link_libraries(di_common PUBLIC shell32 ole32 user32)

# ── DynamicIsland.exe (the island) ──────────────────────────────────────────
add_executable(DynamicIsland WIN32
  src/island/main.cpp
  src/island/IslandCore.cpp
  src/island/app/Control.cpp
  src/island/app/SingleInstance.cpp
  src/island/diag/Diagnostics.cpp
  src/island/settings/SettingsWatcher.cpp
  res/island.rc
)
target_link_options(DynamicIsland PRIVATE /MANIFEST:NO)  # our own manifest comes from island.rc
# Same libraries the Windhawk mod linked (@compilerOptions), plus WinRT.
target_link_libraries(DynamicIsland PRIVATE di_common hid
  windowsapp runtimeobject oleaut32 shcore d2d1 dwrite dwmapi gdi32
  windowscodecs setupapi winhttp pdh winmm)

# ── DynamicIslandLauncher.exe (settings and control window, WebView2) ────────
# WebView2 SDK from NuGet (headers + static loader; the runtime is part of Windows 10/11).
set(WEBVIEW2_VERSION 1.0.2903.40)
set(WEBVIEW2_DIR ${CMAKE_BINARY_DIR}/webview2-${WEBVIEW2_VERSION})
if(NOT EXISTS ${WEBVIEW2_DIR}/build/native/include/WebView2.h)
  file(DOWNLOAD https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}
       ${CMAKE_BINARY_DIR}/webview2.nupkg STATUS dl_status TLS_VERIFY ON)
  list(GET dl_status 0 dl_code)
  if(NOT dl_code EQUAL 0)
    message(FATAL_ERROR "WebView2 SDK download failed: ${dl_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/webview2.nupkg DESTINATION ${WEBVIEW2_DIR})
endif()

add_executable(DynamicIslandLauncher WIN32
  src/launcher/main.cpp
  src/launcher/Autostart.cpp
  src/launcher/IslandProcess.cpp
  res/launcher.rc
)
target_include_directories(DynamicIslandLauncher PRIVATE ${WEBVIEW2_DIR}/build/native/include)
target_link_options(DynamicIslandLauncher PRIVATE /MANIFEST:NO)
target_link_libraries(DynamicIslandLauncher PRIVATE di_common
  ${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib dwmapi advapi32 version)
