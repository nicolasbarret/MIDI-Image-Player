#include <JuceHeader.h>
#include "LibVlcPlayer.h"

#include <array>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
class ReadableLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    static juce::Colour background() { return juce::Colour(0xff050705); }
    static juce::Colour panel() { return juce::Colour(0xff101610); }
    static juce::Colour neon() { return juce::Colour(0xff39ff14); }
    static juce::Colour white() { return juce::Colour(0xfff4fff2); }
    static juce::Font uiFont(float height, int style = juce::Font::plain) {
        return juce::Font(juce::FontOptions(juce::String("Segoe UI"), height, style));
    }

    ReadableLookAndFeel() {
        setColour(juce::ResizableWindow::backgroundColourId, background());
        setColour(juce::DocumentWindow::textColourId, white());
        setColour(juce::Label::textColourId, white());
        setColour(juce::TextButton::buttonColourId, panel());
        setColour(juce::TextButton::buttonOnColourId, neon().withAlpha(0.22f));
        setColour(juce::TextButton::textColourOffId, white());
        setColour(juce::TextButton::textColourOnId, neon());
        setColour(juce::ComboBox::backgroundColourId, panel());
        setColour(juce::ComboBox::textColourId, white());
        setColour(juce::ComboBox::outlineColourId, neon().withAlpha(0.8f));
        setColour(juce::ComboBox::arrowColourId, neon());
        setColour(juce::PopupMenu::backgroundColourId, background());
        setColour(juce::PopupMenu::textColourId, white());
        setColour(juce::PopupMenu::highlightedBackgroundColourId, neon());
        setColour(juce::PopupMenu::highlightedTextColourId, background());
        setColour(juce::TextEditor::backgroundColourId, panel());
        setColour(juce::TextEditor::textColourId, white());
        setColour(juce::TextEditor::highlightColourId, neon().withAlpha(0.35f));
        setColour(juce::TextEditor::outlineColourId, neon().withAlpha(0.65f));
        setColour(juce::TextEditor::focusedOutlineColourId, neon());
        setColour(juce::ScrollBar::thumbColourId, neon().withAlpha(0.8f));
    }

    juce::Font getTextButtonFont(juce::TextButton&, int) override {
        return ReadableLookAndFeel::uiFont(15.0f, juce::Font::bold);
    }
    juce::Font getComboBoxFont(juce::ComboBox&) override {
        return ReadableLookAndFeel::uiFont(14.0f);
    }
    juce::Font getLabelFont(juce::Label&) override {
        return ReadableLookAndFeel::uiFont(14.0f);
    }
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour&, bool highlighted, bool down) override {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.7f);
        const bool selected = button.getToggleState();
        auto fill = (down || selected) ? neon().withAlpha(0.24f)
                   : highlighted ? neon().withAlpha(0.10f) : panel();
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 5.0f);
        g.setColour(neon().withAlpha(highlighted || down || selected ? 1.0f : 0.72f));
        g.drawRoundedRectangle(bounds, 5.0f, 1.1f);
    }
};
constexpr int PitchCount=12, MaxOctaves=4, MaxNotes=48;
const std::array<const char*,12> notes{"Do","Do#","Ré","Ré#","Mi","Fa","Fa#","Sol","Sol#","La","La#","Si"};
static juce::String noteLabel(int i){return juce::String(notes[(size_t)(i%12)])+juce::String(3+i/12);}
struct VideoState {
    juce::CriticalSection lock;
    juce::File file;
    std::array<std::vector<double>,MaxNotes> markers;
    int octaveCount=1;
    double duration=0.0, playhead=0.0;
    std::deque<int> pendingTriggers;
    std::deque<int> pendingNoteOffs;
    bool fadeOnNoteOff = false;
    std::function<void()> dispatchPlayback;
};
static juce::String markerString(const std::vector<double>& v) {
    juce::StringArray a; for(double x:v) a.add(juce::String(x,6)); return a.joinIntoString(",");
}
static std::vector<double> parseMarkers(const juce::String& s) {
    std::vector<double> v; juce::StringArray a; a.addTokens(s,",",""); a.trim(); a.removeEmptyStrings();
    for(auto& x:a) { double d=x.getDoubleValue(); if(d>=0.0) v.push_back(d); }
    std::sort(v.begin(),v.end()); if(v.size()>1) v.resize(1); return v;
}

class MidiRouter final : private juce::MidiInputCallback {
public:
    explicit MidiRouter(std::shared_ptr<VideoState> s):state(std::move(s)){}
    ~MidiRouter() override { if(input) input->stop(); input.reset(); output.reset(); }
    juce::Array<juce::MidiDeviceInfo> inputs() const { return juce::MidiInput::getAvailableDevices(); }
    juce::Array<juce::MidiDeviceInfo> outputs() const { return juce::MidiOutput::getAvailableDevices(); }
    void openInput(const juce::String& id) { if(input) input->stop(); input.reset(); if(id.isNotEmpty()){input=juce::MidiInput::openDevice(id,this);if(input)input->start();} }
    void openOutput(const juce::String& id) { output.reset(); if(id.isNotEmpty())output=juce::MidiOutput::openDevice(id); }
private:
    void handleIncomingMidiMessage(juce::MidiInput*,const juce::MidiMessage& m) override {
        bool dispatch=false;
        std::function<void()> notify;
        {
            const juce::ScopedLock sl(state->lock);
            if(m.isNoteOff()) {
                const int clipIndex=m.getNoteNumber()-48;
                if(clipIndex>=0 && clipIndex<state->octaveCount*PitchCount) {
                    state->pendingNoteOffs.push_back(clipIndex);
                    while(state->pendingNoteOffs.size()>128) state->pendingNoteOffs.pop_front();
                    dispatch=true;
                }
            }
            if(m.isNoteOn()) {
                const int midiNote=m.getNoteNumber();
                const int clipIndex=midiNote-48; // MIDI Do3 is note 48
                if(clipIndex>=0 && clipIndex<state->octaveCount*PitchCount) {
                    auto& list=state->markers[(size_t)clipIndex];
                    if(!list.empty()) {
                        double chosen=list.front(), best=std::abs(chosen-state->playhead);
                        for(double t:list){double d=std::abs(t-state->playhead);if(d<best){best=d;chosen=t;}}
                        state->playhead=chosen;
                        state->pendingTriggers.push_back(clipIndex);
                        while(state->pendingTriggers.size()>128) state->pendingTriggers.pop_front();
                        dispatch=true;
                    }
                }
            }
            if(dispatch) notify=state->dispatchPlayback;
        }
        // Queue the visual trigger before forwarding MIDI to loopMIDI/FL Studio.
        // A slow downstream MIDI port must not delay the video request.
        if(notify) notify();
        if(output) output->sendMessageNow(m);
    }
    std::shared_ptr<VideoState> state;
    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
};

class MarkerTimeline final : public juce::Component {
public:
    explicit MarkerTimeline(std::shared_ptr<VideoState> s):state(std::move(s)){setMouseCursor(juce::MouseCursor::PointingHandCursor);}
    std::function<void(int)> changed;
    std::function<void(double)> seeked;
    void setDuration(double d){duration=juce::jmax(0.01,d);viewStart=juce::jlimit(0.0,juce::jmax(0.0,d-visibleDuration()),viewStart);repaint();}
    int requiredHeight() const { int n=1; { const juce::ScopedLock sl(state->lock); n=state->octaveCount*PitchCount; } return 25+n*22+60; }
    void refreshHeight(){setSize(getWidth(),requiredHeight());repaint();}
    void setWaveform(std::vector<float> peaks){waveform=std::move(peaks);repaint();}
    void setEraseMode(bool enabled){eraseMode=enabled;setMouseCursor(enabled?juce::MouseCursor::CrosshairCursor:juce::MouseCursor::PointingHandCursor);repaint();}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff050705));
        auto r=getLocalBounds().toFloat(); float left=78.0f, right=12.0f, top=25.0f;
        g.setColour(juce::Colour(0xffb8ffad)); g.setFont(ReadableLookAndFeel::uiFont(12.0f));
        g.drawText("Zoom x"+juce::String(zoom,1)+"   |   Molette : zoom   |   Maj + molette : déplacement",48,2,(int)r.getWidth()-60,19,juce::Justification::centredLeft);
        const float waveH=54.0f;
        const float trackArea=r.getHeight()-top-waveH-5.0f;
        int activeNotes=1;
        { const juce::ScopedLock sl(state->lock); activeNotes=state->octaveCount*PitchCount; }
        float trackH=trackArea/(float)juce::jmax(1,activeNotes);
        g.setFont(ReadableLookAndFeel::uiFont(13.0f,juce::Font::bold));
        for(int i=0;i<activeNotes;++i){
            float y=top+i*trackH;
            g.setColour(i%2?juce::Colour(0xff101810):juce::Colour(0xff0b100b));
            g.fillRect(left,y,r.getWidth()-left-right,trackH-1.0f);
            g.setColour(juce::Colour(0xffd8ffd1));
            g.drawText(noteLabel(i),4,(int)y,68,(int)trackH,juce::Justification::centredLeft);
            const juce::ScopedLock sl(state->lock);
            for(double t:state->markers[(size_t)i]){
                float x=timeToX(t,left,r.getWidth()-left-right);
                g.setColour(juce::Colour(0xff39ff14));
                juce::Path p;p.addTriangle(x,y+3,x-5,y+trackH-4,x+5,y+trackH-4);g.fillPath(p);
                g.drawLine(x,y+trackH-5,x,y+trackH-1,1.0f);
            }
        }
        const float waveY=top+trackArea+5.0f;
        g.setColour(juce::Colour(0xff090d09));
        g.fillRoundedRectangle(left,waveY,r.getWidth()-left-right,waveH-2.0f,4.0f);
        g.setColour(juce::Colour(0xff39ff14).withAlpha(0.92f));
        if(!waveform.empty()) {
            const float mid=waveY+(waveH-2.0f)*0.5f;
            const float half=(waveH-10.0f)*0.5f;
            const double first=juce::jlimit(0.0,1.0,viewStart/duration);
            const double last=juce::jlimit(0.0,1.0,(viewStart+visibleDuration())/duration);
            const size_t begin=(size_t)(first*waveform.size());
            const size_t finish=juce::jmin(waveform.size(),(size_t)std::ceil(last*waveform.size()));
            const float dx=(r.getWidth()-left-right)/(float)juce::jmax((size_t)1,finish-begin);
            for(size_t i=begin;i<finish;++i) {
                const float x=left+((float)(i-begin)+0.5f)*dx;
                const float h=juce::jlimit(1.0f,half,waveform[i]*half);
                g.drawVerticalLine((int)x,mid-h,mid+h);
            }
        } else {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawText("Forme d'onde audio en attente", (int)left,(int)waveY,(int)(r.getWidth()-left-right),(int)waveH,juce::Justification::centred);
        }
        g.setColour(juce::Colour(0xfff4fff2).withAlpha(0.85f));
        float px=timeToX(state->playhead,left,r.getWidth()-left-right);
        g.drawLine(px,top,px,waveY+waveH-2.0f,1.3f);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        auto r=getLocalBounds().toFloat(); float left=78.0f,right=12.0f,top=25.0f;
        const float waveH=54.0f;
        const float trackArea=r.getHeight()-top-waveH-5.0f;
        int activeNotes=1; { const juce::ScopedLock sl(state->lock); activeNotes=state->octaveCount*PitchCount; }
        float th=trackArea/(float)juce::jmax(1,activeNotes);
        float x=e.position.x; double clicked=xToTime(x,left,r.getWidth()-left-right);
        if(e.position.y>=top+trackArea) {
            clicked=juce::jlimit(0.0,duration,clicked);
            {const juce::ScopedLock sl(state->lock);state->playhead=clicked;}
            if(seeked)seeked(clicked);
            repaint();
            return;
        }
        int row=juce::jlimit(0,activeNotes-1,(int)((e.position.y-top)/th));
        dragRow=row; dragIndex=-1; double best=visibleDuration()*8.0/juce::jmax(1.0f,r.getWidth()-90.0f);
        {const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)row];
         for(size_t i=0;i<v.size();++i){double dx=std::abs(v[i]-clicked);if(dx<best){best=dx;dragIndex=(int)i;}}}
        if(eraseMode){
            bool erased=false;
            if(dragIndex>=0){
                {
                    const juce::ScopedLock sl(state->lock);
                    auto& v=state->markers[(size_t)row];
                    if(dragIndex<(int)v.size()){v.erase(v.begin()+dragIndex);erased=true;}
                }
            }
            if(erased && changed)changed(row);
            dragging=false;repaint();return;
        }
        if(dragIndex>=0){dragging=true;moveMarker(clicked);} else {dragging=false;{const juce::ScopedLock sl(state->lock);state->playhead=juce::jlimit(0.0,duration,clicked);}if(seeked)seeked(clicked);repaint();}
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if(!dragging)return;
        auto r=getLocalBounds().toFloat();double t=xToTime(e.position.x,78.0f,r.getWidth()-90.0f);moveMarker(t);
    }
    void mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& wheel) override {
        const float left=78.0f, right=12.0f;
        const double window=visibleDuration();
        if(e.mods.isShiftDown()) {
            viewStart=juce::jlimit(0.0,juce::jmax(0.0,duration-window),viewStart-wheel.deltaY*window*0.30);
        } else {
            const double anchor=xToTime(e.position.x,left,(float)getWidth()-left-right);
            const double ratio=juce::jlimit(0.0,1.0,(double(e.position.x)-double(left))/juce::jmax(1.0,double(getWidth())-double(left)-double(right)));
            zoom=juce::jlimit(1.0,64.0,zoom*(wheel.deltaY>0.0f?1.25:0.8));
            const double nextWindow=visibleDuration();
            viewStart=juce::jlimit(0.0,juce::jmax(0.0,duration-nextWindow),anchor-ratio*nextWindow);
        }
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent&) override { zoom=1.0;viewStart=0.0;repaint(); }
    void mouseUp(const juce::MouseEvent&) override {
        const bool markerWasDragged = dragging && dragRow >= 0 && dragIndex >= 0;
        if(markerWasDragged){
            const juce::ScopedLock sl(state->lock);
            auto& v=state->markers[(size_t)dragRow];
            std::sort(v.begin(),v.end());
        }
        dragging=false;dragIndex=-1;
        // Clicking the waveform to seek must never regenerate every MIDI clip.
        if(markerWasDragged && changed)changed(dragRow);
    }
private:
    double visibleDuration() const { return duration/zoom; }
    double xToTime(float x,float left,float width) const { return juce::jlimit(0.0,duration,viewStart+((double)(x-left)/juce::jmax(1.0f,width))*visibleDuration()); }
    float timeToX(double t,float left,float width) const { return left+(float)((t-viewStart)/visibleDuration())*width; }
    void moveMarker(double t){
        { const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)dragRow];
          if(dragIndex>=0&&dragIndex<(int)v.size()) v[(size_t)dragIndex]=juce::jlimit(0.0,duration,t); }
        if(seeked) seeked(juce::jlimit(0.0,duration,t));
        repaint();
    }
    std::shared_ptr<VideoState> state;double duration=60.0,zoom=1.0,viewStart=0.0;bool dragging=false,eraseMode=false;int dragRow=-1,dragIndex=-1;std::vector<float> waveform;
};

class WaveformJob final : public juce::ThreadPoolJob {
public:
    WaveformJob(juce::File f, std::function<void(std::vector<float>, juce::File)> cb)
        : ThreadPoolJob("Audio waveform and preview audio"), source(std::move(f)), callback(std::move(cb)) {}
    JobStatus runJob() override {
        std::vector<float> peaks(18000, 0.0f);
        auto app=juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
        auto ffmpeg=app.getChildFile("ffmpeg.exe");
        auto cache=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("MIDI Video Player").getChildFile("AudioCache");
        cache.createDirectory();
        const auto key=juce::String::toHexString(source.getFullPathName().hashCode64())+"_"
                     +juce::String(source.getSize())+"_"
                     +juce::String(source.getLastModificationTime().toMilliseconds());
        auto wav=cache.getChildFile(key+"_preview_44100_stereo.wav");
        if(ffmpeg.existsAsFile() && source.existsAsFile() && !wav.existsAsFile()) {
            auto partial=cache.getChildFile(key+"_preview.partial.wav");
            juce::StringArray args; args.add(ffmpeg.getFullPathName()); args.add("-hide_banner");
            args.add("-loglevel"); args.add("error"); args.add("-y"); args.add("-i"); args.add(source.getFullPathName());
            args.add("-vn"); args.add("-ac"); args.add("2"); args.add("-ar"); args.add("44100");
            args.add("-c:a"); args.add("pcm_s16le"); args.add(partial.getFullPathName());
            juce::ChildProcess process;
            if(process.start(args) && process.waitForProcessToFinish(120000) && partial.existsAsFile() && partial.getSize()>44)
                partial.moveFileTo(wav);
            else partial.deleteFile();
        }
        if(wav.existsAsFile()) {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(wav));
            if(reader && reader->lengthInSamples>0) {
                const auto total=reader->lengthInSamples;
                juce::AudioBuffer<float> buffer(juce::jmax(1,(int)reader->numChannels),8192);
                for(juce::int64 offset=0;offset<total && !shouldExit();offset+=8192) {
                    int count=(int)juce::jmin((juce::int64)8192,total-offset);
                    if(!reader->read(&buffer,0,count,offset,true,reader->numChannels>1)) break;
                    for(int i=0;i<count;++i) {
                        float sample=0.0f;
                        for(int ch=0;ch<buffer.getNumChannels();++ch) sample+=std::abs(buffer.getSample(ch,i));
                        sample/=buffer.getNumChannels();
                        int bin=juce::jmin(17999,(int)(((offset+i)*18000)/total));
                        peaks[(size_t)bin]=juce::jmax(peaks[(size_t)bin],sample);
                    }
                }
                float maximum=0.0f; for(float p:peaks) maximum=juce::jmax(maximum,p);
                if(maximum>0.0001f) for(auto& p:peaks) p/=maximum;
            }
        }
        if(callback && !shouldExit()) {
            auto cb=std::move(callback);
            juce::MessageManager::callAsync([cb=std::move(cb), data=std::move(peaks), audio=wav]() mutable {
                cb(std::move(data), audio.existsAsFile()?audio:juce::File{});
            });
        }
        return jobHasFinished;
    }
private:
    juce::File source; std::function<void(std::vector<float>, juce::File)> callback;
};

struct PreparedClip
{
    std::vector<juce::Image> frames;
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    double audioSampleRate = 44100.0;
    double sourceStart = 0.0;
};
using PreparedClipArray = std::array<PreparedClip, MaxNotes>;

class ClipAudioSource final : public juce::AudioSource {
public:
    void prepareToPlay(int, double rate) override { const juce::ScopedLock sl(lock); outputRate=juce::jmax(1.0,rate); position=0.0; }
    void releaseResources() override { const juce::ScopedLock sl(lock); clip.reset(); position=0.0; }
    void setClip(std::shared_ptr<juce::AudioBuffer<float>> audio,double rate) {
        const juce::ScopedLock sl(lock); clip=std::move(audio); sourceRate=juce::jmax(1.0,rate); position=0.0; paused=false;
    }
    void pause(){const juce::ScopedLock sl(lock);paused=true;}
    void resume(){const juce::ScopedLock sl(lock);paused=false;}
    void stop(){const juce::ScopedLock sl(lock);clip.reset();position=0.0;paused=false;}
    void setMuted(bool value){const juce::ScopedLock sl(lock);muted=value;}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override {
        info.clearActiveBufferRegion();
        const juce::ScopedLock sl(lock);
        if(paused || !clip || clip->getNumSamples()==0) return;
        const int outChannels=info.buffer->getNumChannels(), sourceChannels=clip->getNumChannels();
        for(int i=0;i<info.numSamples;++i) {
            const int index=(int)position;
            if(index>=clip->getNumSamples()) { clip.reset(); position=0.0; break; }
            const int next=juce::jmin(index+1,clip->getNumSamples()-1);
            const float frac=(float)(position-index);
            for(int ch=0;ch<outChannels;++ch) {
                const int src=juce::jmin(ch,sourceChannels-1);
                const float a=clip->getSample(src,index), b=clip->getSample(src,next);
                info.buffer->addSample(ch,info.startSample+i,(a+(b-a)*frac)*(muted?0.0f:1.0f));
            }
            position += sourceRate/outputRate;
        }
    }
private:
    juce::CriticalSection lock;
    std::shared_ptr<juce::AudioBuffer<float>> clip;
    double sourceRate=44100.0, outputRate=44100.0, position=0.0;
    bool paused=false;
    bool muted=false;
};

class TimelineAudioSource final : public juce::AudioSource
{
public:
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        const juce::ScopedLock sl(lock);
        outputRate = juce::jmax(1.0, sampleRate);
        scratch.setSize(2, juce::jmax(512, samplesPerBlockExpected * 2), false, false, true);
    }

    void releaseResources() override
    {
        const juce::ScopedLock sl(lock);
        playing = false;
        position = 0.0;
    }

    bool setFile(const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> newReader(formats.createReaderFor(file));
        if (!newReader || newReader->lengthInSamples <= 0)
            return false;

        const juce::ScopedLock sl(lock);
        playing = false;
        reader = std::move(newReader);
        position = 0.0;
        sourceRate = juce::jmax(1.0, reader->sampleRate);
        return true;
    }

    void seekAndPlay(double seconds)
    {
        const juce::ScopedLock sl(lock);
        if (!reader)
            return;
        position = juce::jlimit(0.0, (double) reader->lengthInSamples - 1.0,
                                juce::jmax(0.0, seconds) * sourceRate);
        playing = true;
    }

    void stop()
    {
        const juce::ScopedLock sl(lock);
        playing = false;
    }

    void setMuted(bool shouldMute)
    {
        const juce::ScopedLock sl(lock);
        muted = shouldMute;
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const juce::ScopedLock sl(lock);
        if (!playing || !reader || info.numSamples <= 0)
            return;

        const double ratio = sourceRate / outputRate;
        const int needed = juce::jmin(scratch.getNumSamples(),
            juce::jmax(2, (int) std::ceil(info.numSamples * ratio) + 2));
        const auto start = (juce::int64) position;
        if (start >= reader->lengthInSamples)
        {
            playing = false;
            return;
        }
        const int available = (int) juce::jmin((juce::int64) needed, reader->lengthInSamples - start);
        scratch.clear();
        if (!reader->read(&scratch, 0, available, start, true, true))
        {
            playing = false;
            return;
        }

        const int sourceChannels = juce::jmax(1, (int) reader->numChannels);
        for (int i = 0; i < info.numSamples; ++i)
        {
            const double sourcePosition = position - start + i * ratio;
            const int index = (int) sourcePosition;
            if (index >= available)
            {
                playing = false;
                break;
            }
            const int next = juce::jmin(index + 1, available - 1);
            const float fraction = (float) (sourcePosition - index);
            for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
            {
                const int sourceChannel = juce::jmin(channel, sourceChannels - 1);
                const float a = scratch.getSample(sourceChannel, index);
                const float b = scratch.getSample(sourceChannel, next);
                info.buffer->addSample(channel, info.startSample + i, (a + (b - a) * fraction) * (muted ? 0.0f : 1.0f));
            }
        }
        position += info.numSamples * ratio;
        if (position >= reader->lengthInSamples)
            playing = false;
    }

private:
    juce::CriticalSection lock;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> scratch;
    double position = 0.0, sourceRate = 44100.0, outputRate = 44100.0;
    bool playing = false;
    bool muted = false;
};

class ClipFramePlayer final : public juce::Component, private juce::Timer
{
public:
    ClipFramePlayer()
    {
        setOpaque(true);
        startTimerHz(FrameRate);
    }

    void setClips(PreparedClipArray incoming, double secondsPerClip)
    {
        const juce::ScopedLock sl(lock);
        clips = std::move(incoming);
        clipDuration = juce::jlimit(1.0, 8.0, secondsPerClip);
        activeClip = -1;
        playing = false;
        paused = false;
        currentImage = juce::Image();
        displayedFrame = -1;
        repaint();
    }

    void setClip(int index, PreparedClip clip, double secondsPerClip)
    {
        if (index < 0 || index >= (int) clips.size())
            return;
        const juce::ScopedLock sl(lock);
        clips[(size_t) index] = std::move(clip);
        clipDuration = juce::jlimit(1.0, 8.0, secondsPerClip);
        if (activeClip == index)
        {
            activeClip = -1;
            playing = false;
            paused = false;
            currentImage = juce::Image();
            displayedFrame = -1;
        }
        repaint();
    }

    void clearClip(int index)
    {
        if (index < 0 || index >= (int) clips.size())
            return;
        const juce::ScopedLock sl(lock);
        clips[(size_t) index] = PreparedClip{};
        if (activeClip == index)
        {
            activeClip = -1;
            playing = false;
            paused = false;
            currentImage = juce::Image();
            displayedFrame = -1;
        }
        repaint();
    }

    std::shared_ptr<juce::AudioBuffer<float>> audioForClip(int index) const {
        const juce::ScopedLock sl(lock);
        return index>=0 && index<(int)clips.size() ? clips[(size_t)index].audio : nullptr;
    }
    double audioRateForClip(int index) const {
        const juce::ScopedLock sl(lock);
        return index>=0 && index<(int)clips.size() ? clips[(size_t)index].audioSampleRate : 44100.0;
    }

    bool trigger(int clipIndex)
    {
        {
            const juce::ScopedLock sl(lock);
            if (clipIndex < 0 || clipIndex >= (int) clips.size())
                return false;

            auto& clip = clips[(size_t) clipIndex];
            if (clip.frames.empty() || !clip.frames.front().isValid())
                return false;

            activeClip = clipIndex;
            fading = false;
            playing = true;
            paused = false;
            startTimeMs = juce::Time::getMillisecondCounter();
            pausedElapsedMs = 0;
            displayedFrame = 0;
            currentImage = clip.frames.front();
        }
        repaint();
        return true;
    }

    void releaseNote(int clipIndex, bool fade)
    {
        const juce::ScopedLock sl(lock);
        if (activeClip != clipIndex) return;
        if (fade) { fading = true; fadeStartMs = juce::Time::getMillisecondCounter(); playing = false; paused = false; }
        else { activeClip = -1; playing = false; paused = false; currentImage = juce::Image(); displayedFrame = -1; }
        repaint();
    }

    void stop()
    {
        const juce::ScopedLock sl(lock);
        activeClip = -1;
        playing = false;
        paused = false;
        currentImage = juce::Image();
        displayedFrame = -1;
        repaint();
    }

    void pause()
    {
        const juce::ScopedLock sl(lock);
        if (playing && !paused)
        {
            pausedElapsedMs = juce::Time::getMillisecondCounter() - startTimeMs;
            paused = true;
        }
    }

    bool isPlaying() const
    {
        const juce::ScopedLock sl(lock);
        return playing && !paused;
    }

    double position() const
    {
        const juce::ScopedLock sl(lock);
        if (activeClip < 0)
            return 0.0;
        const auto elapsed = paused ? pausedElapsedMs
                                    : (juce::Time::getMillisecondCounter() - startTimeMs);
        return clips[(size_t) activeClip].sourceStart
             + juce::jmin(clipDuration, (double) elapsed / 1000.0);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);
        juce::Image image;
        {
            const juce::ScopedLock sl(lock);
            image = currentImage;
        }
        if (image.isValid())
            g.drawImageWithin(image, 0, 0, getWidth(), getHeight(),
                              juce::RectanglePlacement::centred);
        float alpha = 0.0f;
        { const juce::ScopedLock sl(lock); if (fading) alpha = juce::jlimit(0.0f, 1.0f, (float)(juce::Time::getMillisecondCounter()-fadeStartMs)/300.0f); }
        if (alpha > 0.0f) { g.setColour(juce::Colours::black.withAlpha(alpha)); g.fillAll(); }
    }

private:
    static constexpr int FrameRate = 24;

    void timerCallback() override
    {
        bool changed = false;
        {
            const juce::ScopedLock sl(lock);
            if (fading) { if (juce::Time::getMillisecondCounter()-fadeStartMs >= 300) { fading=false; activeClip=-1; currentImage=juce::Image(); displayedFrame=-1; changed=true; } else changed=true; }
            if (activeClip < 0 || paused || !playing)
                return;

            const auto& clip = clips[(size_t) activeClip];
            if (clip.frames.empty())
                return;

            const auto elapsedMs = juce::Time::getMillisecondCounter() - startTimeMs;
            const int wanted = juce::jlimit(0, (int) clip.frames.size() - 1,
                (int) ((double) elapsedMs * FrameRate / 1000.0));

            if (wanted < (int) clip.frames.size() && wanted != displayedFrame)
            {
                const auto& next = clip.frames[(size_t) wanted];
                if (next.isValid())
                {
                    currentImage = next;
                    displayedFrame = wanted;
                    changed = true;
                }
            }

            if (elapsedMs >= (std::uint32_t) (clipDuration * 1000.0))
                playing = false;
        }
        if (changed)
            repaint();
    }

    mutable juce::CriticalSection lock;
    PreparedClipArray clips;
    juce::Image currentImage;
    int activeClip = -1;
    int displayedFrame = -1;
    bool playing = false;
    bool paused = false;
    bool fading = false;
    std::uint32_t fadeStartMs = 0;
    double clipDuration = 3.0;
    std::uint32_t startTimeMs = 0;
    std::uint32_t pausedElapsedMs = 0;
};

struct ClipPreloadFeedback
{
    juce::CriticalSection lock;
    double progress = 0.0;
    bool running = false;
    int noteIndex = -1;
    juce::String message = "Sélectionne une note et clique sur Précharger.";
};

class ClipPreloadJob final : public juce::ThreadPoolJob
{
public:
    ClipPreloadJob(juce::File sourceFile,
                   int note,
                   double startSeconds,
                   double secondsPerClip,
                   std::shared_ptr<ClipPreloadFeedback> feedbackState,
                   std::function<void(int, PreparedClip, juce::String, bool)> finished)
        : ThreadPoolJob("Preload selected MIDI clip"),
          source(std::move(sourceFile)),
          noteIndex(juce::jlimit(0, MaxNotes-1, note)),
          markerStart(juce::jmax(0.0, startSeconds)),
          clipDuration(juce::jlimit(1.0, 8.0, secondsPerClip)),
          feedback(std::move(feedbackState)),
          callback(std::move(finished))
    {
    }

    JobStatus runJob() override
    {
        PreparedClip prepared;
        juce::String message;
        bool success = false;
        const auto noteName = noteLabel(noteIndex);
        const int expectedFrames = juce::jmax(1, (int) std::round(clipDuration * FramesPerSecond));

        auto setFeedback = [this](double value, const juce::String& text)
        {
            if (!feedback)
                return;
            const juce::ScopedLock sl(feedback->lock);
            feedback->progress = juce::jlimit(0.0, 1.0, value);
            feedback->message = text;
        };

        auto appFolder = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                             .getParentDirectory();
        auto ffmpeg = appFolder.getChildFile("ffmpeg.exe");

        if (!source.existsAsFile())
            message = "Impossible de précharger : vidéo source introuvable.";
        else if (!ffmpeg.existsAsFile())
            message = "Impossible de précharger : ffmpeg.exe absent du dossier du logiciel.";
        else
        {
            auto cacheRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("MIDI Video Player")
                                 .getChildFile("ClipCache");
            cacheRoot.createDirectory();

            juce::String cacheKey = "manual_v5_24fps_854x480_audio_"
                                  + juce::String::toHexString(source.getFullPathName().hashCode64())
                                  + "_" + juce::String::toHexString(source.getSize())
                                  + "_" + juce::String(source.getLastModificationTime().toMilliseconds())
                                  + "_note" + juce::String(noteIndex)
                                  + "_start" + juce::String((juce::int64) std::llround(markerStart * 1000.0))
                                  + "_dur" + juce::String((int) std::llround(clipDuration * 1000.0));

            // The JPEG cache remains on disk; decoded images are also loaded into RAM.
            // Every marker position has a distinct cache folder.
            auto noteFolder = cacheRoot.getChildFile(cacheKey);
            auto completeFlag = noteFolder.getChildFile("complete.txt");
            juce::Array<juce::File> found;
            noteFolder.findChildFiles(found, juce::File::findFiles, false, "*.jpg");

            if (!completeFlag.existsAsFile() || found.isEmpty())
            {
                noteFolder.deleteRecursively();
                noteFolder.createDirectory();

                setFeedback(0.01, "Extraction " + noteName + " : préparation du clip...");
                auto outputPattern = noteFolder.getChildFile("frame_%05d.jpg");
                juce::StringArray args;
                args.add(ffmpeg.getFullPathName());
                args.add("-hide_banner");
                args.add("-loglevel"); args.add("error");
                args.add("-threads"); args.add("1");
                args.add("-y");
                args.add("-ss"); args.add(juce::String(markerStart, 3));
                args.add("-i"); args.add(source.getFullPathName());
                args.add("-t"); args.add(juce::String(clipDuration, 3));
                args.add("-an");
                args.add("-vf");
                args.add("fps=24,scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:(ow-iw)/2:(oh-ih)/2,setsar=1");
                args.add("-q:v"); args.add("5");
                args.add("-start_number"); args.add("0");
                args.add(outputPattern.getFullPathName());

                juce::ChildProcess process;
                if (!process.start(args))
                {
                    message = "FFmpeg n'a pas pu démarrer pour " + noteName + ".";
                }
                else
                {
                    juce::uint32 lastScan = 0;
                    while (!process.waitForProcessToFinish(120) && !shouldExit())
                    {
                        const auto now = juce::Time::getMillisecondCounter();
                        if (now - lastScan >= 180)
                        {
                            lastScan = now;
                            juce::Array<juce::File> current;
                            noteFolder.findChildFiles(current, juce::File::findFiles, false, "*.jpg");
                            const int count = current.size();
                            const double fraction = juce::jlimit(0.0, 1.0,
                                (double) count / (double) expectedFrames);
                            setFeedback(0.02 + 0.58 * fraction,
                                "Extraction " + noteName + " : " + juce::String(count)
                                + " / " + juce::String(expectedFrames) + " images");
                        }
                    }

                    if (shouldExit())
                    {
                        process.kill();
                        process.waitForProcessToFinish(1000);
                        return jobHasFinished;
                    }
                }
            }

            if (message.isEmpty())
            {
                found.clear();
                noteFolder.findChildFiles(found, juce::File::findFiles, false, "*.jpg");
                std::vector<juce::File> frameFiles;
                for (const auto& file : found)
                    frameFiles.push_back(file);
                std::sort(frameFiles.begin(), frameFiles.end(),
                          [](const juce::File& a, const juce::File& b)
                          {
                              return a.getFileName() < b.getFileName();
                          });

                const int total = (int) frameFiles.size();
                if (total == 0)
                {
                    message = "Aucune image extraite pour " + noteName
                            + ". Vérifie le format de la vidéo et le marqueur.";
                }
                else
                {
                    // FFmpeg has finished writing this marker's JPEG cache.
                    // Mark it complete before RAM decoding so a cancelled RAM pass
                    // can resume from disk without extracting the video again.
                    completeFlag.replaceWithText("ready");
                    setFeedback(0.62, "Chargement en RAM de " + noteName + " : 0 / "
                                      + juce::String(total) + " images");
                    for (int i = 0; i < total; ++i)
                    {
                        if (shouldExit())
                            return jobHasFinished;

                        auto image = juce::ImageFileFormat::loadFrom(frameFiles[(size_t) i]);
                        if (image.isValid())
                            prepared.frames.push_back(std::move(image));

                        const double fraction = (double) (i + 1) / (double) total;
                        setFeedback(0.62 + 0.37 * fraction,
                            "Chargement en RAM de " + noteName + " : "
                            + juce::String(i + 1) + " / " + juce::String(total) + " images");
                    }

                    if (!prepared.frames.empty())
                    {
                        prepared.sourceStart = markerStart;
                        // Extract the matching source audio to a compact PCM cache,
                        // then decode it into RAM before the MIDI clip is made ready.
                        auto audioFile = noteFolder.getChildFile("audio_44100_stereo.wav");
                        if (!audioFile.existsAsFile()) {
                            auto partialAudioFile = noteFolder.getChildFile("audio_44100_stereo.partial.wav");
                            juce::StringArray audioArgs;
                            audioArgs.add(ffmpeg.getFullPathName()); audioArgs.add("-hide_banner");
                            audioArgs.add("-loglevel"); audioArgs.add("error"); audioArgs.add("-y");
                            audioArgs.add("-ss"); audioArgs.add(juce::String(markerStart,3));
                            audioArgs.add("-i"); audioArgs.add(source.getFullPathName());
                            audioArgs.add("-t"); audioArgs.add(juce::String(clipDuration,3));
                            audioArgs.add("-vn"); audioArgs.add("-map"); audioArgs.add("0:a:0?");
                            audioArgs.add("-ac"); audioArgs.add("2"); audioArgs.add("-ar"); audioArgs.add("44100");
                            audioArgs.add("-c:a"); audioArgs.add("pcm_s16le"); audioArgs.add(partialAudioFile.getFullPathName());
                            juce::ChildProcess audioProcess;
                            if(audioProcess.start(audioArgs)) {
                                bool finishedAudio=false;
                                while(!finishedAudio && !shouldExit())
                                    finishedAudio=audioProcess.waitForProcessToFinish(120);
                                if(shouldExit()) { audioProcess.kill(); audioProcess.waitForProcessToFinish(1000); return jobHasFinished; }
                                if(finishedAudio && partialAudioFile.existsAsFile() && partialAudioFile.getSize()>44)
                                    partialAudioFile.moveFileTo(audioFile);
                            }
                        }
                        if(audioFile.existsAsFile() && audioFile.getSize()>44) {
                            juce::AudioFormatManager audioFormats; audioFormats.registerBasicFormats();
                            std::unique_ptr<juce::AudioFormatReader> audioReader(audioFormats.createReaderFor(audioFile));
                            if(audioReader && audioReader->lengthInSamples>0) {
                                const int channels=juce::jlimit(1,2,(int)audioReader->numChannels);
                                const int samples=(int)juce::jmin((juce::int64)std::numeric_limits<int>::max(),audioReader->lengthInSamples);
                                prepared.audio=std::make_shared<juce::AudioBuffer<float>>(channels,samples);
                                if(audioReader->read(prepared.audio.get(),0,samples,0,true,channels>1)) {
                                    prepared.audioSampleRate=audioReader->sampleRate;
                                } else prepared.audio.reset();
                            }
                        }
                        completeFlag.replaceWithText("ready");
                        success = true;
                        message = noteName + " : " + juce::String(markerStart, 3)
                                + " s → " + juce::String(markerStart + clipDuration, 3)
                                + " s préchargées. " + juce::String((int) prepared.frames.size())
                                + " images et " + (prepared.audio ? "audio" : "aucun son")
                                + " en RAM.";
                    }
                    else
                    {
                        message = "Les images du clip " + noteName + " n'ont pas pu être décodées.";
                    }
                }
            }
        }

        if (feedback)
        {
            const juce::ScopedLock sl(feedback->lock);
            feedback->running = false;
            feedback->progress = success ? 1.0 : 0.0;
            feedback->message = message;
        }

        if (callback && !shouldExit())
        {
            auto finished = std::move(callback);
            juce::MessageManager::callAsync(
                [finished = std::move(finished),
                 note = noteIndex,
                 clip = std::move(prepared),
                 result = message,
                 ok = success]() mutable
                {
                    finished(note, std::move(clip), result, ok);
                });
        }
        return jobHasFinished;
    }

private:
    static constexpr int FramesPerSecond = 24;
    juce::File source;
    int noteIndex = 0;
    double markerStart = 0.0;
    double clipDuration = 2.0;
    std::shared_ptr<ClipPreloadFeedback> feedback;
    std::function<void(int, PreparedClip, juce::String, bool)> callback;
};


#if JUCE_WINDOWS
class AudioBridgeSource final : public juce::AudioSource, private juce::Thread
{
public:
    explicit AudioBridgeSource(juce::AudioSource& source)
        : juce::Thread("MIDI Video audio bridge"), upstream(source), fifo(32768),
          queue(2, 32768)
    {
        startThread();
    }

    ~AudioBridgeSource() override { shutdown(); }

    void shutdown()
    {
        signalThreadShouldExit();
        waitForThreadToExit(1500);
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        currentRate.store((uint32_t) juce::jlimit(8000.0, 192000.0, sampleRate));
        upstream.prepareToPlay(samplesPerBlockExpected, sampleRate);
    }

    void releaseResources() override { upstream.releaseResources(); }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        upstream.getNextAudioBlock(info);
        if (info.buffer == nullptr || info.numSamples <= 0)
            return;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite(info.numSamples, start1, size1, start2, size2);
        copyToQueue(*info.buffer, info.startSample, start1, size1);
        copyToQueue(*info.buffer, info.startSample + size1, start2, size2);
        fifo.finishedWrite(size1 + size2);
    }

private:
    void copyToQueue(const juce::AudioBuffer<float>& input, int inputStart,
                     int queueStart, int count)
    {
        if (count <= 0) return;
        for (int ch = 0; ch < 2; ++ch)
        {
            const int sourceChannel = juce::jmin(ch, input.getNumChannels() - 1);
            queue.copyFrom(ch, queueStart, input, sourceChannel, inputStart, count);
        }
    }

    void run() override
    {
        constexpr wchar_t pipeName[] = L"\\\\.\\pipe\\MIDIVideoPlayerAudio";
        std::vector<float> interleaved(2048 * 2);
        HANDLE pipe = INVALID_HANDLE_VALUE;
        bool connected = false;
        bool headerSent = false;

        while (!threadShouldExit())
        {
            if (pipe == INVALID_HANDLE_VALUE)
            {
                pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_OUTBOUND,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                    1, 65536, 65536, 0, nullptr);
                if (pipe == INVALID_HANDLE_VALUE) { Thread::sleep(500); continue; }
            }

            if (!connected)
            {
                if (ConnectNamedPipe(pipe, nullptr))
                    connected = true;
                else if (GetLastError() == ERROR_PIPE_CONNECTED)
                    connected = true;
                else { Thread::sleep(20); continue; }
            }

            if (connected && !headerSent)
            {
                struct StreamHeader { uint32_t magic, sampleRate, channels, format; };
                const StreamHeader header { 0x4D565041u, currentRate.load(), 2u, 1u };
                DWORD written = 0;
                if (!WriteFile(pipe, &header, sizeof(header), &written, nullptr)
                    || written != sizeof(header))
                {
                    DisconnectNamedPipe(pipe); connected = false; headerSent = false;
                    continue;
                }
                headerSent = true;
            }

            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead(2048, start1, size1, start2, size2);
            const int total = size1 + size2;
            if (total == 0) { Thread::sleep(2); continue; }

            int out = 0;
            for (int i = 0; i < size1; ++i, ++out) {
                interleaved[(size_t)out * 2] = queue.getSample(0, start1 + i);
                interleaved[(size_t)out * 2 + 1] = queue.getSample(1, start1 + i);
            }
            for (int i = 0; i < size2; ++i, ++out) {
                interleaved[(size_t)out * 2] = queue.getSample(0, start2 + i);
                interleaved[(size_t)out * 2 + 1] = queue.getSample(1, start2 + i);
            }

            DWORD written = 0;
            const DWORD bytes = (DWORD)(total * 2 * (int)sizeof(float));
            const BOOL ok = WriteFile(pipe, interleaved.data(), bytes, &written, nullptr);
            fifo.finishedRead(total);
            if (!ok || written != bytes)
            {
                DisconnectNamedPipe(pipe);
                connected = false;
                headerSent = false;
            }
        }

        if (pipe != INVALID_HANDLE_VALUE) {
            if (connected) DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    juce::AudioSource& upstream;
    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> queue;
    std::atomic<uint32_t> currentRate { 44100 };
};
#endif

class VideoOutputView final : public juce::Component,
                                  private juce::Timer,
                                  private juce::AsyncUpdater
{
public:
    explicit VideoOutputView(std::shared_ptr<VideoState> s)
        : state(std::move(s))
    {
        addAndMakeVisible(sourceVideo);
        addAndMakeVisible(clipPlayer);
        audioDeviceManager.initialise(0,2,nullptr,true);

        audioMixer.addInputSource(&audioClipSource, false);
         audioMixer.addInputSource(&timelineAudioSource, false);
         audioSourcePlayer.setSource(&bridgeSource);
        audioDeviceManager.addAudioCallback(&audioSourcePlayer);
        clipPlayer.setVisible(false);

        juce::Component::SafePointer<VideoOutputView> safe(this);
        {
            const juce::ScopedLock sl(state->lock);
            state->dispatchPlayback = [safe]()
            {
                if (safe)
                    safe->triggerAsyncUpdate();
            };
        }
        startTimerHz(30);
    }

    ~VideoOutputView() override
    {
        cancelPendingUpdate();
        sourceVideo.stop();
        sourceVideo.unload();
         audioDeviceManager.removeAudioCallback(&audioSourcePlayer);
        audioSourcePlayer.setSource(nullptr);
#if JUCE_WINDOWS
        bridgeSource.shutdown();
#endif
        audioMixer.removeAllInputs();
         timelineAudioSource.releaseResources();

        audioDeviceManager.closeAudioDevice();
        const juce::ScopedLock sl(state->lock);
        state->dispatchPlayback = {};
    }

    void resized() override
    {
        sourceVideo.setBounds(getLocalBounds());
        clipPlayer.setBounds(getLocalBounds());
    }

    juce::String play()
    {
        syncFile();
        if (!loadedFile.existsAsFile())
            return "Aucune vidéo chargée.";
        if (!sourceVideo.isAvailable())
            return sourceVideo.errorMessage();

        clipPlayer.stop();
        audioClipSource.stop();
         clipPlayer.setVisible(false);
        sourceVideo.setVisible(true);
        sourceVideo.setOutputVisible(true);
        audioClipSource.stop();

        sourceVideo.play();
        clipMode = false;
        return sourceVideo.playbackDiagnostic();
    }

    void pause()
    {
        if (clipMode) {
            clipPlayer.pause();
            audioClipSource.pause();
        } else { sourceVideo.pause();  }
    }

    void stop()
    {
        sourceVideo.stop();
        clipPlayer.stop();
        audioClipSource.stop();
    }

    bool isPlaying() const
    {
        return clipMode ? clipPlayer.isPlaying() : sourceVideo.isPlaying();
    }

    void seek(double seconds)
    {
        syncFile();
        clipPlayer.stop();
        audioClipSource.stop();
        clipPlayer.setVisible(false);
        sourceVideo.setVisible(true);
        sourceVideo.setOutputVisible(true);
        clipMode = false;
        sourceVideo.setPlayPosition(juce::jmax(0.0, seconds));

    }

    double position() const
    {
        return clipMode ? clipPlayer.position() : sourceVideo.getPlayPosition();
    }

    double duration() const
    {
        return sourceVideo.getVideoDuration();
    }

    juce::String diagnostic() const
    {
        if (clipMode)
            return clipPlayer.isPlaying() ? "Clip MIDI en lecture (images en mémoire)."
                                          : "Clip MIDI prêt.";
        return sourceVideo.playbackDiagnostic();
    }

     void setPreparedClip(int noteIndex, PreparedClip clip, double clipSeconds)
    {
        clipPlayer.setClip(noteIndex, std::move(clip), clipSeconds);
    }

    void clearPreparedClip(int noteIndex)
    {
        clipPlayer.clearClip(noteIndex);
    }

    bool triggerClip(int pitchClass)
    {
        syncFile();

        // Do not hide the working VLC source view unless the requested MIDI clip
        // is actually prepared and can start. This also makes missing clips harmless.
        if (!clipPlayer.trigger(pitchClass))
            return false;
        audioClipSource.setClip(clipPlayer.audioForClip(pitchClass),clipPlayer.audioRateForClip(pitchClass));

        sourceVideo.pause();
        sourceVideo.setOutputVisible(false);
        sourceVideo.setVisible(false);
        clipPlayer.setVisible(true);
        clipMode = true;
        return true;
    }

    void clearPreparedClips()
    {
        clipPlayer.setClips({}, 3.0);
        audioClipSource.stop();
    }
    bool setTimelineAudioFile(const juce::File& file) { return timelineAudioSource.setFile(file); }
    void previewTimelineAudio(double seconds) { timelineAudioSource.seekAndPlay(seconds); }
    void stopTimelineAudio() { timelineAudioSource.stop(); }
    void setOutputMuted(bool muted)
    {

        audioClipSource.setMuted(muted);
        timelineAudioSource.setMuted(muted);
    }    void setPreviewDuration(double seconds) { clipPreviewDuration = juce::jlimit(1.0, 8.0, seconds); }

    juce::StringArray availableAudioOutputs() const {
        juce::StringArray names;
        if (auto* type = audioDeviceManager.getCurrentDeviceTypeObject())
            names = type->getDeviceNames(false);
        return names;
    }

    juce::String currentAudioOutput() const {
        if (auto* device = audioDeviceManager.getCurrentAudioDevice())
            return device->getName();
        return {};
    }

    juce::String selectAudioOutput(const juce::String& name) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        audioDeviceManager.getAudioDeviceSetup(setup);
        setup.outputDeviceName = name;
        return audioDeviceManager.setAudioDeviceSetup(setup, true);
    }

    void configureAudio() {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle="Sortie audio vers FL Studio";
        options.dialogBackgroundColour=juce::Colour(0xff050705);
        options.escapeKeyTriggersCloseButton=true;
        options.useNativeTitleBar=true;
        options.resizable=false;
        options.content.setOwned(new juce::AudioDeviceSelectorComponent(audioDeviceManager,0,0,1,2,false,false,true,true));
        options.content->setSize(520,420);
        options.launchAsync();
    }

private:
    void syncFile()
    {
        juce::File file;
        {
            const juce::ScopedLock sl(state->lock);
            file = state->file;
        }

        if (file != loadedFile)
        {
            sourceVideo.stop();

            sourceVideo.unload();
            loadedFile = file;
            if (loadedFile.existsAsFile())
                loadSucceeded = sourceVideo.load(loadedFile);
            else
                loadSucceeded = false;
            sourceVideo.setVisible(true);
            sourceVideo.setOutputVisible(true);
            clipPlayer.setVisible(false);
            clipMode = false;
        }
    }

    void timerCallback() override
    {
        syncFile();
        if(previewEndMs!=0 && juce::Time::getMillisecondCounter()>=previewEndMs) {
            sourceVideo.pause();

            previewEndMs=0;
        }
    }
    void previewMarker(double seconds) {
        syncFile();
        if(!loadedFile.existsAsFile() || !sourceVideo.isAvailable()) return;
        clipPlayer.stop();
        audioClipSource.stop();
        clipPlayer.setVisible(false);
        sourceVideo.setVisible(true);
        sourceVideo.setOutputVisible(true);
        clipMode=false;
        sourceVideo.setPlayPosition(juce::jmax(0.0,seconds));

        sourceVideo.play();
        previewEndMs=juce::Time::getMillisecondCounter()+(juce::uint32)juce::roundToInt(clipPreviewDuration*1000.0);
    }
    void handleAsyncUpdate() override
    {
        std::deque<int> triggers, releases;
        bool fadeRelease = false;
        {
            const juce::ScopedLock sl(state->lock);
            triggers.swap(state->pendingTriggers);
            releases.swap(state->pendingNoteOffs);
            fadeRelease = state->fadeOnNoteOff;
        }
        for (int note : releases) { clipPlayer.releaseNote(note, fadeRelease); if (clipMode) audioClipSource.stop(); }

        // Route MIDI hits through triggerClip(), not directly to ClipFramePlayer.
        // triggerClip() performs the essential VLC -> prepared-frame display switch.
        // The newest MIDI hit wins within one UI dispatch.
        if (!triggers.empty()) {
            const int noteIndex=triggers.back();
            if(!triggerClip(noteIndex)) {
                double marker=-1.0;
                { const juce::ScopedLock sl(state->lock);
                  if(noteIndex>=0 && noteIndex<(int)state->markers.size() && !state->markers[(size_t)noteIndex].empty())
                      marker=state->markers[(size_t)noteIndex].front();
                }
                if(marker>=0.0) previewMarker(marker);
            }
        }
    }

    std::shared_ptr<VideoState> state;
    vlcplayer::VideoView sourceVideo;
    ClipFramePlayer clipPlayer;
    juce::File loadedFile;
    bool loadSucceeded = false;
    bool clipMode = false;
    juce::uint32 previewEndMs=0;
    double clipPreviewDuration=2.0;
    ClipAudioSource audioClipSource;
    TimelineAudioSource timelineAudioSource;

    juce::MixerAudioSource audioMixer;
#if JUCE_WINDOWS
    AudioBridgeSource bridgeSource { audioMixer };
#endif
     juce::AudioDeviceManager audioDeviceManager;
    juce::AudioSourcePlayer audioSourcePlayer;
};

class OutputWindow final : public juce::DocumentWindow {
public:
    explicit OutputWindow(std::shared_ptr<VideoState> s):DocumentWindow("MIDI Video Player | Sortie OBS",juce::Colours::black,allButtons){
        setUsingNativeTitleBar(true);setContentOwned(new VideoOutputView(std::move(s)),true);setResizable(true,true);
        getConstrainer()->setFixedAspectRatio(16.0/9.0);setResizeLimits(480,270,1920,1080);centreWithSize(960,540);
    }
    void closeButtonPressed() override {setVisible(false);}
    juce::String playVideo() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->play(); return "Fenêtre de sortie vidéo indisponible."; }
    void pauseVideo() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->pause(); }
    void stopVideo() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->stop(); }
    bool isVideoPlaying() const { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->isPlaying(); return false; }
    void seekVideo(double seconds) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->seek(seconds); }
    double videoPosition() const { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->position(); return 0.0; }
    double videoDuration() const { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->duration(); return 0.0; }
    juce::String videoDiagnostic() const { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->diagnostic(); return "Sortie OBS non ouverte"; }
    void configureAudio() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->configureAudio(); }
    juce::StringArray availableAudioOutputs() const { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->availableAudioOutputs(); return {}; }
    juce::String currentAudioOutput() const { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->currentAudioOutput(); return {}; }
    juce::String selectAudioOutput(const juce::String& name) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->selectAudioOutput(name); return "Fenêtre audio indisponible."; }
    void setOutputMuted(bool muted) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->setOutputMuted(muted); }
    bool setTimelineAudioFile(const juce::File& file) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) return v->setTimelineAudioFile(file); return false; }
    void previewTimelineAudio(double seconds) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->previewTimelineAudio(seconds); }
    void stopTimelineAudio() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->stopTimelineAudio(); }

    void setPreviewDuration(double seconds){if(auto* v=dynamic_cast<VideoOutputView*>(getContentComponent()))v->setPreviewDuration(seconds);}
     void setPreparedClip(int noteIndex, PreparedClip clip, double seconds) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->setPreparedClip(noteIndex, std::move(clip), seconds); }
    void clearPreparedClip(int noteIndex) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->clearPreparedClip(noteIndex); }
    void clearPreparedClips() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->clearPreparedClips(); }
};

class MainView final : public juce::Component, private juce::Timer {
public:
    MainView():state(std::make_shared<VideoState>()),router(state),timeline(state){
        juce::PropertiesFile::Options o;o.applicationName="MIDI Video Player";o.filenameSuffix="settings";o.folderName="MIDI Video Player";
        settings=std::make_unique<juce::PropertiesFile>(o);
        auto path=settings->getValue("video");
        clipDuration = juce::jlimit(1.0, 8.0, settings->getDoubleValue("clipDuration", 2.0));
        for(int i=0;i<MaxNotes;++i){auto key="markers"+juce::String(i);state->markers[(size_t)i]=parseMarkers(settings->getValue(key));}
        state->octaveCount=juce::jlimit(1,MaxOctaves,settings->getIntValue("octaveCount",1));
        timelineViewport.setViewedComponent(&timeline,false); timeline.refreshHeight(); addAndMakeVisible(timelineViewport); videoStatus.setColour(juce::Label::textColourId,juce::Colour(0xff39ff14));videoStatus.setText("Aucune vidéo chargée",juce::dontSendNotification);
        setLookAndFeel(&readableLookAndFeel);
        addAndMakeVisible(newProject);addAndMakeVisible(openProject);addAndMakeVisible(saveProject);addAndMakeVisible(saveAsProject);
        addAndMakeVisible(openVideo);addAndMakeVisible(play);addAndMakeVisible(addMarker);addAndMakeVisible(eraseMarker);addAndMakeVisible(noteChoice);addAndMakeVisible(octaveChoice);addAndMakeVisible(clipDurationChoice);addAndMakeVisible(preloadClipButton);addAndMakeVisible(preloadAllButton);addAndMakeVisible(preloadProgressBar);addAndMakeVisible(preloadStatusLabel);addAndMakeVisible(videoStatus);
        newProject.setButtonText("Nouveau projet");newProject.onClick=[this]{newProjectAction();};
        openProject.setButtonText("Ouvrir projet");openProject.onClick=[this]{chooseProjectToOpen();};
        saveProject.setButtonText("Enregistrer");saveProject.onClick=[this]{saveProjectAction();};
        saveAsProject.setButtonText("Enregistrer sous");saveAsProject.onClick=[this]{chooseProjectToSave();};
        for(auto* b:{&newProject,&openProject,&saveProject,&saveAsProject,&openVideo,&play,&addMarker,&eraseMarker,&refresh,&outputButton,&audioOutputButton,&preloadClipButton,&preloadAllButton}) {
            b->setColour(juce::TextButton::buttonColourId,juce::Colour(0xff101610));
            b->setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xff39ff14).withAlpha(0.22f));
            b->setColour(juce::TextButton::textColourOffId,juce::Colour(0xfff4fff2));
            b->setColour(juce::TextButton::textColourOnId,juce::Colour(0xff39ff14));
        }
        for(auto* box:{&noteChoice,&clipDurationChoice,&inputBox,&outputBox,&octaveChoice}) {
            box->setColour(juce::ComboBox::backgroundColourId,juce::Colour(0xff101610));
            box->setColour(juce::ComboBox::textColourId,juce::Colour(0xfff4fff2));
            box->setColour(juce::ComboBox::outlineColourId,juce::Colour(0xff39ff14).withAlpha(0.8f));
            box->setColour(juce::ComboBox::arrowColourId,juce::Colour(0xff39ff14));
        }
        addAndMakeVisible(inputLabel);addAndMakeVisible(outputLabel);addAndMakeVisible(inputBox);addAndMakeVisible(outputBox);addAndMakeVisible(refresh);
        addAndMakeVisible(outputButton);addAndMakeVisible(audioOutputButton);
        addAndMakeVisible(localMuteButton);addAndMakeVisible(noteReleaseButton);
        addAndMakeVisible(audioOutputLabel);addAndMakeVisible(audioOutputChoice);addAndMakeVisible(refreshAudioButton);
        audioOutputLabel.setText("Périphérique audio de sortie",juce::dontSendNotification);
        refreshAudioButton.setButtonText("Actualiser les sorties audio");
        refreshAudioButton.onClick=[this]{refreshAudioOutputs();};
        audioOutputChoice.onChange=[this]{selectAudioOutput();};
        state->fadeOnNoteOff=settings->getBoolValue("fadeOnNoteOff",false);
        noteReleaseButton.setButtonText(state->fadeOnNoteOff ? "Relâchement : fondu" : "Relâchement : arrêt net");
        noteReleaseButton.onClick=[this]{ {const juce::ScopedLock sl(state->lock);state->fadeOnNoteOff=!state->fadeOnNoteOff;noteReleaseButton.setButtonText(state->fadeOnNoteOff ? "Relâchement : fondu" : "Relâchement : arrêt net");settings->setValue("fadeOnNoteOff",state->fadeOnNoteOff);settings->saveIfNeeded();} };
         localMuteButton.setButtonText("Mute audio désactivé");localMuteButton.setClickingTogglesState(true);
        localMuteButton.onClick=[this]{const bool muted=localMuteButton.getToggleState();localMuteButton.setButtonText(muted?"Mute audio activé":"Mute audio désactivé");if(outputWindow)outputWindow->setOutputMuted(muted);};
        openVideo.setButtonText("Ouvrir une vidéo");openVideo.onClick=[this]{chooseVideo();};
        play.setButtonText("Lire / Pause");play.onClick=[this]{ensureOutputWindow();if(outputWindow->isVideoPlaying()){outputWindow->pauseVideo();videoStatus.setText("Lecture en pause",juce::dontSendNotification);}else{outputWindow->setVisible(true);outputWindow->toFront(true);auto result=outputWindow->playVideo();videoStatus.setText(result,juce::dontSendNotification);}};
        addMarker.setButtonText("Ajouter un repère à la position");addMarker.onClick=[this]{addCurrentMarker();};
        eraseMarker.setButtonText("Gomme désactivée");eraseMarker.setClickingTogglesState(true);eraseMarker.onClick=[this]{bool on=eraseMarker.getToggleState();eraseMarker.setButtonText(on?"Gomme activée : cliquer un repère":"Gomme désactivée");timeline.setEraseMode(on);};
        rebuildNoteChoice();
        noteChoice.setSelectedId(1,juce::dontSendNotification);
        octaveChoice.addItem("1 octave",1);octaveChoice.addItem("2 octaves",2);octaveChoice.addItem("3 octaves",3);octaveChoice.addItem("4 octaves",4);
        octaveChoice.setSelectedId(state->octaveCount);
        octaveChoice.onChange=[this]{changeOctaveCount(octaveChoice.getSelectedId());};
        for(int seconds=1;seconds<=8;++seconds)
            clipDurationChoice.addItem("Clip " + juce::String(seconds) + " s", seconds);
        clipDurationChoice.setSelectedId((int) clipDuration, juce::dontSendNotification);
        clipDurationChoice.onChange=[this]{
            clipDuration = (double) clipDurationChoice.getSelectedId();
            if(outputWindow) outputWindow->setPreviewDuration(clipDuration);
            settings->setValue("clipDuration", clipDuration);
            settings->saveIfNeeded();
            cancelPreload("Durée modifiée. Précharge à nouveau les clips nécessaires.");
            if(outputWindow) outputWindow->clearPreparedClips();
            markProjectDirty();
        };
        preloadClipButton.setButtonText("Précharger la note sélectionnée");
        preloadClipButton.onClick=[this]{batchPreloadActive=false;batchPreloadNotes.clear();startSelectedClipPreload(false);};
        preloadAllButton.setButtonText("Précharger toutes les notes");
        preloadAllButton.onClick=[this]{startAllClipsPreload();};
        preloadProgressBar.setColour(juce::ProgressBar::backgroundColourId,juce::Colour(0xff101610));
        preloadProgressBar.setColour(juce::ProgressBar::foregroundColourId,juce::Colour(0xff39ff14));
        preloadStatusLabel.setColour(juce::Label::textColourId,juce::Colour(0xffd8ffd1));
        preloadStatusLabel.setText("Choisis une note, place son marqueur, puis précharge manuellement.",juce::dontSendNotification);
        noteChoice.onChange=[this]{updatePreloadSelectionText();};
        timeline.changed=[this](int noteIndex){saveMarkers(noteIndex);};
        timeline.seeked=[this](double t){if(outputWindow){outputWindow->seekVideo(t);outputWindow->previewTimelineAudio(t);}};
        inputLabel.setText("Entrée MIDI",juce::dontSendNotification);outputLabel.setText("Sortie MIDI (loopMIDI)",juce::dontSendNotification);

        refresh.setButtonText("Actualiser");refresh.onClick=[this]{refreshMidi();};inputBox.onChange=[this]{selectInput();};outputBox.onChange=[this]{selectOutput();};
        outputButton.setButtonText("Ouvrir la sortie vidéo OBS");outputButton.onClick=[this]{ensureOutputWindow();outputWindow->setVisible(true);outputWindow->toFront(true);};
        audioOutputButton.setButtonText("Configurer la sortie audio");audioOutputButton.onClick=[this]{ensureOutputWindow();outputWindow->configureAudio();};
        refreshMidi();ensureOutputWindow();refreshAudioOutputs();setSize(1120,900);
        auto recovery=recoveryProjectFile();
        bool restoredProject = false;
        if(recovery.existsAsFile() && recovery.getSize()>0) {
            loadProjectFile(recovery);
            restoredProject = true;
        } else {
            auto last=settings->getValue("lastProject");
            if(last.isNotEmpty() && juce::File(last).existsAsFile()) {
                loadProjectFile(juce::File(last));
                restoredProject = true;
            }
        }
        // Fall back to the last selected video only when there is no project to restore.
        if(!restoredProject && path.isNotEmpty()) {
            juce::File f(path);
            if(f.existsAsFile()) loadVideo(f);
        }
        startTimerHz(10);
    }
    ~MainView() override {cancelPreload("Fermeture du logiciel.");outputWindow.reset();setLookAndFeel(nullptr);if(settings)settings->saveIfNeeded();}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff050705));
        g.setColour(juce::Colour(0xff39ff14));
        g.setFont(ReadableLookAndFeel::uiFont(22.0f, juce::Font::bold));
        g.drawText("MIDI VIDEO PLAYER",24,12,getWidth()-48,30,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xfff4fff2));
        g.setFont(ReadableLookAndFeel::uiFont(13.0f));
        g.drawText("Départ Do3 | Les notes sont affichées à gauche de chaque piste. Teste un marqueur avant de le précharger.",24,42,getWidth()-48,22,juce::Justification::centredLeft);
    }
    void setWaveformData(std::vector<float> p) { timeline.setWaveform(std::move(p)); }

    void changeOctaveCount(int count) {
        count=juce::jlimit(1,MaxOctaves,count);
        { const juce::ScopedLock sl(state->lock); state->octaveCount=count; }
        settings->setValue("octaveCount",count); settings->saveIfNeeded();
        rebuildNoteChoice();
        timeline.refreshHeight(); timelineViewport.setViewPosition(0,0);
        markProjectDirty(); updatePreloadSelectionText();
    }
    void rebuildNoteChoice() {
        const int previous=noteChoice.getSelectedId();
        int count=1; { const juce::ScopedLock sl(state->lock); count=state->octaveCount; }
        noteChoice.clear(juce::dontSendNotification);
        for(int i=0;i<count*PitchCount;++i) noteChoice.addItem(noteLabel(i),i+1);
        noteChoice.setSelectedId(juce::jlimit(1,count*PitchCount,previous>0?previous:1),juce::dontSendNotification);
    }
    void resized() override {
        auto a=getLocalBounds().reduced(22);a.removeFromTop(72);
        auto projectBar=a.removeFromTop(38);
        newProject.setBounds(projectBar.removeFromLeft(145));projectBar.removeFromLeft(7);
        openProject.setBounds(projectBar.removeFromLeft(130));projectBar.removeFromLeft(7);
        saveProject.setBounds(projectBar.removeFromLeft(115));projectBar.removeFromLeft(7);
        saveAsProject.setBounds(projectBar.removeFromLeft(145));
        a.removeFromTop(8);
        auto top=a.removeFromTop(260);auto left=top.removeFromLeft(250);videoStatus.setBounds(left.removeFromTop(44));localMuteButton.setBounds(left.removeFromTop(32));noteReleaseButton.setBounds(left.removeFromTop(32));left.removeFromTop(5);audioOutputLabel.setBounds(left.removeFromTop(20));audioOutputChoice.setBounds(left.removeFromTop(28));left.removeFromTop(4);refreshAudioButton.setBounds(left.removeFromTop(28));auto controls=top.reduced(12,0);
        openVideo.setBounds(controls.removeFromTop(36));controls.removeFromTop(8);play.setBounds(controls.removeFromTop(34));controls.removeFromTop(8);
        inputLabel.setBounds(controls.removeFromTop(20));inputBox.setBounds(controls.removeFromTop(28));controls.removeFromTop(5);
        outputLabel.setBounds(controls.removeFromTop(20));outputBox.setBounds(controls.removeFromTop(28));refresh.setBounds(controls.removeFromTop(30));audioOutputButton.setBounds(controls.removeFromTop(44));
        a.removeFromTop(8);auto timelineHead=a.removeFromTop(32);addMarker.setBounds(timelineHead.removeFromLeft(220));timelineHead.removeFromLeft(8);eraseMarker.setBounds(timelineHead.removeFromLeft(160));timelineHead.removeFromLeft(8);noteChoice.setBounds(timelineHead.removeFromLeft(90));timelineHead.removeFromLeft(6);octaveChoice.setBounds(timelineHead.removeFromLeft(90));timelineHead.removeFromLeft(6);clipDurationChoice.setBounds(timelineHead.removeFromLeft(95));timelineHead.removeFromLeft(8);preloadClipButton.setBounds(timelineHead.removeFromLeft(190));timelineHead.removeFromLeft(6);preloadAllButton.setBounds(timelineHead.removeFromLeft(190));
        a.removeFromTop(5);auto preloadRow=a.removeFromTop(24);preloadProgressBar.setBounds(preloadRow.removeFromLeft(250));preloadRow.removeFromLeft(10);preloadStatusLabel.setBounds(preloadRow);
        timelineViewport.setBounds(a.removeFromTop(350));timeline.setSize(timelineViewport.getWidth(),timeline.requiredHeight());a.removeFromTop(8);
        outputButton.setBounds(a.removeFromTop(38).withWidth(250));
    }
private:
    double lastTimelineDuration=0.0;
    bool timelineDurationNeedsUpdate(double duration) const { return std::abs(duration - lastTimelineDuration) > 0.02; }
    void markProjectDirty(){projectDirty=true;nextAutoSaveMs=juce::Time::getMillisecondCounter()+1000;}
    void timerCallback() override {
        if(outputWindow && outputWindow->isVisible()){
            double p=outputWindow->videoPosition();
            double duration=outputWindow->videoDuration();
            {
                const juce::ScopedLock sl(state->lock);
                if(outputWindow->isVideoPlaying())
                    state->playhead=p;
                if(duration>0.0)
                    state->duration=duration;
            }
            if(duration>0.0 && timelineDurationNeedsUpdate(duration)){lastTimelineDuration=duration;timeline.setDuration(duration);}
            timeline.repaint();
        }
        if(preloadFeedback){
            double progress = 0.0;
            bool running = false;
            juce::String message;
            {
                const juce::ScopedLock sl(preloadFeedback->lock);
                progress = preloadFeedback->progress;
                running = preloadFeedback->running;
                message = preloadFeedback->message;
            }
            preloadProgressValue = progress;
            if(message != lastPreloadMessage){
                lastPreloadMessage = message;
                preloadStatusLabel.setText(message,juce::dontSendNotification);
            }
            preloadClipButton.setEnabled(!running && !batchPreloadActive);preloadAllButton.setEnabled(!running);
        }
        if(projectDirty && juce::Time::getMillisecondCounter() >= nextAutoSaveMs){
            writeRecoveryProject();projectDirty=false;nextAutoSaveMs=juce::Time::getMillisecondCounter()+3000;
        }
    }
    void chooseVideo(){chooser=std::make_unique<juce::FileChooser>("Choisir une vidéo",juce::File(),"*.mp4;*.mov;*.m4v;*.avi;*.wmv");chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& c){auto f=c.getResult();if(f.existsAsFile())loadVideo(f);chooser.reset();});}
    void loadVideo(const juce::File& f){
        videoStatus.setText("Chargement de : "+f.getFileName(),juce::dontSendNotification);
        if(f.existsAsFile()){
            {const juce::ScopedLock sl(state->lock);state->file=f;state->duration=0.0;state->playhead=0.0;state->pendingTriggers.clear();}
            timeline.setDuration(60.0);timeline.setWaveform({});timeline.repaint();
            juce::Component::SafePointer<MainView> safe(this);
            waveformJobs.addJob(new WaveformJob(f,[safe, source=f](std::vector<float> p, juce::File audio){if(safe){bool current=false;{const juce::ScopedLock sl(safe->state->lock);current=(safe->state->file==source);}if(current){safe->setWaveformData(std::move(p));if(audio.existsAsFile()){safe->ensureOutputWindow();if(safe->outputWindow)safe->outputWindow->setTimelineAudioFile(audio);}}}}),true);
            ensureOutputWindow();
            outputWindow->setVisible(true);outputWindow->toFront(true);
            // Start the source preview immediately so the user can position markers
            // without needing an extra Play click after loading a file.
            const auto previewStatus = outputWindow->playVideo();
            videoStatus.setText("Vidéo source : "+previewStatus+" | Préchargement MIDI manuel.",juce::dontSendNotification);
            settings->setValue("video",f.getFullPathName());settings->saveIfNeeded();
            cancelPreload("Vidéo chargée. Place un marqueur puis clique sur Précharger.");
            if(outputWindow) outputWindow->clearPreparedClips();
        }else videoStatus.setText("Fichier vidéo introuvable : "+f.getFullPathName(),juce::dontSendNotification);
    }
    void setPreloadMessage(const juce::String& message, double progress = 0.0, bool running = false)
    {
        if (!preloadFeedback)
            preloadFeedback = std::make_shared<ClipPreloadFeedback>();
        const juce::ScopedLock sl(preloadFeedback->lock);
        preloadFeedback->message = message;
        preloadFeedback->progress = juce::jlimit(0.0, 1.0, progress);
        preloadFeedback->running = running;
    }

    bool preloadIsRunning() const
    {
        if (!preloadFeedback)
            return false;
        const juce::ScopedLock sl(preloadFeedback->lock);
        return preloadFeedback->running;
    }

    void cancelPreload(const juce::String& reason)
    {
        batchPreloadActive=false;
        batchPreloadNotes.clear();
        batchPreloadPosition=0;
        preloadAllButton.setEnabled(true);
        ++preloadGeneration;
        clipJobs.removeAllJobs(true, 0);
        preloadNoteIndex = -1;
        // A cancelled worker keeps its own feedback object and cannot overwrite
        // progress for a later preload operation.
        preloadFeedback = std::make_shared<ClipPreloadFeedback>();
        setPreloadMessage(reason);
        lastPreloadMessage = {};
        preloadProgressValue = 0.0;
        preloadClipButton.setEnabled(true);
        preloadStatusLabel.setText(reason,juce::dontSendNotification);
    }

    void updatePreloadSelectionText()
    {
        const int note = noteChoice.getSelectedId() - 1;
        if (note < 0 || note >= state->octaveCount*PitchCount || preloadIsRunning())
            return;
        double marker = -1.0;
        {
            const juce::ScopedLock sl(state->lock);
            if (!state->markers[(size_t) note].empty())
                marker = state->markers[(size_t) note].front();
        }
        if (marker < 0.0)
            setPreloadMessage("Note " + noteLabel(note)
                              + " : aucun marqueur. Place-le avant de précharger.");
        else
            setPreloadMessage("Note " + noteLabel(note)
                              + " : repère à " + juce::String(marker, 3)
                              + " s. Préchargement manuel disponible.");
        lastPreloadMessage = {};
    }

    void startSelectedClipPreload(bool batchRun)
    {
        const int note = noteChoice.getSelectedId() - 1;
        if (note < 0 || note >= state->octaveCount*PitchCount)
            return;

        juce::File source;
        double markerStart = -1.0;
        {
            const juce::ScopedLock sl(state->lock);
            source = state->file;
            if (!state->markers[(size_t) note].empty())
                markerStart = state->markers[(size_t) note].front();
        }

        if (!source.existsAsFile())
        {
            if(batchRun){batchPreloadActive=false;batchPreloadNotes.clear();preloadAllButton.setEnabled(true);}
            setPreloadMessage("Charge d'abord une vidéo source.");
            lastPreloadMessage = {};
            return;
        }
        if (markerStart < 0.0)
        {
            setPreloadMessage("Aucun repère pour " + noteLabel(note)
                              + ". Place le marqueur avant de précharger.");
            lastPreloadMessage = {};
            return;
        }

        ++preloadGeneration;
        const int generation = preloadGeneration;
        clipJobs.removeAllJobs(true, 0);
        preloadFeedback = std::make_shared<ClipPreloadFeedback>();
        preloadNoteIndex = note;
        setPreloadMessage(batchRun ? ("Préchargement global " + juce::String(batchPreloadPosition)
                          + " / " + juce::String(batchPreloadNotes.size()) + " : " + noteLabel(note))
                          : ("Préparation de " + noteLabel(note) + " depuis " + juce::String(markerStart, 3) + " s..."),
                          0.0, true);
        lastPreloadMessage = {};
        preloadProgressValue = 0.0;
        preloadClipButton.setEnabled(false);

        ensureOutputWindow();
        juce::Component::SafePointer<MainView> safe(this);
        auto feedback = preloadFeedback;
        clipJobs.addJob(new ClipPreloadJob(source, note, markerStart, clipDuration, feedback,
            [safe, generation, seconds = clipDuration, batchRun](int finishedNote,
                                                        PreparedClip clip,
                                                        juce::String message,
                                                        bool success) mutable
            {
                if (!safe || safe->preloadGeneration != generation)
                    return;

                safe->preloadNoteIndex = -1;
                if (success)
                    safe->outputWindow->setPreparedClip(finishedNote, std::move(clip), seconds);
                else
                    safe->outputWindow->clearPreparedClip(finishedNote);

                {
                    const juce::ScopedLock sl(safe->preloadFeedback->lock);
                    safe->preloadFeedback->running = false;
                    safe->preloadFeedback->progress = success ? 1.0 : 0.0;
                    safe->preloadFeedback->message = message;
                }
                safe->lastPreloadMessage = {};
                safe->preloadClipButton.setEnabled(true);
                safe->preloadProgressValue = success ? 1.0 : 0.0;
                if(batchRun && safe->batchPreloadActive) safe->startNextBatchPreload();
            }), true);
    }

    void startAllClipsPreload()
    {
        if(preloadIsRunning()) return;
        batchPreloadNotes.clear();
        {
            const juce::ScopedLock sl(state->lock);
            const int count=juce::jmin((int)state->markers.size(),state->octaveCount*PitchCount);
            for(int i=0;i<count;++i)
                if(!state->markers[(size_t)i].empty()) batchPreloadNotes.push_back(i);
        }
        if(batchPreloadNotes.empty()) { setPreloadMessage("Aucune note ne possède de marqueur à précharger."); lastPreloadMessage={}; return; }
        batchPreloadActive=true;
        batchPreloadPosition=0;
        startNextBatchPreload();
    }

    void startNextBatchPreload()
    {
        if(!batchPreloadActive) return;
        if(batchPreloadPosition >= (int)batchPreloadNotes.size()) {
            batchPreloadActive=false;
            batchPreloadNotes.clear();
            preloadClipButton.setEnabled(true);
            preloadAllButton.setEnabled(true);
            setPreloadMessage("Préchargement terminé : toutes les notes avec marqueur sont prêtes.",1.0,false);
            lastPreloadMessage={};
            return;
        }
        const int note=batchPreloadNotes[(size_t)batchPreloadPosition++];
        noteChoice.setSelectedId(note+1,juce::dontSendNotification);
        startSelectedClipPreload(true);
        setPreloadMessage("Préchargement global : note " + juce::String(batchPreloadPosition)
            + " / " + juce::String(batchPreloadNotes.size()) + " (" + noteLabel(note) + ")",0.0,true);
        lastPreloadMessage={};
    }

    void ensureOutputWindow(){if(!outputWindow){outputWindow=std::make_unique<OutputWindow>(state);outputWindow->setPreviewDuration(clipDuration);}}
    juce::File recoveryProjectFile() const {
        auto dir=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MIDI Video Player");
        dir.createDirectory();return dir.getChildFile("Recovery.mvproj");
    }
    juce::var makeProjectData(){
        auto* obj=new juce::DynamicObject();juce::var root(obj);
        const juce::ScopedLock sl(state->lock);
        obj->setProperty("formatVersion",3);
        obj->setProperty("video",state->file.getFullPathName());
        obj->setProperty("clipDuration", clipDuration);
        obj->setProperty("midiInput",settings->getValue("input"));
        obj->setProperty("midiOutput",settings->getValue("output"));
        juce::Array<juce::var> all;
        for(int i=0;i<MaxNotes;++i) all.add(markerString(state->markers[(size_t)i]));
        obj->setProperty("markers",all);
        obj->setProperty("octaveCount",state->octaveCount);
        return root;
    }
    bool writeProjectFile(const juce::File& file){
        auto json=juce::JSON::toString(makeProjectData(),true);
        if(!file.replaceWithText(json)) return false;
        if(file!=recoveryProjectFile()){
            settings->setValue("lastProject",file.getFullPathName());settings->saveIfNeeded();
            recoveryProjectFile().deleteFile();currentProject=file;
        }
        projectDirty=false;return true;
    }
    void writeRecoveryProject(){auto f=recoveryProjectFile();f.replaceWithText(juce::JSON::toString(makeProjectData(),true));}
    void saveProjectAction(){
        if(currentProject.existsAsFile()){
            if(writeProjectFile(currentProject)) videoStatus.setText("Projet enregistré : "+currentProject.getFileName(),juce::dontSendNotification);
        }else chooseProjectToSave();
    }
    void chooseProjectToSave(){
        chooser=std::make_unique<juce::FileChooser>("Enregistrer le projet",juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Mon projet.mvproj"),"*.mvproj");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& c){
            auto file=c.getResult();if(file!=juce::File()){if(file.getFileExtension().isEmpty())file=file.withFileExtension(".mvproj");if(writeProjectFile(file))videoStatus.setText("Projet enregistré : "+file.getFileName(),juce::dontSendNotification);}
            chooser.reset();
        });
    }
    void chooseProjectToOpen(){
        chooser=std::make_unique<juce::FileChooser>("Ouvrir un projet",juce::File(),"*.mvproj");
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& c){auto file=c.getResult();if(file.existsAsFile())loadProjectFile(file);chooser.reset();});
    }
    void loadProjectFile(const juce::File& file){
        auto data=juce::JSON::parse(file.loadFileAsString());
        if(!data.isObject()){videoStatus.setText("Projet illisible ou endommagé",juce::dontSendNotification);return;}
        auto* obj=data.getDynamicObject();auto markers=obj->getProperty("markers");
        const double savedClipDuration = obj->hasProperty("clipDuration")
                                           ? (double) obj->getProperty("clipDuration") : 2.0;
        clipDuration = juce::jlimit(1.0, 8.0, savedClipDuration);
        if (clipDurationChoice.getSelectedId() != (int) clipDuration)
            clipDurationChoice.setSelectedId((int) clipDuration, juce::dontSendNotification);
        settings->setValue("clipDuration", clipDuration);
        juce::String videoPath;
        {
            const juce::ScopedLock sl(state->lock);
            state->octaveCount=juce::jlimit(1,MaxOctaves,obj->hasProperty("octaveCount")?(int)obj->getProperty("octaveCount"):1);
            octaveChoice.setSelectedId(state->octaveCount,juce::dontSendNotification);
            settings->setValue("octaveCount",state->octaveCount);
            for(int i=0;i<MaxNotes;++i) state->markers[(size_t)i]=(markers.isArray()&&i<markers.size())?parseMarkers(markers[i].toString()):std::vector<double>{};
            videoPath=obj->getProperty("video").toString();
            state->playhead=0.0;
        }
        rebuildNoteChoice();
        settings->setValue("input",obj->getProperty("midiInput").toString());settings->setValue("output",obj->getProperty("midiOutput").toString());
        settings->saveIfNeeded();currentProject=(file==recoveryProjectFile()?juce::File{}:file);projectDirty=false;
        auto videoFile=juce::File(videoPath);
        if(videoFile.existsAsFile()) loadVideo(videoFile);
        else {
            {const juce::ScopedLock sl(state->lock);state->file=videoFile;}
            cancelPreload("Vidéo introuvable. Aucun clip préchargé.");
            if(outputWindow) outputWindow->clearPreparedClips();
            videoStatus.setText("Vidéo introuvable : "+videoPath,juce::dontSendNotification);
        }
        timeline.refreshHeight(); timelineViewport.setViewPosition(0,0);
        timeline.setDuration(state->duration>0.0?state->duration:60.0);timeline.repaint();refreshMidi();
        if(videoPath.isEmpty()) videoStatus.setText("Projet chargé : aucune vidéo associée",juce::dontSendNotification);
        else if(!videoFile.existsAsFile()) videoStatus.setText("Projet chargé, vidéo introuvable : "+videoPath,juce::dontSendNotification);
        else videoStatus.setText("Projet chargé : "+file.getFileName(),juce::dontSendNotification);
    }
    void newProjectAction(){
        {const juce::ScopedLock sl(state->lock);for(auto& m:state->markers)m.clear();state->file={};state->duration=0;state->playhead=0;state->pendingTriggers.clear();}
        cancelPreload("Nouveau projet. Aucun clip préchargé.");
        if(outputWindow){outputWindow->stopVideo();outputWindow->clearPreparedClips();}
        {const juce::ScopedLock sl(state->lock);state->file={};state->duration=0.0;state->playhead=0.0;}
        timeline.setDuration(60.0);timeline.repaint();currentProject={};projectDirty=true;
        videoStatus.setText("Nouveau projet",juce::dontSendNotification);writeRecoveryProject();
    }
    void addCurrentMarker()
    {
        int p=noteChoice.getSelectedId()-1;
        if(p<0||p>=state->octaveCount*PitchCount)return;
        double t=outputWindow?outputWindow->videoPosition():0.0;
        {
            const juce::ScopedLock sl(state->lock);
            t=juce::jlimit(0.0,state->duration,t);
            auto& v=state->markers[(size_t)p];
            v.clear();
            v.push_back(t);
        }
        saveMarkers(p);
        timeline.repaint();
    }

    void saveMarkers(int changedNote = -1)
    {
        {
            const juce::ScopedLock sl(state->lock);
            for(int i=0;i<MaxNotes;++i)
                settings->setValue("markers"+juce::String(i),markerString(state->markers[(size_t)i]));
        }
        settings->saveIfNeeded();
        markProjectDirty();

        if(changedNote >= 0 && changedNote < MaxNotes)
        {
            if(outputWindow)
                outputWindow->clearPreparedClip(changedNote);
            // Also invalidate a just-finished job whose UI callback may still be queued.
            if(preloadNoteIndex == changedNote)
                cancelPreload("Marqueur " + noteLabel(changedNote)
                              + " modifié. Préchargement annulé.");
            else if(!preloadIsRunning())
                setPreloadMessage("Repère " + noteLabel(changedNote)
                                  + " modifié. Clique sur Précharger quand sa position te convient.");
            lastPreloadMessage = {};
        }
    }

    void refreshAudioOutputs(){
        if (!outputWindow) ensureOutputWindow();
        audioOutputChoice.clear(juce::dontSendNotification);
        audioOutputChoice.addItem("Choisir une sortie audio...",1);
        const auto names = outputWindow->availableAudioOutputs();
        for (int i=0;i<names.size();++i)
            audioOutputChoice.addItem(names[i],i+2);
        const auto saved = settings->getValue("audioOutput");
        const auto current = outputWindow->currentAudioOutput();
        const auto preferred = saved.isNotEmpty() ? saved : current;
        for (int i=0;i<names.size();++i)
            if (names[i] == preferred) {
                audioOutputChoice.setSelectedId(i+2,juce::dontSendNotification);
                break;
            }
    }
    void selectAudioOutput(){
        const int index=audioOutputChoice.getSelectedId()-2;
        if(index<0 || !outputWindow) return;
        const auto names=outputWindow->availableAudioOutputs();
        if(index>=names.size()) return;
        const auto error=outputWindow->selectAudioOutput(names[index]);
        if(error.isEmpty()){
            settings->setValue("audioOutput",names[index]);
            settings->saveIfNeeded();
            videoStatus.setText("Sortie audio sélectionnée : "+names[index],juce::dontSendNotification);
        } else {
            videoStatus.setText("Impossible de sélectionner cette sortie : "+error,juce::dontSendNotification);
            refreshAudioOutputs();
        }
    }

    void refreshMidi(){ins=router.inputs();outs=router.outputs();inputBox.clear();outputBox.clear();inputBox.addItem("Choisir une entrée...",1);outputBox.addItem("Choisir une sortie...",1);for(int i=0;i<ins.size();++i)inputBox.addItem(ins.getReference(i).name,i+2);for(int i=0;i<outs.size();++i)outputBox.addItem(outs.getReference(i).name,i+2);
        auto si=settings->getValue("input"),so=settings->getValue("output");for(int i=0;i<ins.size();++i)if(ins.getReference(i).identifier==si)inputBox.setSelectedId(i+2,juce::dontSendNotification);for(int i=0;i<outs.size();++i)if(outs.getReference(i).identifier==so)outputBox.setSelectedId(i+2,juce::dontSendNotification);}
    void selectInput(){int i=inputBox.getSelectedId()-2;if(i>=0&&i<ins.size()){auto d=ins.getReference(i);router.openInput(d.identifier);settings->setValue("input",d.identifier);settings->saveIfNeeded();}}
    void selectOutput(){int i=outputBox.getSelectedId()-2;if(i>=0&&i<outs.size()){auto d=outs.getReference(i);router.openOutput(d.identifier);settings->setValue("output",d.identifier);settings->saveIfNeeded();}}
    std::shared_ptr<VideoState> state;MidiRouter router;MarkerTimeline timeline;juce::Viewport timelineViewport;juce::ThreadPool waveformJobs{1};juce::ThreadPool clipJobs{1};
    std::shared_ptr<ClipPreloadFeedback> preloadFeedback = std::make_shared<ClipPreloadFeedback>();
    double clipDuration = 2.0;
    double preloadProgressValue = 0.0;
    int preloadGeneration = 0;
    int preloadNoteIndex = -1;
    std::vector<int> batchPreloadNotes;
    int batchPreloadPosition = 0;
    bool batchPreloadActive = false;
    juce::String lastPreloadMessage;
    ReadableLookAndFeel readableLookAndFeel;
    juce::TextButton newProject,openProject,saveProject,saveAsProject;
    juce::File currentProject;bool projectDirty=false;juce::uint32 nextAutoSaveMs=0;std::unique_ptr<juce::PropertiesFile> settings;std::unique_ptr<juce::FileChooser> chooser;std::unique_ptr<OutputWindow> outputWindow;
    juce::TextButton openVideo,play,addMarker,eraseMarker,refresh,outputButton,audioOutputButton,preloadClipButton,preloadAllButton,localMuteButton,noteReleaseButton,refreshAudioButton;juce::ComboBox noteChoice,clipDurationChoice,inputBox,outputBox,octaveChoice,audioOutputChoice;juce::Label inputLabel,outputLabel,videoStatus,preloadStatusLabel,audioOutputLabel;juce::ProgressBar preloadProgressBar{preloadProgressValue};juce::Array<juce::MidiDeviceInfo> ins,outs;
};

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow():DocumentWindow("MIDI Video Player",juce::Colour(0xff050705),allButtons){setUsingNativeTitleBar(true);setContentOwned(new MainView(),true);setResizable(true,true);centreWithSize(1120,900);}
    void closeButtonPressed() override {juce::JUCEApplication::getInstance()->systemRequestedQuit();}
};
class App final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override{return "MIDI Video Player";}
    const juce::String getApplicationVersion() override{return "0.1.0";}
    bool moreThanOneInstanceAllowed() override{return false;}
    void initialise(const juce::String&) override {window=std::make_unique<MainWindow>();window->setVisible(true);window->toFront(true);}
    void shutdown() override {window.reset();}
private:std::unique_ptr<MainWindow> window;
};
}
START_JUCE_APPLICATION(App)