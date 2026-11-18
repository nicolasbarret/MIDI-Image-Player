
#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

class MIDIImagePlayerAudioProcessor : public juce::AudioProcessor
{
public:
    MIDIImagePlayerAudioProcessor();
    ~MIDIImagePlayerAudioProcessor() override = default;

    void prepareToPlay(double, int) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(
        const BusesLayout& layouts) const override;

    void processBlock(
        juce::AudioBuffer<float>&,
        juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override
    {
        return JucePlugin_Name;
    }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}

    const juce::String getProgramName(int) override
    {
        return {};
    }

    void changeProgramName(
        int, const juce::String&) override {}

    void getStateInformation(
        juce::MemoryBlock& destData) override;

    void setStateInformation(
        const void* data, int sizeInBytes) override;

    int getActivePitchClass() const
    {
        return activePitchClass.load(
            std::memory_order_acquire);
    }

    juce::String getImagePath(int index) const;

    void setImagePath(
        int index, const juce::File& file);

private:
    std::array<juce::String, 12> imagePaths;

    std::atomic<int> activePitchClass { -1 };
    std::atomic<int> activeMidiNote { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        MIDIImagePlayerAudioProcessor)
};
