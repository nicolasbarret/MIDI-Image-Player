#include "PluginEditor.h"

static const char* noteNames[12] =
{
    "Do", "Do#", "Ré", "Ré#", "Mi", "Fa",
    "Fa#", "Sol", "Sol#", "La", "La#", "Si"
};

MIDIImagePlayerAudioProcessorEditor::
MIDIImagePlayerAudioProcessorEditor(
    MIDIImagePlayerAudioProcessor& p)
    : AudioProcessorEditor(&p),
      processor(p)
{
    setSize(760, 470);

    for (int i = 0; i < 12; ++i)
    {
        noteLabels[(size_t)i].setText(
            noteNames[i],
            juce::dontSendNotification
        );

        noteLabels[(size_t)i].setColour(
            juce::Label::textColourId,
            juce::Colours::white
        );

        addAndMakeVisible(noteLabels[(size_t)i]);

        loadButtons[(size_t)i].setButtonText(
            "Charger..."
        );

        loadButtons[(size_t)i].onClick =
            [this, i]
            {
                loadImage(i);
            };

        addAndMakeVisible(loadButtons[(size_t)i]);
    }

    addAndMakeVisible(preview);

    startTimerHz(30);
}

void MIDIImagePlayerAudioProcessorEditor::paint(
    juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101010));

    g.setColour(juce::Colours::white);
    g.setFont(18.0f);

    g.drawText(
        "MIDI Image Player",
        18, 10, 300, 28,
        juce::Justification::left
    );

    g.setColour(juce::Colour(0xff454545));

    g.drawRect(
        preview.getBounds().toFloat(),
        1.0f
    );
}

void MIDIImagePlayerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(18);

    area.removeFromTop(42);

    auto left = area.removeFromLeft(230);

    for (int i = 0; i < 12; ++i)
    {
        auto line = left.removeFromTop(30);

        noteLabels[(size_t)i].setBounds(
            line.removeFromLeft(62)
        );

        loadButtons[(size_t)i].setBounds(
            line.removeFromLeft(145).reduced(2)
        );
    }

    preview.setBounds(area.reduced(8));
}

void MIDIImagePlayerAudioProcessorEditor::loadImage(
    int index)
{
    fileChooser = std::make_unique<juce::FileChooser>(
        "Choisir une image",
        juce::File(),
        "*.png;*.jpg;*.jpeg"
    );

    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this, index](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();

            if (file.existsAsFile())
            {
                processor.setImagePath(index, file);

                if (processor.getActivePitchClass() == index)
                    refreshPreview();
            }

            fileChooser.reset();
        }
    );
}

void MIDIImagePlayerAudioProcessorEditor::refreshPreview()
{
    const int pc = processor.getActivePitchClass();

    displayedPitchClass = pc;

    if (pc < 0)
    {
        currentImage = juce::Image();
        preview.setImage(juce::Image());
        return;
    }

    const auto path = processor.getImagePath(pc);

    if (path.isNotEmpty())
    {
        currentImage =
            juce::ImageFileFormat::loadFrom(
                juce::File(path)
            );

        preview.setImage(
            currentImage,
            juce::RectanglePlacement::centred
            | juce::RectanglePlacement::onlyReduceInSize
        );
    }
    else
    {
        currentImage = juce::Image();
        preview.setImage(juce::Image());
    }
}

void MIDIImagePlayerAudioProcessorEditor::timerCallback()
{
    const int pc = processor.getActivePitchClass();

    if (pc != displayedPitchClass)
        refreshPreview();
}
