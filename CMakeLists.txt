cmake_minimum_required(VERSION 3.28)
project(fruityhost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(fruityhost
    main.cpp
    fruitynode.cpp
    fstfile.cpp
    presetui.cpp
    node.cpp
)
target_compile_definitions(fruityhost PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX)
target_compile_options(fruityhost PRIVATE /W4 /utf-8 /EHsc)
target_link_libraries(fruityhost PRIVATE ole32 user32 comdlg32 shell32 uuid)
