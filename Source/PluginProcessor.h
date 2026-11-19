#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

class MIDIImagePlayerAudioProcessor final : public juce::AudioProcessor
{
public:
    MIDIImagePlayerAudioProcessor();
    ~MIDIImagePlayerAudioProcessor() override = default;

    void prepareToPlay(double, int) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    int getActivePitchClass() const noexcept;
    juce::String getImagePath(int pitchClass) const;
    void setImagePath(int pitchClass, const juce::File& file);

private:
    // A key includes both MIDI channel and note number, avoiding false Note Off matches.
    std::atomic<int> activeKey { -1 };

    mutable juce::CriticalSection imagePathsLock;
    std::array<juce::String, 12> imagePaths;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MIDIImagePlayerAudioProcessor)
};
