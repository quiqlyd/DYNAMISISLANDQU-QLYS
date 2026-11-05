cmake_minimum_required(VERSION 3.20)
project(DynamisIsland LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Statik CRT: kullanıcının ayrıca VC++ runtime kurmasına gerek kalmaz.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(DynamisIsland WIN32
    src/main.cpp
    src/Island.cpp
    src/MediaService.cpp
    src/Eyes.cpp
    src/Updater.cpp
    src/Companion.cpp
    src/Sounds.cpp
    src/Watchers.cpp
    src/Command.cpp
    src/Tools.cpp
    src/Sys.cpp
    src/Net.cpp
    src/Lyrics.cpp
    src/Extras.cpp
    src/Home.cpp
    src/Diary.cpp
    src/Notify.cpp
    src/Weather.cpp
    src/Island.h
    src/MediaService.h
    src/Spring.h
    src/Eyes.h
    src/Updater.h
    src/Companion.h
    src/Sounds.h
    src/Watchers.h
    src/Command.h
    src/Sys.h
    src/IslandUi.h
    src/Net.h
    src/Lyrics.h
    src/Weather.h
    src/Notify.h
)

set(APP_VERSION 0 CACHE STRING "Sürüm numarası (CI çalıştırma numarası)")
target_compile_definitions(DynamisIsland PRIVATE APP_VERSION=${APP_VERSION} UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(DynamisIsland PRIVATE /utf-8 /W4 /permissive- /bigobj)
target_link_libraries(DynamisIsland PRIVATE
    d2d1 dwrite windowscodecs winhttp winmm shcore windowsapp shell32 advapi32 user32 gdi32 ole32 uuid setupapi cfgmgr32 d3d11 dxgi dwmapi psapi wbemuuid oleaut32)
