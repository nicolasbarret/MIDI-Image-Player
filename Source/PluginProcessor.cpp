#include "PluginProcessor.h"
#include "PluginEditor.h"

MIDIImagePlayerAudioProcessor::MIDIImagePlayerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo(), true))
{
}

void MIDIImagePlayerAudioProcessor::prepareToPlay(double, int)
{
}

void MIDIImagePlayerAudioProcessor::releaseResources()
{
}

bool MIDIImagePlayerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo()
        || output == juce::AudioChannelSet::mono();
}

void MIDIImagePlayerAudioProcessor::processBlock(
    juce::AudioBuffer<float>& audio,
    juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // This is an intentionally silent instrument.
    audio.clear();

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        const int key = message.getChannel() * 128 + message.getNoteNumber();

        if (message.isNoteOn())
        {
            activeKey.store(key, std::memory_order_release);
        }
        else if (message.isNoteOff())
        {
            int expected = key;
            activeKey.compare_exchange_strong(
                expected, -1, std::memory_order_acq_rel);
        }
    }

    // MIDI is consumed here and is not sent onward.
    midi.clear();
}

int MIDIImagePlayerAudioProcessor::getActivePitchClass() const noexcept
{
    const int key = activeKey.load(std::memory_order_acquire);
    if (key < 0)
        return -1;

    const int noteNumber = key % 128;
    return noteNumber % 12;
}

juce::String MIDIImagePlayerAudioProcessor::getImagePath(int pitchClass) const
{
    if (pitchClass < 0 || pitchClass >= 12)
        return {};

    const juce::ScopedLock lock(imagePathsLock);
    return imagePaths[static_cast<size_t>(pitchClass)];
}

void MIDIImagePlayerAudioProcessor::setImagePath(
    int pitchClass, const juce::File& file)
{
    if (pitchClass < 0 || pitchClass >= 12)
        return;

    {
        const juce::ScopedLock lock(imagePathsLock);
        imagePaths[static_cast<size_t>(pitchClass)] = file.getFullPathName();
    }

    updateHostDisplay();
}

void MIDIImagePlayerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::XmlElement xml("MIDIImagePlayerState");
    {
        const juce::ScopedLock lock(imagePathsLock);
        for (int i = 0; i < 12; ++i)
        {
            auto* image = xml.createNewChildElement("Image");
            image->setAttribute("pitchClass", i);
            image->setAttribute("path", imagePaths[static_cast<size_t>(i)]);
        }
    }
    copyXmlToBinary(xml, destData);
}

void MIDIImagePlayerAudioProcessor::setStateInformation(
    const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName("MIDIImagePlayerState"))
        return;

    const juce::ScopedLock lock(imagePathsLock);
    forEachXmlChildElement(*xml, image)
    {
        if (!image->hasTagName("Image"))
            continue;

        const int pitchClass = image->getIntAttribute("pitchClass", -1);
        if (pitchClass >= 0 && pitchClass < 12)
            imagePaths[static_cast<size_t>(pitchClass)] =
                image->getStringAttribute("path");
    }
}

juce::AudioProcessorEditor* MIDIImagePlayerAudioProcessor::createEditor()
{
    return new MIDIImagePlayerAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MIDIImagePlayerAudioProcessor();
}
