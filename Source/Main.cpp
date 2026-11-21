#include <JuceHeader.h>
#include "LibVlcPlayer.h"
#include <array>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <cmath>

namespace {
class ReadableLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return juce::Font(16.0f, juce::Font::bold); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(15.0f); }
    juce::Font getLabelFont(juce::Label&) override { return juce::Font(15.0f); }
};
const std::array<const char*,12> notes{"Do","Do#","Ré","Ré#","Mi","Fa","Fa#","Sol","Sol#","La","La#","Si"};
struct VideoState {
    juce::CriticalSection lock;
    juce::File file;
    std::array<std::vector<double>,12> markers;
    double duration=0.0, playhead=0.0;
    bool held=false;
    int activeMidiNote=-1;
    std::array<int,128> activeNoteCounts{};
    double releaseMs=0.0;
    std::deque<double> pendingTriggers;
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
        if(output) output->sendMessageNow(m);
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
                    state->activeMidiNote=midiNote;
                    state->activeNoteCounts[(size_t)midiNote]++;
                    state->held=true;
                    state->releaseMs=0.0;
                    state->playhead=chosen;
                    state->pendingTriggers.clear();
                    state->pendingTriggers.push_back(chosen);
                    dispatch=true;
                }
            } else if(m.isNoteOff()) {
                const int midiNote=m.getNoteNumber();
                if(midiNote>=0 && midiNote<128 && state->activeNoteCounts[(size_t)midiNote]>0)
                    --state->activeNoteCounts[(size_t)midiNote];
                if(state->activeMidiNote==midiNote && state->activeNoteCounts[(size_t)midiNote]==0) {
                    state->activeMidiNote=-1;
                    state->held=false;
                    state->releaseMs=juce::Time::getMillisecondCounterHiRes();
                    dispatch=true;
                }
            }
            if(dispatch) notify=state->dispatchPlayback;
        }
        if(notify) notify();
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
        g.fillAll(juce::Colour(0xff171a20));
        auto r=getLocalBounds().toFloat(); float left=48.0f, right=12.0f, top=25.0f;
        g.setColour(juce::Colour(0xffc0c8d2)); g.setFont(12.0f);
        g.drawText("Zoom x"+juce::String(zoom,1)+"   |   Molette : zoom   |   Maj + molette : déplacement",48,2,(int)r.getWidth()-60,19,juce::Justification::centredLeft);
        const float waveH=54.0f;
        const float trackArea=r.getHeight()-top-waveH-5.0f;
        float trackH=trackArea/12.0f;
        g.setFont(12.0f);
        for(int i=0;i<12;++i){
            float y=top+i*trackH;
            g.setColour(i%2?juce::Colour(0xff20252d):juce::Colour(0xff1c2128));
            g.fillRect(left,y,r.getWidth()-left-right,trackH-1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(notes[(size_t)i],2,(int)y,42,(int)trackH,juce::Justification::centredLeft);
            const juce::ScopedLock sl(state->lock);
            for(double t:state->markers[(size_t)i]){
                float x=timeToX(t,left,r.getWidth()-left-right);
                g.setColour(juce::Colour::fromHSV((float)i/12.0f,0.72f,0.98f,1.0f));
                juce::Path p;p.addTriangle(x,y+3,x-5,y+trackH-4,x+5,y+trackH-4);g.fillPath(p);
                g.drawLine(x,y+trackH-5,x,y+trackH-1,1.0f);
            }
        }
        const float waveY=top+trackArea+5.0f;
        g.setColour(juce::Colour(0xff111820));
        g.fillRoundedRectangle(left,waveY,r.getWidth()-left-right,waveH-2.0f,4.0f);
        g.setColour(juce::Colour(0xff65c6d8).withAlpha(0.9f));
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
        g.setColour(juce::Colours::white.withAlpha(0.45f));
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
        dragRow=row; dragIndex=-1; double best=0.45;
        {const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)row];
         for(size_t i=0;i<v.size();++i){double dx=std::abs(v[i]-clicked);if(dx<best){best=dx;dragIndex=(int)i;}}}
        if(eraseMode){
            if(dragIndex>=0){const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)row];if(dragIndex<(int)v.size())v.erase(v.begin()+dragIndex);if(changed)changed();}
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
            const double ratio=juce::jlimit(0.0,1.0,(e.position.x-left)/juce::jmax(1.0f,(float)getWidth()-left-right));
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
        std::vector<float> peaks(1800, 0.0f);
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
                        for(int i=0;i<count;++i) { int bin=juce::jmin(1799,(int)(((offset+i)*1800)/total)); peaks[(size_t)bin]=juce::jmax(peaks[(size_t)bin],std::abs(buffer.getSample(0,i))); }
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
class VideoOutputView final : public juce::Component, private juce::Timer, private juce::AsyncUpdater {
public:
    explicit VideoOutputView(std::shared_ptr<VideoState> s):state(std::move(s)){
        addAndMakeVisible(video);
        juce::Component::SafePointer<VideoOutputView> safe(this);
        { const juce::ScopedLock sl(state->lock); state->dispatchPlayback=[safe]() { if(safe) safe->triggerAsyncUpdate(); }; }
        startTimerHz(30);
    }
    ~VideoOutputView() override {
        cancelPendingUpdate();
        const juce::ScopedLock sl(state->lock);
        state->dispatchPlayback={};
    }
    void resized() override {video.setBounds(getLocalBounds());}
    juce::String play() {
        syncFile();
        if (!loadedFile.existsAsFile()) return "Aucune vidéo chargée dans la Sortie OBS.";
        if (!video.isAvailable()) return video.errorMessage();
        video.setBlackoutAlpha(0);
        video.play();
        return video.playbackDiagnostic();
    }
    void pause() { video.pause(); }
    void stop() { video.stop(); }
    bool isPlaying() const { return video.isPlaying(); }
    void seek(double seconds) { video.setPlayPosition(seconds); }
    double position() const { return video.getPlayPosition(); }
    double duration() const { return video.getVideoDuration(); }
    juce::String diagnostic() const { return video.playbackDiagnostic(); }
private:
    void syncFile() {
        juce::File file;
        { const juce::ScopedLock sl(state->lock); file = state->file; }
        if (file != loadedFile) {
            video.stop();
            video.unload();
            loadedFile = file;
            loadSucceeded = file.existsAsFile() && video.load(file);
            if (loadSucceeded) {
                video.setPlayPosition(0.0);
                video.setBlackoutAlpha(0);
            } else {
                video.setBlackoutAlpha(0);
            }
        }
    }
    void timerCallback() override { syncFile(); }
    void handleAsyncUpdate() override {
        std::deque<double> triggers;
        bool held=false;
        double release=0.0;
        {
            const juce::ScopedLock sl(state->lock);
            triggers.swap(state->pendingTriggers);
            held=state->held;
            release=state->releaseMs;
        }
        if(!triggers.empty()) {
            const double start=triggers.back();
            video.setPlayPosition(start);
            video.setBlackoutAlpha(0);
            video.play();
        }
        if(!held && release>0.0) {
            video.stop();
            const juce::ScopedLock sl(state->lock);
            state->releaseMs=0.0;
        }
    }
    std::shared_ptr<VideoState> state;vlcplayer::VideoView video;juce::File loadedFile;bool loadSucceeded=false;
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
};

class MainView final : public juce::Component, private juce::Timer {
public:
    MainView():state(std::make_shared<VideoState>()),router(state),timeline(state){
        juce::PropertiesFile::Options o;o.applicationName="MIDI Video Player";o.filenameSuffix="settings";o.folderName="MIDI Video Player";
        settings=std::make_unique<juce::PropertiesFile>(o);
        auto path=settings->getValue("video");if(path.isNotEmpty()){juce::File f(path);if(f.existsAsFile())loadVideo(f);}
        for(int i=0;i<12;++i){auto key="markers"+juce::String(i);state->markers[(size_t)i]=parseMarkers(settings->getValue(key));}

        addAndMakeVisible(timeline);videoStatus.setColour(juce::Label::textColourId,juce::Colours::white);videoStatus.setText("Aucune vidéo chargée",juce::dontSendNotification);
        setLookAndFeel(&readableLookAndFeel);
        addAndMakeVisible(newProject);addAndMakeVisible(openProject);addAndMakeVisible(saveProject);addAndMakeVisible(saveAsProject);
        addAndMakeVisible(openVideo);addAndMakeVisible(play);addAndMakeVisible(addMarker);addAndMakeVisible(eraseMarker);addAndMakeVisible(noteChoice);addAndMakeVisible(videoStatus);
        newProject.setButtonText("Nouveau projet");newProject.onClick=[this]{newProjectAction();};
        openProject.setButtonText("Ouvrir projet");openProject.onClick=[this]{chooseProjectToOpen();};
        saveProject.setButtonText("Enregistrer");saveProject.onClick=[this]{saveProjectAction();};
        saveAsProject.setButtonText("Enregistrer sous");saveAsProject.onClick=[this]{chooseProjectToSave();};
        for(auto* b:{&newProject,&openProject,&saveProject,&saveAsProject,&openVideo,&play,&addMarker,&eraseMarker,&refresh,&outputButton}) b->setColour(juce::TextButton::buttonColourId,juce::Colour(0xff354252));
        addAndMakeVisible(inputLabel);addAndMakeVisible(outputLabel);addAndMakeVisible(inputBox);addAndMakeVisible(outputBox);addAndMakeVisible(refresh);
        addAndMakeVisible(outputButton);
        openVideo.setButtonText("Ouvrir une vidéo");openVideo.onClick=[this]{chooseVideo();};
        play.setButtonText("Lire / Pause");play.onClick=[this]{ensureOutputWindow();if(outputWindow->isVideoPlaying()){outputWindow->pauseVideo();videoStatus.setText("Lecture en pause",juce::dontSendNotification);}else{outputWindow->setVisible(true);outputWindow->toFront(true);auto result=outputWindow->playVideo();videoStatus.setText("Diagnostic VLC : "+result,juce::dontSendNotification);}};
        addMarker.setButtonText("Ajouter un repère à la position");addMarker.onClick=[this]{addCurrentMarker();};
        eraseMarker.setButtonText("Gomme désactivée");eraseMarker.setClickingTogglesState(true);eraseMarker.onClick=[this]{bool on=eraseMarker.getToggleState();eraseMarker.setButtonText(on?"Gomme activée : cliquer un repère":"Gomme désactivée");timeline.setEraseMode(on);};
        for(int i=0;i<12;++i)noteChoice.addItem(notes[(size_t)i],i+1);
        noteChoice.setSelectedId(1);
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
    void paint(juce::Graphics& g) override {g.fillAll(juce::Colour(0xff202329));g.setColour(juce::Colours::white);g.setFont(juce::Font(22,juce::Font::bold));g.drawText("MIDI VIDEO PLAYER",24,12,getWidth()-48,30,juce::Justification::centredLeft);g.setColour(juce::Colour(0xffb4bdc9));g.setFont(13);g.drawText("Place les marqueurs sur les 12 pistes, puis déclenche-les avec ton clavier MIDI.",24,42,getWidth()-48,22,juce::Justification::centredLeft);}
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
        a.removeFromTop(8);auto timelineHead=a.removeFromTop(32);addMarker.setBounds(timelineHead.removeFromLeft(245));timelineHead.removeFromLeft(8);eraseMarker.setBounds(timelineHead.removeFromLeft(180));timelineHead.removeFromLeft(8);noteChoice.setBounds(timelineHead.removeFromLeft(110));
        timeline.setBounds(a.removeFromTop(310));a.removeFromTop(8);
        outputButton.setBounds(a.removeFromTop(38).withWidth(250));
    }
private:
    double lastTimelineDuration=0.0;
    bool timelineDurationNeedsUpdate(double duration) const { return std::abs(duration - lastTimelineDuration) > 0.02; }
    void markProjectDirty(){projectDirty=true;nextAutoSaveMs=juce::Time::getMillisecondCounter()+1000;}
    void timerCallback() override {
        if(outputWindow && outputWindow->isVisible() && outputWindow->isVideoPlaying()){
            double p=outputWindow->videoPosition();
            double duration=outputWindow->videoDuration();
            {
                const juce::ScopedLock sl(state->lock);
                state->playhead=p;
                if(duration>0.0 && state->duration<=0.0) state->duration=duration;
            }
            if(duration>0.0 && timelineDurationNeedsUpdate(duration)){lastTimelineDuration=duration;timeline.setDuration(duration);}
            timeline.repaint();
        }
        else {
            juce::File currentVideo;
            { const juce::ScopedLock sl(state->lock); currentVideo = state->file; }
            if (currentVideo.existsAsFile() && videoStatus.getText().startsWith("Diagnostic VLC :"))
                videoStatus.setText(outputWindow ? outputWindow->videoDiagnostic() : "Sortie OBS non ouverte", juce::dontSendNotification);
        }
        if(projectDirty && juce::Time::getMillisecondCounter() >= nextAutoSaveMs){
            writeRecoveryProject();projectDirty=false;nextAutoSaveMs=juce::Time::getMillisecondCounter()+3000;
        }
    }
    void chooseVideo(){chooser=std::make_unique<juce::FileChooser>("Choisir une vidéo",juce::File(),"*.mp4;*.mov;*.m4v;*.avi;*.wmv");chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& c){auto f=c.getResult();if(f.existsAsFile())loadVideo(f);chooser.reset();});}
    void loadVideo(const juce::File& f){
        videoStatus.setText("Chargement de : "+f.getFileName(),juce::dontSendNotification);
        if(f.existsAsFile()){
            {const juce::ScopedLock sl(state->lock);state->file=f;state->duration=0.0;state->playhead=0.0;state->held=false;state->activeMidiNote=-1;state->activeNoteCounts.fill(0);state->releaseMs=0.0;state->pendingTriggers.clear();}
            timeline.setDuration(60.0);timeline.setWaveform({});timeline.repaint();
            juce::Component::SafePointer<MainView> safe(this);
            waveformJobs.addJob(new WaveformJob(f,[safe](std::vector<float> p){if(safe) safe->setWaveformData(std::move(p));}),true);
            ensureOutputWindow();
            outputWindow->setVisible(true);outputWindow->toFront(true);
            videoStatus.setText("Vidéo chargée dans la Sortie OBS : "+f.getFileName(),juce::dontSendNotification);
            settings->setValue("video",f.getFullPathName());settings->saveIfNeeded();
        }else videoStatus.setText("Fichier vidéo introuvable : "+f.getFullPathName(),juce::dontSendNotification);
    }
    void ensureOutputWindow(){if(!outputWindow)outputWindow=std::make_unique<OutputWindow>(state);}
    juce::File recoveryProjectFile() const {
        auto dir=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MIDI Video Player");
        dir.createDirectory();return dir.getChildFile("Recovery.mvproj");
    }
    juce::var makeProjectData(){
        auto* obj=new juce::DynamicObject();juce::var root(obj);
        const juce::ScopedLock sl(state->lock);
        obj->setProperty("formatVersion",1);
        obj->setProperty("video",state->file.getFullPathName());
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
        juce::String videoPath;
        {
            const juce::ScopedLock sl(state->lock);
            for(int i=0;i<12;++i) state->markers[(size_t)i]=(markers.isArray()&&i<markers.size())?parseMarkers(markers[i].toString()):std::vector<double>{};
            videoPath=obj->getProperty("video").toString();
            state->playhead=0.0;state->held=false;state->activeMidiNote=-1;
        }
        settings->setValue("input",obj->getProperty("midiInput").toString());settings->setValue("output",obj->getProperty("midiOutput").toString());
        settings->saveIfNeeded();currentProject=(file==recoveryProjectFile()?juce::File{}:file);projectDirty=false;
        auto videoFile=juce::File(videoPath);
        if(videoFile.existsAsFile()) loadVideo(videoFile); else {const juce::ScopedLock sl(state->lock);state->file=videoFile;videoStatus.setText("Vidéo introuvable : "+videoPath,juce::dontSendNotification);}
        timeline.setDuration(state->duration>0.0?state->duration:60.0);timeline.repaint();refreshMidi();
        if(videoPath.isEmpty()) videoStatus.setText("Projet chargé : aucune vidéo associée",juce::dontSendNotification);
        else if(!videoFile.existsAsFile()) videoStatus.setText("Projet chargé, vidéo introuvable : "+videoPath,juce::dontSendNotification);
        else videoStatus.setText("Projet chargé : "+file.getFileName(),juce::dontSendNotification);
    }
    void newProjectAction(){
        {const juce::ScopedLock sl(state->lock);for(auto& m:state->markers)m.clear();state->file={};state->duration=0;state->playhead=0;state->activeMidiNote=-1;state->activeNoteCounts.fill(0);state->held=false;state->pendingTriggers.clear();}
        if(outputWindow)outputWindow->stopVideo();{const juce::ScopedLock sl(state->lock);state->file={};state->duration=0.0;state->playhead=0.0;}timeline.setDuration(60.0);timeline.repaint();currentProject={};projectDirty=true;
        videoStatus.setText("Nouveau projet",juce::dontSendNotification);writeRecoveryProject();
    }
    void addCurrentMarker(){int p=noteChoice.getSelectedId()-1;if(p<0||p>=12)return;double t=outputWindow?outputWindow->videoPosition():0.0;{const juce::ScopedLock sl(state->lock);t=juce::jlimit(0.0,state->duration,t);auto& v=state->markers[(size_t)p];v.clear();v.push_back(t);}saveMarkers();markProjectDirty();timeline.repaint();}
    void saveMarkers(){const juce::ScopedLock sl(state->lock);for(int i=0;i<12;++i)settings->setValue("markers"+juce::String(i),markerString(state->markers[(size_t)i]));settings->saveIfNeeded();markProjectDirty();}
    void refreshMidi(){ins=router.inputs();outs=router.outputs();inputBox.clear();outputBox.clear();inputBox.addItem("Choisir une entrée...",1);outputBox.addItem("Choisir une sortie...",1);for(int i=0;i<ins.size();++i)inputBox.addItem(ins.getReference(i).name,i+2);for(int i=0;i<outs.size();++i)outputBox.addItem(outs.getReference(i).name,i+2);
        auto si=settings->getValue("input"),so=settings->getValue("output");for(int i=0;i<ins.size();++i)if(ins.getReference(i).identifier==si)inputBox.setSelectedId(i+2,juce::dontSendNotification);for(int i=0;i<outs.size();++i)if(outs.getReference(i).identifier==so)outputBox.setSelectedId(i+2,juce::dontSendNotification);}
    void selectInput(){int i=inputBox.getSelectedId()-2;if(i>=0&&i<ins.size()){auto d=ins.getReference(i);router.openInput(d.identifier);settings->setValue("input",d.identifier);settings->saveIfNeeded();}}
    void selectOutput(){int i=outputBox.getSelectedId()-2;if(i>=0&&i<outs.size()){auto d=outs.getReference(i);router.openOutput(d.identifier);settings->setValue("output",d.identifier);settings->saveIfNeeded();}}
    std::shared_ptr<VideoState> state;MidiRouter router;MarkerTimeline timeline;juce::ThreadPool waveformJobs{1};
    ReadableLookAndFeel readableLookAndFeel;
    juce::TextButton newProject,openProject,saveProject,saveAsProject;
    juce::File currentProject;bool projectDirty=false;juce::uint32 nextAutoSaveMs=0;std::unique_ptr<juce::PropertiesFile> settings;std::unique_ptr<juce::FileChooser> chooser;std::unique_ptr<OutputWindow> outputWindow;
    juce::TextButton openVideo,play,addMarker,eraseMarker,refresh,outputButton;juce::ComboBox noteChoice,inputBox,outputBox;juce::Label inputLabel,outputLabel,videoStatus;juce::Array<juce::MidiDeviceInfo> ins,outs;
};

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow():DocumentWindow("MIDI Video Player",juce::Colour(0xff202329),allButtons){setUsingNativeTitleBar(true);setContentOwned(new MainView(),true);setResizable(true,true);centreWithSize(1120,850);}
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
