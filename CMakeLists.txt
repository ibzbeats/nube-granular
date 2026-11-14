cmake_minimum_required(VERSION 3.22)

# macOS: version "universal" (Intel + Apple Silicon). Debe ir ANTES de project().
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Arquitecturas de macOS" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Version minima de macOS" FORCE)
endif()

project(NubeGranular VERSION 1.0.0 LANGUAGES C CXX)

# Windows: el runtime de C++ va incluido dentro del plugin,
# asi NO hace falta instalar nada mas en el ordenador.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Descarga JUCE automaticamente (version fija para que el resultado sea siempre el mismo).
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(NubeGranular
    COMPANY_NAME "Nube Audio"
    PLUGIN_MANUFACTURER_CODE Nbau
    PLUGIN_CODE Ngrn
    FORMATS VST3
    PRODUCT_NAME "Nube Granular"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx)

target_sources(NubeGranular PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/NubeEngine.h)

target_compile_definitions(NubeGranular PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(NubeGranular
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags)
