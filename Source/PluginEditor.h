#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>

class MIDIImagePlayerAudioProcessorEditor
    : public juce::AudioProcessorEditor,
      private juce::Timer
{
public:
    explicit MIDIImagePlayerAudioProcessorEditor(
        MIDIImagePlayerAudioProcessor&);

    ~MIDIImagePlayerAudioProcessorEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void loadImage(int index);
    void refreshPreview();

    MIDIImagePlayerAudioProcessor& processor;

    std::unique_ptr<juce::FileChooser> fileChooser;

    std::array<juce::TextButton, 12> loadButtons;
    std::array<juce::Label, 12> noteLabels;

    juce::ImageComponent preview;
    juce::Image currentImage;

    int displayedPitchClass = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        MIDIImagePlayerAudioProcessorEditor)
};
