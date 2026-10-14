cmake_minimum_required(VERSION 3.22)

# macOS: one universal app for Apple Silicon and Intel (ignored on Windows / Linux).
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS build" FORCE)
endif()

project(OpenMix VERSION 2.0.2 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE)

juce_add_gui_app(OpenMix
    PRODUCT_NAME "OpenMix"
    COMPANY_NAME "OpenMix"
    BUNDLE_ID "app.openmix.OpenMix"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OpenMix necesita acceso a la interfaz de audio para entradas externas.")

file(GLOB_RECURSE OPENMIX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp)
if(APPLE)
    file(GLOB_RECURSE OPENMIX_MAC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.mm)
    list(APPEND OPENMIX_SOURCES ${OPENMIX_MAC_SOURCES})
endif()

target_sources(OpenMix PRIVATE ${OPENMIX_SOURCES})

juce_add_binary_data(OpenMixAssets SOURCES Resources/logo-mark.png Resources/mappings/library.json)
target_include_directories(OpenMix PRIVATE Source)
target_include_directories(OpenMix SYSTEM PRIVATE
    external/signalsmith-stretch
    external/signalsmith-linear/include)

target_compile_definitions(OpenMix PRIVATE
    JUCE_WEB_BROWSER=0
    $<IF:$<PLATFORM_ID:Linux>,JUCE_USE_CURL=1,JUCE_USE_CURL=0>   # Linux downloads with libcurl; macOS/Windows use the OS
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenMix,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenMix,JUCE_VERSION>"
    $<$<NOT:$<PLATFORM_ID:Darwin>>:JUCE_USE_MP3AUDIOFORMAT=1>)   # macOS decodes MP3/AAC with the OS

if(APPLE)
    target_link_libraries(OpenMix PRIVATE "-framework AudioToolbox")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(CURL REQUIRED)
    target_link_libraries(OpenMix PRIVATE CURL::libcurl)
endif()
if(MSVC)
    target_compile_options(OpenMix PRIVATE /utf-8 /bigobj)
endif()

# Windows: low-latency vendor drivers. The SDK is not in the repository: pass its folder
# (downloaded from the vendor at build time, GPLv3 option) with -DOPENMIX_ASIO_SDK=<path>.
set(OPENMIX_ASIO_SDK "" CACHE PATH "Low-latency driver SDK folder (Windows)")
if(WIN32 AND OPENMIX_ASIO_SDK)
    target_compile_definitions(OpenMix PRIVATE JUCE_ASIO=1)
    target_include_directories(OpenMix SYSTEM PRIVATE "${OPENMIX_ASIO_SDK}/common")
endif()

target_link_libraries(OpenMix
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_cryptography
        OpenMixAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
