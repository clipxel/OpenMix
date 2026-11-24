cmake_minimum_required(VERSION 3.22)

# macOS: one universal app for Apple Silicon and Intel (ignored on Windows / Linux).
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal macOS build" FORCE)
endif()

project(OpenMix VERSION 2.7.2 LANGUAGES C CXX)
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
    external/signalsmith-linear/include
    external/onnxruntime/include)

target_compile_definitions(OpenMix PRIVATE
    JUCE_WEB_BROWSER=0
    $<IF:$<PLATFORM_ID:Linux>,JUCE_USE_CURL=1,JUCE_USE_CURL=0>   # Linux downloads with libcurl; macOS/Windows use the OS
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenMix,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenMix,JUCE_VERSION>"
    JUCE_USE_MP3AUDIOFORMAT=1      # macOS decodes with the OS first; this reads the MP3s it refuses
    $<$<PLATFORM_ID:Windows>:JUCE_USE_WINDOWS_MEDIA_FORMAT=1>)   # Windows: WMA/AAC/M4A via Media Foundation

# Google Drive: the OAuth client of the OpenMix project, built in so "Iniciar sesión con Google" is one click.
# Kept out of the public code: from the environment (CI secrets) or a private local file, one value per line:
# google-oauth.local → line 1 the client id, line 2 the client secret.
set(OPENMIX_GOOGLE_CLIENT_ID "$ENV{OPENMIX_GOOGLE_CLIENT_ID}")
set(OPENMIX_GOOGLE_CLIENT_SECRET "$ENV{OPENMIX_GOOGLE_CLIENT_SECRET}")
if(OPENMIX_GOOGLE_CLIENT_ID STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/google-oauth.local")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/google-oauth.local" OPENMIX_GOOGLE_OAUTH LIMIT_COUNT 2)
    list(LENGTH OPENMIX_GOOGLE_OAUTH n)
    if(n EQUAL 2)
        list(GET OPENMIX_GOOGLE_OAUTH 0 OPENMIX_GOOGLE_CLIENT_ID)
        list(GET OPENMIX_GOOGLE_OAUTH 1 OPENMIX_GOOGLE_CLIENT_SECRET)
    endif()
endif()
string(STRIP "${OPENMIX_GOOGLE_CLIENT_ID}" OPENMIX_GOOGLE_CLIENT_ID)
string(STRIP "${OPENMIX_GOOGLE_CLIENT_SECRET}" OPENMIX_GOOGLE_CLIENT_SECRET)
configure_file(Source/Library/GoogleDriveClient.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/GoogleDriveClient.h" @ONLY)
target_include_directories(OpenMix PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/google-oauth.local")

# Stems: ONNX Runtime is loaded at run time (never linked). Pass the folder holding the platform's library
# (libonnxruntime.dylib / onnxruntime.dll / libonnxruntime.so, from the Microsoft.ML.OnnxRuntime package)
# with -DOPENMIX_ONNXRUNTIME_LIB=<file> and it is copied next to the app; without it, stems are unavailable.
set(OPENMIX_ONNXRUNTIME_LIB "" CACHE FILEPATH "ONNX Runtime shared library to bundle (stems)")
if(OPENMIX_ONNXRUNTIME_LIB)
    if(APPLE)
        add_custom_command(TARGET OpenMix POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:OpenMix>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENMIX_ONNXRUNTIME_LIB}" "$<TARGET_BUNDLE_CONTENT_DIR:OpenMix>/Frameworks/libonnxruntime.dylib")
    else()
        get_filename_component(ORT_NAME "${OPENMIX_ONNXRUNTIME_LIB}" NAME)
        add_custom_command(TARGET OpenMix POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENMIX_ONNXRUNTIME_LIB}" "$<TARGET_FILE_DIR:OpenMix>/${ORT_NAME}")
    endif()
endif()

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
