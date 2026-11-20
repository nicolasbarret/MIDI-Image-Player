cmake_minimum_required(VERSION 3.22)
project(MIDIImagePlayer VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
juce_add_gui_app(MIDIImagePlayer PRODUCT_NAME "MIDI Image Player" COMPANY_NAME "Nicolas" BUNDLE_ID "fr.nicolas.midiimageplayer")
juce_generate_juce_header(MIDIImagePlayer)
target_sources(MIDIImagePlayer PRIVATE Source/Main.cpp)
target_compile_definitions(MIDIImagePlayer PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(MIDIImagePlayer PRIVATE juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
