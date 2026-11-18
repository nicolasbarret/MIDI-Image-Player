
cmake_minimum_required(VERSION 3.22)

project(MIDI_Image_Player VERSION 1.0.0)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(JUCE)

juce_add_plugin(MIDI_Image_Player
    COMPANY_NAME "Nicolas"
    BUNDLE_ID "fr.nicolas.midiimageplayer"
    PLUGIN_MANUFACTURER_CODE Nics
    PLUGIN_CODE MImg
    FORMATS VST3
    PRODUCT_NAME "MIDI Image Player"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(MIDI_Image_Player)

target_sources(MIDI_Image_Player PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
)

target_compile_features(MIDI_Image_Player PRIVATE cxx_std_17)

target_compile_definitions(MIDI_Image_Player PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(MIDI_Image_Player PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
