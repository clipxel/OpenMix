cmake_minimum_required(VERSION 3.22)

project(OpenMix VERSION 1.0.0 LANGUAGES C CXX)
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

juce_add_binary_data(OpenMixAssets SOURCES Resources/logo-mark.png)
target_include_directories(OpenMix PRIVATE Source)
target_include_directories(OpenMix SYSTEM PRIVATE
    external/signalsmith-stretch
    external/signalsmith-linear/include)

target_compile_definitions(OpenMix PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenMix,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenMix,JUCE_VERSION>"
    $<$<NOT:$<PLATFORM_ID:Darwin>>:JUCE_USE_MP3AUDIOFORMAT=1>)   # macOS decodes MP3/AAC with the OS

if(APPLE)
    target_link_libraries(OpenMix PRIVATE "-framework AudioToolbox")
endif()
if(MSVC)
    target_compile_options(OpenMix PRIVATE /utf-8 /bigobj)
endif()

target_link_libraries(OpenMix
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        OpenMixAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
