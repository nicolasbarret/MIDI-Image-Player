
#include "PluginProcessor.h"
#include "PluginEditor.h"

MIDIImagePlayerAudioProcessor::MIDIImagePlayerAudioProcessor()
    : AudioProcessor(
        BusesProperties()
            .withOutput(
                "Output",
                juce::AudioChannelSet::stereo(),
                true))
{
}

void MIDIImagePlayerAudioProcessor::prepareToPlay(
    double, int)
{
}

void MIDIImagePlayerAudioProcessor::releaseResources()
{
}

bool MIDIImagePlayerAudioProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();

    return output == juce::AudioChannelSet::stereo()
        || output == juce::AudioChannelSet::mono();
}

void MIDIImagePlayerAudioProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Le plugin ne produit aucun son.
    buffer.clear();

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
        {
            const int note = message.getNoteNumber();

            // On mémorise la note MIDI exacte.
            activeMidiNote.store(
                note, std::memory_order_relaxed);

            // On ignore l'octave.
            activePitchClass.store(
                note % 12,
                std::memory_order_release);
        }
        else if (message.isNoteOff())
        {
            const int note = message.getNoteNumber();

            // Seule la note actuellement affichée
            // peut provoquer l'extinction de l'image.
            int expected = note;

            if (activeMidiNote.compare_exchange_strong(
                    expected,
                    -1,
                    std::memory_order_acq_rel))
            {
                activePitchClass.store(
                    -1,
                    std::memory_order_release);
            }
        }
    }

    // Le plugin ne transmet pas les événements MIDI.
    midi.clear();
}

juce::AudioProcessorEditor*
MIDIImagePlayerAudioProcessor::createEditor()
{
    return new MIDIImagePlayerAudioProcessorEditor(*this);
}

juce::String MIDIImagePlayerAudioProcessor::getImagePath(
    int index) const
{
    if (index < 0 || index >= 12)
        return {};

    return imagePaths[static_cast<size_t>(index)];
}

void MIDIImagePlayerAudioProcessor::setImagePath(
    int index,
    const juce::File& file)
{
    if (index < 0 || index >= 12)
        return;

    imagePaths[static_cast<size_t>(index)] =
        file.getFullPathName();

    updateHostDisplay();
}

void MIDIImagePlayerAudioProcessor::getStateInformation(
    juce::MemoryBlock& destData)
{
    juce::XmlElement xml("MIDIImagePlayerState");

    for (int i = 0; i < 12; ++i)
    {
        auto* child = xml.createNewChildElement("Image");

        child->setAttribute("pitchClass", i);

        child->setAttribute(
            "path",
            imagePaths[static_cast<size_t>(i)]);
    }

    copyXmlToBinary(xml, destData);
}

void MIDIImagePlayerAudioProcessor::setStateInformation(
    const void* data,
    int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(
        getXmlFromBinary(data, sizeInBytes));

    if (xml == nullptr
        || !xml->hasTagName("MIDIImagePlayerState"))
        return;

    forEachXmlChildElement(*xml, child)
    {
        if (child->hasTagName("Image"))
        {
            const int index =
                child->getIntAttribute("pitchClass", -1);

            if (index >= 0 && index < 12)
            {
                imagePaths[static_cast<size_t>(index)] =
                    child->getStringAttribute("path");
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MIDIImagePlayerAudioProcessor();
}
