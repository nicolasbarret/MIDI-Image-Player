#include "PluginEditor.h"

namespace
{
    constexpr const char* noteNames[12] = {
        "Do", "Do#", "Ré", "Ré#", "Mi", "Fa",
        "Fa#", "Sol", "Sol#", "La", "La#", "Si"
    };
}

MIDIImagePlayerAudioProcessorEditor::MIDIImagePlayerAudioProcessorEditor(
    MIDIImagePlayerAudioProcessor& processorToUse)
    : AudioProcessorEditor(&processorToUse),
      processor(processorToUse)
{
    setSize(760, 470);

    for (int i = 0; i < 12; ++i)
    {
        auto& label = noteLabels[static_cast<size_t>(i)];
        label.setText(noteNames[i], juce::dontSendNotification);
        label.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(label);

        auto& button = loadButtons[static_cast<size_t>(i)];
        button.setButtonText("Charger...");
        button.onClick = [this, i] { loadImage(i); };
        addAndMakeVisible(button);
    }

    preview.setOpaque(true);
    preview.setColour(juce::ImageComponent::backgroundColourId,
                      juce::Colour(0xff080808));
    addAndMakeVisible(preview);

    startTimerHz(30);
}

void MIDIImagePlayerAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(0xff101010));

    graphics.setColour(juce::Colours::white);
    graphics.setFont(18.0f);
    graphics.drawText("MIDI Image Player", 18, 10, 300, 28,
                      juce::Justification::left);

    graphics.setColour(juce::Colour(0xff454545));
    graphics.drawRect(preview.getBounds().toFloat(), 1.0f);
}

void MIDIImagePlayerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(18);
    area.removeFromTop(42);

    auto left = area.removeFromLeft(230);
    for (int i = 0; i < 12; ++i)
    {
        auto line = left.removeFromTop(30);
        noteLabels[static_cast<size_t>(i)].setBounds(line.removeFromLeft(62));
        loadButtons[static_cast<size_t>(i)].setBounds(
            line.removeFromLeft(145).reduced(2));
    }

    preview.setBounds(area.reduced(8));
}

void MIDIImagePlayerAudioProcessorEditor::loadImage(int pitchClass)
{
    if (fileChooser != nullptr)
        return;

    fileChooser = std::make_unique<juce::FileChooser>(
        "Choisir une image PNG ou JPEG",
        juce::File(),
        "*.png;*.jpg;*.jpeg");

    juce::Component::SafePointer<MIDIImagePlayerAudioProcessorEditor> safeThis(this);

    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, pitchClass](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            const auto file = chooser.getResult();
            if (file.existsAsFile())
            {
                safeThis->processor.setImagePath(pitchClass, file);
                if (safeThis->processor.getActivePitchClass() == pitchClass)
                    safeThis->refreshPreview();
            }

            safeThis->fileChooser.reset();
        });
}

void MIDIImagePlayerAudioProcessorEditor::refreshPreview()
{
    displayedPitchClass = processor.getActivePitchClass();

    if (displayedPitchClass < 0)
    {
        preview.setImage(juce::Image());
        return;
    }

    const auto path = processor.getImagePath(displayedPitchClass);
    if (path.isEmpty())
    {
        preview.setImage(juce::Image());
        return;
    }

    // Image decoding happens on the message thread, never in processBlock().
    const auto image = juce::ImageFileFormat::loadFrom(juce::File(path));
    preview.setImage(image,
        juce::RectanglePlacement::centred
            | juce::RectanglePlacement::onlyReduceInSize);
}

void MIDIImagePlayerAudioProcessorEditor::timerCallback()
{
    if (processor.getActivePitchClass() != displayedPitchClass)
        refreshPreview();
}
