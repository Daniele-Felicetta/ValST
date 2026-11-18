cmake_minimum_required(VERSION 3.22)

# Runtime C++ collegato staticamente: sul PC di destinazione NON serve installare
# nessun "Visual C++ Redistributable". (Va impostato prima di project().)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ValSTMSEQ VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/MP /utf-8)
endif()

# --- JUCE (scaricato automaticamente al primo configure, in build/_deps) ---------------
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.17-security
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

set(VALST_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/EqGraph.cpp)

# --- Il plugin -------------------------------------------------------------------------
juce_add_plugin(ValSTMSEQ
    COMPANY_NAME                "ValST"
    PRODUCT_NAME                "ValST MS EQ"
    PLUGIN_MANUFACTURER_CODE    Vlst
    PLUGIN_CODE                 Vmse
    FORMATS                     VST3
    VST3_CATEGORIES             Fx EQ
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(ValSTMSEQ PRIVATE ${VALST_SOURCES})
target_include_directories(ValSTMSEQ PRIVATE Source)

target_compile_definitions(ValSTMSEQ PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(ValSTMSEQ
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --- Test (DSP, caricamento del VST3 reale, snapshot della GUI) -----------------------
option(VALST_BUILD_TESTS "Compila l'eseguibile di test ValSTTests" OFF)
if(VALST_BUILD_TESTS)
    juce_add_console_app(ValSTTests PRODUCT_NAME "ValSTTests")
    target_sources(ValSTTests PRIVATE Tests/Tests.cpp ${VALST_SOURCES})
    target_include_directories(ValSTTests PRIVATE Source)
    target_compile_definitions(ValSTTests PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ValSTTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
