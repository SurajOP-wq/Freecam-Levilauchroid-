cmake_minimum_required(VERSION 3.22)
project(freecam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Путь к preloader SDK передаётся из yml через -DPRELOADER_SDK=...
# По умолчанию ищем в текущей папке или рядом с папкой мода
if(NOT DEFINED PRELOADER_SDK)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/preloader-sdk")
        set(PRELOADER_SDK "${CMAKE_CURRENT_SOURCE_DIR}/preloader-sdk")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/preloader-android-main")
        set(PRELOADER_SDK "${CMAKE_CURRENT_SOURCE_DIR}/preloader-android-main")
    else()
        set(PRELOADER_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../preloader-sdk")
    endif()
endif()

message(STATUS "Using PRELOADER_SDK path: ${PRELOADER_SDK}")

add_library(freecam SHARED
    freecam.cpp
)

target_include_directories(freecam PRIVATE
    ${PRELOADER_SDK}/src
    ${PRELOADER_SDK}/src/pl
)

find_library(log-lib log)
find_library(egl-lib EGL)

target_link_libraries(freecam
    PRIVATE
    # GlossHook статическая библиотека
    ${PRELOADER_SDK}/lib/ARM64/libGlossHook.a
    ${log-lib}
    ${egl-lib}
    # GetPreloaderInput резолвится в рантайме через dlsym — не линкуем
)

set_target_properties(freecam PROPERTIES
    OUTPUT_NAME "libfreecam"
    PREFIX ""
)
