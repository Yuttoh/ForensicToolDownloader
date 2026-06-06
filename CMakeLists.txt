cmake_minimum_required(VERSION 3.20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(ForensicToolDownloader LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ForensicToolDownloader WIN32
    src/main.cpp
    src/app.rc
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts.example.ini
    ${CMAKE_CURRENT_BINARY_DIR}/scripts.example.ini
    COPYONLY
)

target_compile_definitions(ForensicToolDownloader PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MINGW)
    target_link_options(ForensicToolDownloader PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()

target_link_libraries(ForensicToolDownloader PRIVATE
    comctl32
    winhttp
    shell32
    user32
    gdi32
)
