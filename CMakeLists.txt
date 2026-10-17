cmake_minimum_required(VERSION 3.15)
project(AetherSpaceReverb VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)

# Wskaż ścieżkę do pobranego folderu JUCE lub użyj FetchContent:
include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.3
)
FetchContent_MakeAvailable(juce)

juce_add_plugin(AetherSpaceReverb
    COMPANY_NAME "AetherAudioDSP"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Aeth
    PLUGIN_CODE AsRv
    FORMATS VST3 Standalone
    PRODUCT_NAME "AetherSpace Reverb"
)

target_sources(AetherSpaceReverb PRIVATE AetherSpaceReverbVST3.cpp)

target_compile_definitions(AetherSpaceReverb PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(AetherSpaceReverb
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
