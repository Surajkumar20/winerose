# ui_snapshot — renders every editor page to PNG offscreen (UI review without a DAW). A tool, not product code:
# it compiles the editor sources itself (as the plugin does) against a real Engine + Controller.
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
target_sources(ui_snapshot PRIVATE main.cpp)
target_compile_features(ui_snapshot PRIVATE cxx_std_20)
target_compile_definitions(ui_snapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
if(MSVC)
    target_compile_options(ui_snapshot PRIVATE /utf-8)
endif()
target_link_libraries(ui_snapshot
    PRIVATE winerose_ui_juce winerose_control winerose_engine winerose_params
            juce::juce_audio_processors juce::juce_audio_utils juce::juce_gui_basics
    PUBLIC  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
set_target_properties(ui_snapshot PROPERTIES FOLDER "Winerose/Tools")
