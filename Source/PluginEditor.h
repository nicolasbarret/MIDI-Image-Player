#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>

class MIDIImagePlayerAudioProcessorEditor final
    : public juce::AudioProcessorEditor,
      private juce::Timer
{
public:
    explicit MIDIImagePlayerAudioProcessorEditor(MIDIImagePlayerAudioProcessor& processor);
    ~MIDIImagePlayerAudioProcessorEditor() override = default;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void timerCallback() override;
    void loadImage(int pitchClass);
    void refreshPreview();

    MIDIImagePlayerAudioProcessor& processor;
    std::unique_ptr<juce::FileChooser> fileChooser;

    std::array<juce::Label, 12> noteLabels;
    std::array<juce::TextButton, 12> loadButtons;

    juce::ImageComponent preview;
    int displayedPitchClass = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        MIDIImagePlayerAudioProcessorEditor)
};
