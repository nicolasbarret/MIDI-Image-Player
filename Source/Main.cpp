#include <JuceHeader.h>
#include "LibVlcPlayer.h"

#include <array>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstdint>

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
const std::array<const char*,12> notes{"Do","Do#","Ré","Ré#","Mi","Fa","Fa#","Sol","Sol#","La","La#","Si"};
struct VideoState {
    juce::CriticalSection lock;
    juce::File file;
    std::array<std::vector<double>,12> markers;
    double duration=0.0, playhead=0.0;
    std::deque<int> pendingTriggers;
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
            if(m.isNoteOn()) {
                const int midiNote=m.getNoteNumber();
                const int p=midiNote%12;
                auto& list=state->markers[(size_t)p];
                if(!list.empty()) {
                    double chosen=list.front(), best=std::abs(chosen-state->playhead);
                    for(double t:list){double d=std::abs(t-state->playhead);if(d<best){best=d;chosen=t;}}
                    state->playhead=chosen;
                    // Note Off remains forwarded to loopMIDI/FL Studio, but it does not
                    // pause the video. Short drum notes must not cut a running clip.
                    // Keep the MIDI event sequence. The video output switches clips
                    // without seeking or starting a decoder on the MIDI callback thread.
                    state->pendingTriggers.push_back(p);
                    while (state->pendingTriggers.size() > 128)
                        state->pendingTriggers.pop_front();
                    dispatch=true;
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
    std::function<void()> changed;
    std::function<void(double)> seeked;
    void setDuration(double d){duration=juce::jmax(0.01,d);viewStart=juce::jlimit(0.0,juce::jmax(0.0,d-visibleDuration()),viewStart);repaint();}
    void setWaveform(std::vector<float> peaks){waveform=std::move(peaks);repaint();}
    void setEraseMode(bool enabled){eraseMode=enabled;setMouseCursor(enabled?juce::MouseCursor::CrosshairCursor:juce::MouseCursor::PointingHandCursor);repaint();}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff050705));
        auto r=getLocalBounds().toFloat(); float left=48.0f, right=12.0f, top=25.0f;
        g.setColour(juce::Colour(0xffb8ffad)); g.setFont(ReadableLookAndFeel::uiFont(12.0f));
        g.drawText("Zoom x"+juce::String(zoom,1)+"   |   Molette : zoom   |   Maj + molette : déplacement",48,2,(int)r.getWidth()-60,19,juce::Justification::centredLeft);
        const float waveH=54.0f;
        const float trackArea=r.getHeight()-top-waveH-5.0f;
        float trackH=trackArea/12.0f;
        g.setFont(ReadableLookAndFeel::uiFont(12.0f));
        for(int i=0;i<12;++i){
            float y=top+i*trackH;
            g.setColour(i%2?juce::Colour(0xff101810):juce::Colour(0xff0b100b));
            g.fillRect(left,y,r.getWidth()-left-right,trackH-1.0f);
            g.setColour(juce::Colour(0xffd8ffd1));
            g.drawText(notes[(size_t)i],2,(int)y,42,(int)trackH,juce::Justification::centredLeft);
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
        auto r=getLocalBounds().toFloat(); float left=48.0f,right=12.0f,top=25.0f;
        const float waveH=54.0f;
        const float trackArea=r.getHeight()-top-waveH-5.0f;
        float th=trackArea/12.0f;
        float x=e.position.x; double clicked=xToTime(x,left,r.getWidth()-left-right);
        if(e.position.y>=top+trackArea) {
            clicked=juce::jlimit(0.0,duration,clicked);
            {const juce::ScopedLock sl(state->lock);state->playhead=clicked;}
            if(seeked)seeked(clicked);
            repaint();
            return;
        }
        int row=juce::jlimit(0,11,(int)((e.position.y-top)/th));
        dragRow=row; dragIndex=-1; double best=visibleDuration()*8.0/juce::jmax(1.0f,r.getWidth()-60.0f);
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
            if(erased && changed)changed();
            dragging=false;repaint();return;
        }
        if(dragIndex>=0){dragging=true;moveMarker(clicked);} else {dragging=false;{const juce::ScopedLock sl(state->lock);state->playhead=juce::jlimit(0.0,duration,clicked);}if(seeked)seeked(clicked);repaint();}
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if(!dragging)return;
        auto r=getLocalBounds().toFloat();double t=xToTime(e.position.x,48.0f,r.getWidth()-60.0f);moveMarker(t);
    }
    void mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& wheel) override {
        const float left=48.0f, right=12.0f;
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
        if(dragging && dragRow>=0){const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)dragRow];std::sort(v.begin(),v.end());}
        dragging=false;dragIndex=-1;if(changed)changed();
    }
private:
    double visibleDuration() const { return duration/zoom; }
    double xToTime(float x,float left,float width) const { return juce::jlimit(0.0,duration,viewStart+((double)(x-left)/juce::jmax(1.0f,width))*visibleDuration()); }
    float timeToX(double t,float left,float width) const { return left+(float)((t-viewStart)/visibleDuration())*width; }
    void moveMarker(double t){
        const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)dragRow];
        if(dragIndex>=0&&dragIndex<(int)v.size()){v[(size_t)dragIndex]=juce::jlimit(0.0,duration,t);}
        repaint();
    }
    std::shared_ptr<VideoState> state;double duration=60.0,zoom=1.0,viewStart=0.0;bool dragging=false,eraseMode=false;int dragRow=-1,dragIndex=-1;std::vector<float> waveform;
};

class WaveformJob final : public juce::ThreadPoolJob {
public:
    WaveformJob(juce::File f, std::function<void(std::vector<float>)> cb) : ThreadPoolJob("Audio waveform"), source(std::move(f)), callback(std::move(cb)) {}
    JobStatus runJob() override {
        std::vector<float> peaks(18000, 0.0f);
        auto app=juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
        auto ffmpeg=app.getChildFile("ffmpeg.exe");
        auto wav=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("mvp-waveform", ".wav", false);
        if(ffmpeg.existsAsFile() && source.existsAsFile()) {
            juce::StringArray args; args.add(ffmpeg.getFullPathName()); args.add("-y"); args.add("-i"); args.add(source.getFullPathName());
            args.add("-vn"); args.add("-ac"); args.add("1"); args.add("-ar"); args.add("8000"); args.add("-f"); args.add("wav"); args.add(wav.getFullPathName());
            juce::ChildProcess process;
            if(process.start(args) && process.waitForProcessToFinish(120000) && wav.existsAsFile()) {
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(wav));
                if(reader && reader->lengthInSamples>0) {
                    const auto total=reader->lengthInSamples; juce::AudioBuffer<float> buffer(1,8192);
                    for(juce::int64 offset=0;offset<total && !shouldExit();offset+=8192) {
                        int count=(int)juce::jmin((juce::int64)8192,total-offset);
                        if(!reader->read(&buffer,0,count,offset,true,false)) break;
                        for(int i=0;i<count;++i) { int bin=juce::jmin(17999,(int)(((offset+i)*18000)/total)); peaks[(size_t)bin]=juce::jmax(peaks[(size_t)bin],std::abs(buffer.getSample(0,i))); }
                    }
                    float maximum=0.0f; for(float p:peaks) maximum=juce::jmax(maximum,p);
                    if(maximum>0.0001f) for(auto& p:peaks) p/=maximum;
                }
            }
        }
        wav.deleteFile();
        if(callback && !shouldExit()) { auto cb=std::move(callback); juce::MessageManager::callAsync([cb=std::move(cb), data=std::move(peaks)]() mutable { cb(std::move(data)); }); }
        return jobHasFinished;
    }
private:
    juce::File source; std::function<void(std::vector<float>)> callback;
};

struct PreparedClip
{
    std::vector<juce::Image> frames;
    double sourceStart = 0.0;
};
using PreparedClipArray = std::array<PreparedClip, 12>;

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
    }

private:
    static constexpr int FrameRate = 24;

    void timerCallback() override
    {
        bool changed = false;
        {
            const juce::ScopedLock sl(lock);
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
    double clipDuration = 3.0;
    std::uint32_t startTimeMs = 0;
    std::uint32_t pausedElapsedMs = 0;
};

class ClipCacheJob final : public juce::ThreadPoolJob
{
public:
    ClipCacheJob(juce::File sourceFile,
                 std::array<double, 12> starts,
                 double secondsPerClip,
                 std::function<void(PreparedClipArray, juce::String)> finished)
        : ThreadPoolJob("Prepare MIDI video clips"),
          source(std::move(sourceFile)),
          markerStarts(starts),
          clipDuration(juce::jlimit(1.0, 8.0, secondsPerClip)),
          callback(std::move(finished))
    {
    }

    JobStatus runJob() override
    {
        PreparedClipArray prepared;
        juce::StringArray errors;
        auto appFolder = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                             .getParentDirectory();
        auto ffmpeg = appFolder.getChildFile("ffmpeg.exe");

        if (!source.existsAsFile())
            errors.add("Vidéo source introuvable.");
        else if (!ffmpeg.existsAsFile())
            errors.add("ffmpeg.exe absent du dossier du logiciel.");
        else
        {
            auto cacheRoot = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("MIDI Video Player").getChildFile("ClipCache");
            cacheRoot.createDirectory();

            // Version the cache so old 30 fps / 720p files are never mistaken
            // for the fully-preloaded 24 fps / 854x480 image sets.
            juce::String cacheKey = "frames_v2_24fps_854x480_"
                                  + juce::String::toHexString(source.getFullPathName().hashCode64())
                                  + "_" + juce::String::toHexString(source.getSize())
                                  + "_" + juce::String(source.getLastModificationTime().toMilliseconds())
                                  + "_" + juce::String((int) (clipDuration * 1000.0));
            for (double start : markerStarts)
                cacheKey += "_" + juce::String(start, 3);

            auto projectCache = cacheRoot.getChildFile(cacheKey);
            projectCache.createDirectory();

            int readyCount = 0;
            for (int i = 0; i < 12 && !shouldExit(); ++i)
            {
                if (markerStarts[(size_t) i] < 0.0)
                    continue;

                auto noteFolder = projectCache.getChildFile("note_" + juce::String(i));
                auto completeFlag = noteFolder.getChildFile("complete.txt");
                juce::Array<juce::File> found;
                noteFolder.findChildFiles(found, juce::File::findFiles, false, "*.jpg");

                if (!completeFlag.existsAsFile() || found.isEmpty())
                {
                    noteFolder.deleteRecursively();
                    noteFolder.createDirectory();

                    auto outputPattern = noteFolder.getChildFile("frame_%05d.jpg");
                    juce::StringArray args;
                    args.add(ffmpeg.getFullPathName());
                    args.add("-hide_banner");
                    args.add("-loglevel"); args.add("error");
                    args.add("-threads"); args.add("1");
                    args.add("-y");
                    args.add("-ss"); args.add(juce::String(markerStarts[(size_t) i], 3));
                    args.add("-i"); args.add(source.getFullPathName());
                    args.add("-t"); args.add(juce::String(clipDuration, 2));
                    args.add("-an");
                    args.add("-vf");
                    args.add("fps=24,scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:(ow-iw)/2:(oh-ih)/2,setsar=1");
                    args.add("-q:v"); args.add("5");
                    args.add("-start_number"); args.add("0");
                    args.add(outputPattern.getFullPathName());

                    juce::ChildProcess process;
                    if (!process.start(args))
                    {
                        errors.add("FFmpeg n'a pas pu démarrer pour la note " + juce::String(notes[(size_t) i]) + ".");
                        continue;
                    }

                    while (!process.waitForProcessToFinish(200) && !shouldExit()) {}
                    if (shouldExit())
                    {
                        process.kill();
                        process.waitForProcessToFinish(1000);
                        return jobHasFinished;
                    }
                }

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

                auto& clip = prepared[(size_t) i];
                for (const auto& file : frameFiles)
                {
                    if (shouldExit())
                        return jobHasFinished;
                    auto image = juce::ImageFileFormat::loadFrom(file);
                    if (image.isValid())
                        clip.frames.push_back(std::move(image));
                }

                if (!clip.frames.empty())
                {
                    clip.sourceStart = markerStarts[(size_t) i];
                    completeFlag.replaceWithText("ready");
                    ++readyCount;
                }
                else
                {
                    errors.add("Aucune image extraite pour la note " + juce::String(notes[(size_t) i]) + ".");
                }
            }

            if (readyCount > 0)
                errors.insert(0, "Clips prêts : " + juce::String(readyCount) + " / 12");
            else if (errors.isEmpty())
                errors.add("Aucun clip à préparer : ajoute des marqueurs.");
        }

        if (callback && !shouldExit())
        {
            auto finished = std::move(callback);
            juce::MessageManager::callAsync(
                [finished = std::move(finished),
                 data = std::move(prepared),
                 message = errors.joinIntoString("  |  ")]() mutable
                {
                    finished(std::move(data), message);
                });
        }
        return jobHasFinished;
    }

private:
    juce::File source;
    std::array<double, 12> markerStarts{};
    double clipDuration = 3.0;
    std::function<void(PreparedClipArray, juce::String)> callback;
};

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
        clipPlayer.setVisible(false);
        sourceVideo.setVisible(true);
        sourceVideo.setBlackoutAlpha(0);
        sourceVideo.play();
        clipMode = false;
        return sourceVideo.playbackDiagnostic();
    }

    void pause()
    {
        if (clipMode)
            clipPlayer.pause();
        else
            sourceVideo.pause();
    }

    void stop()
    {
        sourceVideo.stop();
        clipPlayer.stop();
    }

    bool isPlaying() const
    {
        return clipMode ? clipPlayer.isPlaying() : sourceVideo.isPlaying();
    }

    void seek(double seconds)
    {
        syncFile();
        clipPlayer.stop();
        clipPlayer.setVisible(false);
        sourceVideo.setVisible(true);
        sourceVideo.setBlackoutAlpha(0);
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

    void setPreparedClips(PreparedClipArray clips, double clipSeconds)
    {
        clipPlayer.setClips(std::move(clips), clipSeconds);
    }

    void triggerClip(int pitchClass)
    {
        syncFile();
        sourceVideo.pause();
        sourceVideo.setVisible(false);
        clipPlayer.setVisible(true);
        clipMode = true;
        clipPlayer.trigger(pitchClass);
    }

    void clearPreparedClips()
    {
        clipPlayer.setClips({}, 3.0);
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
            clipPlayer.setVisible(false);
            clipMode = false;
        }
    }

    void timerCallback() override
    {
        syncFile();
    }

    void handleAsyncUpdate() override
    {
        std::deque<int> triggers;
        {
            const juce::ScopedLock sl(state->lock);
            triggers.swap(state->pendingTriggers);
        }

        // The newest MIDI hit wins within one UI dispatch. All clip frames are
        // already decoded in memory, so this is only an image selection.
        if (!triggers.empty())
            clipPlayer.trigger(triggers.back());
    }

    std::shared_ptr<VideoState> state;
    vlcplayer::VideoView sourceVideo;
    ClipFramePlayer clipPlayer;
    juce::File loadedFile;
    bool loadSucceeded = false;
    bool clipMode = false;
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
    void setPreparedClips(PreparedClipArray clips, double seconds) { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->setPreparedClips(std::move(clips), seconds); }
    void clearPreparedClips() { if (auto* v = dynamic_cast<VideoOutputView*>(getContentComponent())) v->clearPreparedClips(); }
};

class MainView final : public juce::Component, private juce::Timer {
public:
    MainView():state(std::make_shared<VideoState>()),router(state),timeline(state){
        juce::PropertiesFile::Options o;o.applicationName="MIDI Video Player";o.filenameSuffix="settings";o.folderName="MIDI Video Player";
        settings=std::make_unique<juce::PropertiesFile>(o);
        auto path=settings->getValue("video");
        clipDuration = juce::jlimit(1.0, 8.0, settings->getDoubleValue("clipDuration", 3.0));
        for(int i=0;i<12;++i){auto key="markers"+juce::String(i);state->markers[(size_t)i]=parseMarkers(settings->getValue(key));}
        if(path.isNotEmpty()){juce::File f(path);if(f.existsAsFile())loadVideo(f);}

        addAndMakeVisible(timeline);videoStatus.setColour(juce::Label::textColourId,juce::Colour(0xff39ff14));videoStatus.setText("Aucune vidéo chargée",juce::dontSendNotification);
        setLookAndFeel(&readableLookAndFeel);
        addAndMakeVisible(newProject);addAndMakeVisible(openProject);addAndMakeVisible(saveProject);addAndMakeVisible(saveAsProject);
        addAndMakeVisible(openVideo);addAndMakeVisible(play);addAndMakeVisible(addMarker);addAndMakeVisible(eraseMarker);addAndMakeVisible(noteChoice);addAndMakeVisible(clipDurationChoice);addAndMakeVisible(videoStatus);
        newProject.setButtonText("Nouveau projet");newProject.onClick=[this]{newProjectAction();};
        openProject.setButtonText("Ouvrir projet");openProject.onClick=[this]{chooseProjectToOpen();};
        saveProject.setButtonText("Enregistrer");saveProject.onClick=[this]{saveProjectAction();};
        saveAsProject.setButtonText("Enregistrer sous");saveAsProject.onClick=[this]{chooseProjectToSave();};
        for(auto* b:{&newProject,&openProject,&saveProject,&saveAsProject,&openVideo,&play,&addMarker,&eraseMarker,&refresh,&outputButton}) {
            b->setColour(juce::TextButton::buttonColourId,juce::Colour(0xff101610));
            b->setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xff39ff14).withAlpha(0.22f));
            b->setColour(juce::TextButton::textColourOffId,juce::Colour(0xfff4fff2));
            b->setColour(juce::TextButton::textColourOnId,juce::Colour(0xff39ff14));
        }
        for(auto* box:{&noteChoice,&clipDurationChoice,&inputBox,&outputBox}) {
            box->setColour(juce::ComboBox::backgroundColourId,juce::Colour(0xff101610));
            box->setColour(juce::ComboBox::textColourId,juce::Colour(0xfff4fff2));
            box->setColour(juce::ComboBox::outlineColourId,juce::Colour(0xff39ff14).withAlpha(0.8f));
            box->setColour(juce::ComboBox::arrowColourId,juce::Colour(0xff39ff14));
        }
        addAndMakeVisible(inputLabel);addAndMakeVisible(outputLabel);addAndMakeVisible(inputBox);addAndMakeVisible(outputBox);addAndMakeVisible(refresh);
        addAndMakeVisible(outputButton);
        openVideo.setButtonText("Ouvrir une vidéo");openVideo.onClick=[this]{chooseVideo();};
        play.setButtonText("Lire / Pause");play.onClick=[this]{ensureOutputWindow();if(outputWindow->isVideoPlaying()){outputWindow->pauseVideo();videoStatus.setText("Lecture en pause",juce::dontSendNotification);}else{outputWindow->setVisible(true);outputWindow->toFront(true);auto result=outputWindow->playVideo();videoStatus.setText(result,juce::dontSendNotification);}};
        addMarker.setButtonText("Ajouter un repère à la position");addMarker.onClick=[this]{addCurrentMarker();};
        eraseMarker.setButtonText("Gomme désactivée");eraseMarker.setClickingTogglesState(true);eraseMarker.onClick=[this]{bool on=eraseMarker.getToggleState();eraseMarker.setButtonText(on?"Gomme activée : cliquer un repère":"Gomme désactivée");timeline.setEraseMode(on);};
        for(int i=0;i<12;++i)noteChoice.addItem(notes[(size_t)i],i+1);
        noteChoice.setSelectedId(1);
        for(int seconds=1;seconds<=8;++seconds)
            clipDurationChoice.addItem("Clip " + juce::String(seconds) + " s", seconds);
        clipDurationChoice.setSelectedId((int) clipDuration, juce::dontSendNotification);
        clipDurationChoice.onChange=[this]{
            clipDuration = (double) clipDurationChoice.getSelectedId();
            settings->setValue("clipDuration", clipDuration);
            settings->saveIfNeeded();
            rebuildClipCache();
            markProjectDirty();
        };
        timeline.changed=[this]{saveMarkers();};
        timeline.seeked=[this](double t){if(outputWindow)outputWindow->seekVideo(t);};
        inputLabel.setText("Entrée MIDI",juce::dontSendNotification);outputLabel.setText("Sortie MIDI (loopMIDI)",juce::dontSendNotification);

        refresh.setButtonText("Actualiser");refresh.onClick=[this]{refreshMidi();};inputBox.onChange=[this]{selectInput();};outputBox.onChange=[this]{selectOutput();};
        outputButton.setButtonText("Ouvrir la sortie vidéo OBS");outputButton.onClick=[this]{ensureOutputWindow();outputWindow->setVisible(true);outputWindow->toFront(true);};
        refreshMidi();setSize(1120,850);
        auto recovery=recoveryProjectFile();
        if(recovery.existsAsFile() && recovery.getSize()>0) loadProjectFile(recovery);
        else {
            auto last=settings->getValue("lastProject");
            if(last.isNotEmpty() && juce::File(last).existsAsFile()) loadProjectFile(juce::File(last));
        }
        startTimerHz(10);
    }
    ~MainView() override {outputWindow.reset();setLookAndFeel(nullptr);if(settings)settings->saveIfNeeded();}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff050705));
        g.setColour(juce::Colour(0xff39ff14));
        g.setFont(ReadableLookAndFeel::uiFont(22.0f, juce::Font::bold));
        g.drawText("MIDI VIDEO PLAYER",24,12,getWidth()-48,30,juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xfff4fff2));
        g.setFont(ReadableLookAndFeel::uiFont(13.0f));
        g.drawText("Place les marqueurs sur les 12 pistes, puis déclenche-les avec ton clavier MIDI.",24,42,getWidth()-48,22,juce::Justification::centredLeft);
    }
    void setWaveformData(std::vector<float> p) { timeline.setWaveform(std::move(p)); }
    void resized() override {
        auto a=getLocalBounds().reduced(22);a.removeFromTop(72);
        auto projectBar=a.removeFromTop(38);
        newProject.setBounds(projectBar.removeFromLeft(145));projectBar.removeFromLeft(7);
        openProject.setBounds(projectBar.removeFromLeft(130));projectBar.removeFromLeft(7);
        saveProject.setBounds(projectBar.removeFromLeft(115));projectBar.removeFromLeft(7);
        saveAsProject.setBounds(projectBar.removeFromLeft(145));
        a.removeFromTop(8);
        auto top=a.removeFromTop(180);auto left=top.removeFromLeft(250);videoStatus.setBounds(left.removeFromTop(44));auto controls=top.reduced(12,0);
        openVideo.setBounds(controls.removeFromTop(36));controls.removeFromTop(8);play.setBounds(controls.removeFromTop(34));controls.removeFromTop(8);
        inputLabel.setBounds(controls.removeFromTop(20));inputBox.setBounds(controls.removeFromTop(28));controls.removeFromTop(5);
        outputLabel.setBounds(controls.removeFromTop(20));outputBox.setBounds(controls.removeFromTop(28));refresh.setBounds(controls.removeFromTop(30));
        a.removeFromTop(8);auto timelineHead=a.removeFromTop(32);addMarker.setBounds(timelineHead.removeFromLeft(245));timelineHead.removeFromLeft(8);eraseMarker.setBounds(timelineHead.removeFromLeft(180));timelineHead.removeFromLeft(8);noteChoice.setBounds(timelineHead.removeFromLeft(100));timelineHead.removeFromLeft(8);clipDurationChoice.setBounds(timelineHead.removeFromLeft(105));
        timeline.setBounds(a.removeFromTop(310));a.removeFromTop(8);
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
            waveformJobs.addJob(new WaveformJob(f,[safe](std::vector<float> p){if(safe) safe->setWaveformData(std::move(p));}),true);
            ensureOutputWindow();
            outputWindow->setVisible(true);outputWindow->toFront(true);
            videoStatus.setText("Vidéo source chargée : "+f.getFileName()+" | Préparation des clips...",juce::dontSendNotification);
            settings->setValue("video",f.getFullPathName());settings->saveIfNeeded();
            rebuildClipCache();
        }else videoStatus.setText("Fichier vidéo introuvable : "+f.getFullPathName(),juce::dontSendNotification);
    }
    void rebuildClipCache()
    {
        juce::File source;
        std::array<double, 12> starts;
        starts.fill(-1.0);
        {
            const juce::ScopedLock sl(state->lock);
            source = state->file;
            for (int i = 0; i < 12; ++i)
                if (!state->markers[(size_t) i].empty())
                    starts[(size_t) i] = state->markers[(size_t) i].front();
        }

        ++clipCacheGeneration;
        const int generation = clipCacheGeneration;
        clipJobs.removeAllJobs(true, 0);
        ensureOutputWindow();
        // Prevent MIDI notes from triggering clips belonging to the previous
        // marker layout while the refreshed cache is being rendered.
        outputWindow->clearPreparedClips();

        if (!source.existsAsFile())
        {
            outputWindow->clearPreparedClips();
            videoStatus.setText("Aucune vidéo à préparer.", juce::dontSendNotification);
            return;
        }

        videoStatus.setText("Préparation des clips MIDI en arrière-plan...", juce::dontSendNotification);
        juce::Component::SafePointer<MainView> safe(this);
        clipJobs.addJob(new ClipCacheJob(source, starts, clipDuration,
            [safe, generation, seconds = clipDuration](PreparedClipArray clips, juce::String message) mutable
            {
                if (!safe || safe->clipCacheGeneration != generation)
                    return;
                int count = 0;
                for (const auto& clip : clips)
                    if (!clip.frames.empty())
                        ++count;
                safe->ensureOutputWindow();
                safe->outputWindow->setPreparedClips(std::move(clips), seconds);
                safe->videoStatus.setText(message, juce::dontSendNotification);
                if (count == 0)
                    safe->videoStatus.setText(message + " | Vérifie que FFmpeg et les marqueurs sont disponibles.", juce::dontSendNotification);
            }), true);
    }

    void ensureOutputWindow(){if(!outputWindow)outputWindow=std::make_unique<OutputWindow>(state);}
    juce::File recoveryProjectFile() const {
        auto dir=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MIDI Video Player");
        dir.createDirectory();return dir.getChildFile("Recovery.mvproj");
    }
    juce::var makeProjectData(){
        auto* obj=new juce::DynamicObject();juce::var root(obj);
        const juce::ScopedLock sl(state->lock);
        obj->setProperty("formatVersion",2);
        obj->setProperty("video",state->file.getFullPathName());
        obj->setProperty("clipDuration", clipDuration);
        obj->setProperty("midiInput",settings->getValue("input"));
        obj->setProperty("midiOutput",settings->getValue("output"));
        juce::Array<juce::var> all;
        for(int i=0;i<12;++i) all.add(markerString(state->markers[(size_t)i]));
        obj->setProperty("markers",all);
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
                                           ? (double) obj->getProperty("clipDuration") : 3.0;
        clipDuration = juce::jlimit(1.0, 8.0, savedClipDuration);
        if (clipDurationChoice.getSelectedId() != (int) clipDuration)
            clipDurationChoice.setSelectedId((int) clipDuration, juce::dontSendNotification);
        settings->setValue("clipDuration", clipDuration);
        juce::String videoPath;
        {
            const juce::ScopedLock sl(state->lock);
            for(int i=0;i<12;++i) state->markers[(size_t)i]=(markers.isArray()&&i<markers.size())?parseMarkers(markers[i].toString()):std::vector<double>{};
            videoPath=obj->getProperty("video").toString();
            state->playhead=0.0;
        }
        settings->setValue("input",obj->getProperty("midiInput").toString());settings->setValue("output",obj->getProperty("midiOutput").toString());
        settings->saveIfNeeded();currentProject=(file==recoveryProjectFile()?juce::File{}:file);projectDirty=false;
        auto videoFile=juce::File(videoPath);
        if(videoFile.existsAsFile()) loadVideo(videoFile);
        else {
            {const juce::ScopedLock sl(state->lock);state->file=videoFile;}
            rebuildClipCache();
            videoStatus.setText("Vidéo introuvable : "+videoPath,juce::dontSendNotification);
        }
        timeline.setDuration(state->duration>0.0?state->duration:60.0);timeline.repaint();refreshMidi();
        if(videoPath.isEmpty()) videoStatus.setText("Projet chargé : aucune vidéo associée",juce::dontSendNotification);
        else if(!videoFile.existsAsFile()) videoStatus.setText("Projet chargé, vidéo introuvable : "+videoPath,juce::dontSendNotification);
        else videoStatus.setText("Projet chargé : "+file.getFileName(),juce::dontSendNotification);
    }
    void newProjectAction(){
        {const juce::ScopedLock sl(state->lock);for(auto& m:state->markers)m.clear();state->file={};state->duration=0;state->playhead=0;state->pendingTriggers.clear();}
        if(outputWindow){outputWindow->stopVideo();outputWindow->clearPreparedClips();}
        ++clipCacheGeneration;
        clipJobs.removeAllJobs(true, 1000);
        {const juce::ScopedLock sl(state->lock);state->file={};state->duration=0.0;state->playhead=0.0;}
        timeline.setDuration(60.0);timeline.repaint();currentProject={};projectDirty=true;
        videoStatus.setText("Nouveau projet",juce::dontSendNotification);writeRecoveryProject();
    }
    void addCurrentMarker(){int p=noteChoice.getSelectedId()-1;if(p<0||p>=12)return;double t=outputWindow?outputWindow->videoPosition():0.0;{const juce::ScopedLock sl(state->lock);t=juce::jlimit(0.0,state->duration,t);auto& v=state->markers[(size_t)p];v.clear();v.push_back(t);}saveMarkers();markProjectDirty();timeline.repaint();}
    void saveMarkers(){
        {
            const juce::ScopedLock sl(state->lock);
            for(int i=0;i<12;++i)
                settings->setValue("markers"+juce::String(i),markerString(state->markers[(size_t)i]));
        }
        settings->saveIfNeeded();
        markProjectDirty();
        rebuildClipCache();
    }
    void refreshMidi(){ins=router.inputs();outs=router.outputs();inputBox.clear();outputBox.clear();inputBox.addItem("Choisir une entrée...",1);outputBox.addItem("Choisir une sortie...",1);for(int i=0;i<ins.size();++i)inputBox.addItem(ins.getReference(i).name,i+2);for(int i=0;i<outs.size();++i)outputBox.addItem(outs.getReference(i).name,i+2);
        auto si=settings->getValue("input"),so=settings->getValue("output");for(int i=0;i<ins.size();++i)if(ins.getReference(i).identifier==si)inputBox.setSelectedId(i+2,juce::dontSendNotification);for(int i=0;i<outs.size();++i)if(outs.getReference(i).identifier==so)outputBox.setSelectedId(i+2,juce::dontSendNotification);}
    void selectInput(){int i=inputBox.getSelectedId()-2;if(i>=0&&i<ins.size()){auto d=ins.getReference(i);router.openInput(d.identifier);settings->setValue("input",d.identifier);settings->saveIfNeeded();}}
    void selectOutput(){int i=outputBox.getSelectedId()-2;if(i>=0&&i<outs.size()){auto d=outs.getReference(i);router.openOutput(d.identifier);settings->setValue("output",d.identifier);settings->saveIfNeeded();}}
    std::shared_ptr<VideoState> state;MidiRouter router;MarkerTimeline timeline;juce::ThreadPool waveformJobs{1};juce::ThreadPool clipJobs{1};
    double clipDuration = 3.0;
    int clipCacheGeneration = 0;
    ReadableLookAndFeel readableLookAndFeel;
    juce::TextButton newProject,openProject,saveProject,saveAsProject;
    juce::File currentProject;bool projectDirty=false;juce::uint32 nextAutoSaveMs=0;std::unique_ptr<juce::PropertiesFile> settings;std::unique_ptr<juce::FileChooser> chooser;std::unique_ptr<OutputWindow> outputWindow;
    juce::TextButton openVideo,play,addMarker,eraseMarker,refresh,outputButton;juce::ComboBox noteChoice,clipDurationChoice,inputBox,outputBox;juce::Label inputLabel,outputLabel,videoStatus;juce::Array<juce::MidiDeviceInfo> ins,outs;
};

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow():DocumentWindow("MIDI Video Player",juce::Colour(0xff050705),allButtons){setUsingNativeTitleBar(true);setContentOwned(new MainView(),true);setResizable(true,true);centreWithSize(1120,850);}
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
