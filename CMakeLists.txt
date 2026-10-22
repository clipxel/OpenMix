cmake_minimum_required(VERSION 3.22)

# iPhone / iPad: cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS (see docs/MOVIL.md). arm64 only, iOS 15+.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS devices" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "")
endif()

# macOS: one universal app for Apple Silicon and Intel (ignored on Windows / Linux).
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS build" FORCE)
endif()

project(OpenMix VERSION 2.4.1 LANGUAGES C CXX)

# The phone version has its own number and its own releases (tags movil-v*, see docs/MOVIL.md):
# desktop updates and phone updates are not the same.
set(OPENMIX_MOBILE_VERSION 0.1.0)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(OPENMIX_APP_VERSION ${OPENMIX_MOBILE_VERSION})
else()
    set(OPENMIX_APP_VERSION ${PROJECT_VERSION})
endif()
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")   # macOS (iOS sets 15.0 above)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE)

juce_add_gui_app(OpenMix
    PRODUCT_NAME "OpenMix"
    VERSION ${OPENMIX_APP_VERSION}
    COMPANY_NAME "OpenMix"
    BUNDLE_ID "app.openmix.OpenMix"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OpenMix necesita acceso a la interfaz de audio para entradas externas."
    # iOS only (ignored elsewhere): landscape, full screen, keeps playing with the screen locked,
    # and its Documents folder shows up in Files › On My iPhone › OpenMix for copying music in.
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    REQUIRES_FULL_SCREEN TRUE
    STATUS_BAR_HIDDEN TRUE
    BACKGROUND_AUDIO_ENABLED TRUE
    FILE_SHARING_ENABLED TRUE
    DOCUMENT_BROWSER_ENABLED TRUE
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "OpenMix usa Bluetooth para conectarse a controladoras MIDI inalámbricas.")

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
    JUCE_USE_MP3AUDIOFORMAT=1      # macOS decodes with the OS first; this reads the MP3s it refuses
    $<$<PLATFORM_ID:Windows>:JUCE_USE_WINDOWS_MEDIA_FORMAT=1>)   # Windows: WMA/AAC/M4A via Media Foundation

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
