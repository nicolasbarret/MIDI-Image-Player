#include <JuceHeader.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#if ! JUCE_WINDOWS
#error MIDI Audio Router currently supports Windows only.
#endif

namespace
{
struct StreamHeader
{
    uint32_t magic;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t format;
};

class RouterComponent final : public juce::Component,
                              private juce::AudioSource,
                              private juce::Thread,
                              private juce::Timer
{
public:
    RouterComponent()
        : juce::Thread("MIDI audio pipe reader"),
          fifo(65536),
          ring(2, 65536)
    {
        addAndMakeVisible(title);
        title.setText("MIDI Audio Router", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        addAndMakeVisible(status);
        status.setText("En attente de MIDI Video Player...", juce::dontSendNotification);
        addAndMakeVisible(deviceSelector);

        const auto error = deviceManager.initialise(0, 2, nullptr, true);
        if (error.isNotEmpty())
            status.setText("Audio : " + error, juce::dontSendNotification);

        audioPlayer.setSource(this);
        deviceManager.addAudioCallback(&audioPlayer);
        startThread();
        startTimerHz(2);
        setSize(560, 390);
    }

    ~RouterComponent() override
    {
        stopTimer();
        signalThreadShouldExit();
        const HANDLE pipe = activePipe.exchange(INVALID_HANDLE_VALUE);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(pipe, nullptr);
            CloseHandle(pipe);
        }
        waitForThreadToExit(2000);
        deviceManager.removeAudioCallback(&audioPlayer);
        audioPlayer.setSource(nullptr);
        deviceManager.closeAudioDevice();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff080b08));
        g.setColour(juce::Colour(0xff39ff14));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 8.0f, 1.2f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18);
        title.setBounds(area.removeFromTop(34));
        area.removeFromTop(6);
        status.setBounds(area.removeFromTop(38));
        area.removeFromTop(8);
        deviceSelector.setBounds(area);
    }

private:
    void prepareToPlay(int, double outputRate) override
    {
        currentOutputRate.store(outputRate);
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr || info.numSamples <= 0)
            return;

        info.clearActiveBufferRegion();
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead(info.numSamples, start1, size1, start2, size2);
        copyFromRing(*info.buffer, info.startSample, start1, size1);
        copyFromRing(*info.buffer, info.startSample + size1, start2, size2);
        fifo.finishedRead(size1 + size2);
    }

    void copyFromRing(juce::AudioBuffer<float>& output, int outputStart,
                      int ringStart, int count)
    {
        if (count <= 0)
            return;
        for (int ch = 0; ch < output.getNumChannels(); ++ch)
        {
            const int sourceChannel = juce::jmin(ch, 1);
            output.copyFrom(ch, outputStart, ring, sourceChannel, ringStart, count);
        }
    }

    bool readExact(HANDLE pipe, void* destination, DWORD bytes)
    {
        auto* out = static_cast<unsigned char*>(destination);
        DWORD received = 0;
        while (received < bytes && !threadShouldExit())
        {
            DWORD amount = 0;
            if (!ReadFile(pipe, out + received, bytes - received, &amount, nullptr) || amount == 0)
                return false;
            received += amount;
        }
        return received == bytes;
    }

    void run() override
    {
        constexpr wchar_t pipeName[] = L"\\\\.\\pipe\\MIDIVideoPlayerAudio";
        constexpr int packetFrames = 256;
        std::vector<float> interleaved(packetFrames * 2);

        while (!threadShouldExit())
        {
            HANDLE pipe = CreateFileW(pipeName, GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                Thread::sleep(400);
                continue;
            }

            activePipe.store(pipe);
            StreamHeader header {};
            if (!readExact(pipe, &header, sizeof(header))
                || header.magic != 0x4D565041u
                || header.channels != 2u
                || header.format != 1u
                || header.sampleRate < 8000u
                || header.sampleRate > 192000u)
            {
                closePipe(pipe);
                Thread::sleep(300);
                continue;
            }

            const double streamRate = (double) header.sampleRate;
            streamSampleRate.store(header.sampleRate);
            juce::Component::SafePointer<RouterComponent> safe(this);
            juce::MessageManager::callAsync([safe, streamRate]
            {
                if (safe != nullptr)
                {
                    safe->status.setText("Connecté | Flux " + juce::String((int) streamRate)
                                         + " Hz | Choisis VB-CABLE Input ci-dessous.",
                                         juce::dontSendNotification);
                    safe->matchSampleRate(streamRate);
                }
            });

            while (!threadShouldExit())
            {
                if (!readExact(pipe, interleaved.data(),
                               (DWORD)(interleaved.size() * sizeof(float))))
                    break;

                constexpr int packetFrames = 256;
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                fifo.prepareToWrite(packetFrames, start1, size1, start2, size2);
                copyToRing(interleaved, 0, start1, size1);
                copyToRing(interleaved, size1, start2, size2);
                fifo.finishedWrite(size1 + size2);
            }

            closePipe(pipe);
        }
    }

    void copyToRing(const std::vector<float>& input, int frameOffset,
                    int ringStart, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            ring.setSample(0, ringStart + i, input[(size_t)(frameOffset + i) * 2]);
            ring.setSample(1, ringStart + i, input[(size_t)(frameOffset + i) * 2 + 1]);
        }
    }

    void closePipe(HANDLE pipe)
    {
        HANDLE expected = pipe;
        if (activePipe.compare_exchange_strong(expected, INVALID_HANDLE_VALUE))
            CloseHandle(pipe);
    }

    void matchSampleRate(double wantedRate)
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr)
            return;

        const auto rates = device->getAvailableSampleRates();
        bool supported = false;
        for (double rate : rates)
            if (std::abs(rate - wantedRate) < 1.0)
                supported = true;

        if (!supported)
        {
            status.setText("Flux reçu à " + juce::String((int) wantedRate)
                           + " Hz. Cette sortie ne propose pas cette fréquence.",
                           juce::dontSendNotification);
            return;
        }

        auto setup = deviceManager.getAudioDeviceSetup();
        if (std::abs(setup.sampleRate - wantedRate) > 1.0)
        {
            setup.sampleRate = wantedRate;
            const auto error = deviceManager.setAudioDeviceSetup(setup, true);
            if (error.isNotEmpty())
                status.setText("Impossible de régler " + juce::String((int) wantedRate)
                               + " Hz : " + error, juce::dontSendNotification);
        }
    }

    void timerCallback() override
    {
        if (activePipe.load() == INVALID_HANDLE_VALUE)
        {
            streamSampleRate.store(0);
            status.setText("En attente de MIDI Video Player...", juce::dontSendNotification);
        }
        else if (streamSampleRate.load() > 0)
        {
            matchSampleRate((double) streamSampleRate.load());
        }
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer audioPlayer;
    juce::AudioDeviceSelectorComponent deviceSelector {
        deviceManager, 0, 0, 0, 2, false, false, true, false
    };
    juce::Label title, status;
    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> ring;
    std::atomic<HANDLE> activePipe { INVALID_HANDLE_VALUE };
    std::atomic<uint32_t> streamSampleRate { 0 };
    std::atomic<double> currentOutputRate { 44100.0 };
};

class RouterWindow final : public juce::DocumentWindow
{
public:
    RouterWindow()
        : juce::DocumentWindow("MIDI Audio Router", juce::Colour(0xff080b08), allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new RouterComponent(), true);
        setResizable(true, true);
        centreWithSize(580, 430);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class RouterApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MIDI Audio Router"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String&) override { window = std::make_unique<RouterWindow>(); }
    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<RouterWindow> window;
};
}

START_JUCE_APPLICATION(RouterApplication)
