cmake_minimum_required(VERSION 3.22)

# Build only for this Mac's processor (the Rust AI engine is compiled natively)
if(APPLE)
    execute_process(COMMAND uname -m OUTPUT_VARIABLE HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_OSX_ARCHITECTURES "${HOST_ARCH}" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(NebulaClean VERSION 1.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# ---- AI engine (Rust: DeepFilterNet 3) ------------------------------------------
FetchContent_Declare(Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG        v0.5.1
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(Corrosion)
corrosion_import_crate(MANIFEST_PATH ai/Cargo.toml CRATES eclipse_ai CRATE_TYPES staticlib PROFILE release)

juce_add_plugin(NebulaClean
    COMPANY_NAME                "Eclipse Audio"
    BUNDLE_ID                   com.eclipseaudio.eclipsecore
    PLUGIN_MANUFACTURER_CODE    Nbla
    PLUGIN_CODE                 NbCl
    FORMATS                     VST3 AU Standalone
    PRODUCT_NAME                "Eclipse Core"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    VST3_CATEGORIES             Fx Restoration
    AU_MAIN_TYPE                kAudioUnitType_Effect)

juce_add_binary_data(EclipseAssets SOURCES Assets/skin.jpg)

target_sources(NebulaClean PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(NebulaClean PRIVATE Source ai/include)

target_compile_definitions(NebulaClean PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(NebulaClean
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        EclipseAssets
        eclipse_ai
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
