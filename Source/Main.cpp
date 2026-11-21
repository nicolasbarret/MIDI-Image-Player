#include <JuceHeader.h>
#include "LibVlcPlayer.h"
#include <array>
#include <vector>
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
    double duration=0.0, playhead=0.0, fadeSeconds=0.6;
    int pitch=-1;
    bool held=false, freezeOnRelease=true;
    double releaseMs=0.0;
    juce::uint64 triggerSerial=0;
};
static juce::String markerString(const std::vector<double>& v) {
    juce::StringArray a; for(double x:v) a.add(juce::String(x,6)); return a.joinIntoString(",");
}
static std::vector<double> parseMarkers(const juce::String& s) {
    std::vector<double> v; juce::StringArray a; a.addTokens(s,",",""); a.trim(); a.removeEmptyStrings();
    for(auto& x:a) { double d=x.getDoubleValue(); if(d>=0.0) v.push_back(d); }
    std::sort(v.begin(),v.end()); return v;
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
        const juce::ScopedLock sl(state->lock);
        if(m.isNoteOn()) {
            int p=m.getNoteNumber()%12; auto& list=state->markers[(size_t)p];
            if(!list.empty()) {
                double chosen=list.front(), best=std::abs(chosen-state->playhead);
                for(double t:list){double d=std::abs(t-state->playhead);if(d<best){best=d;chosen=t;}}
                state->pitch=p; state->held=true; state->releaseMs=0.0; ++state->triggerSerial;
                state->playhead=chosen;
            }
        } else if(m.isNoteOff() && state->pitch==m.getNoteNumber()%12) {
            state->held=false; state->releaseMs=juce::Time::getMillisecondCounterHiRes();
        }
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
    void setDuration(double d){duration=juce::jmax(0.01,d);repaint();}
    void setEraseMode(bool enabled){eraseMode=enabled;setMouseCursor(enabled?juce::MouseCursor::CrosshairCursor:juce::MouseCursor::PointingHandCursor);repaint();}
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff171a20));
        auto r=getLocalBounds().toFloat(); float left=48.0f, right=12.0f, top=5.0f;
        float trackH=(r.getHeight()-top-4.0f)/12.0f; 
        g.setFont(12.0f);
        for(int i=0;i<12;++i){
            float y=top+i*trackH;
            g.setColour(i%2?juce::Colour(0xff20252d):juce::Colour(0xff1c2128));
            g.fillRect(left,y,r.getWidth()-left-right,trackH-1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(notes[(size_t)i],2,(int)y,42,(int)trackH,juce::Justification::centredLeft);
            const juce::ScopedLock sl(state->lock);
            for(double t:state->markers[(size_t)i]){
                float x=left+(float)(juce::jlimit(0.0,duration,t)/duration)*(r.getWidth()-left-right);
                g.setColour(juce::Colour::fromHSV((float)i/12.0f,0.72f,0.98f,1.0f));
                juce::Path p;p.addTriangle(x,y+3,x-5,y+trackH-4,x+5,y+trackH-4);g.fillPath(p);
                g.drawLine(x,y+trackH-5,x,y+trackH-1,1.0f);
            }
        }
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        float px=left+(float)(juce::jlimit(0.0,duration,state->playhead)/duration)*(r.getWidth()-left-right);
        g.drawLine(px,top,px,r.getHeight()-2,1.3f);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        auto r=getLocalBounds().toFloat(); float left=48.0f,right=12.0f,top=5.0f;
        float th=(r.getHeight()-top-4.0f)/12.0f;
        int row=juce::jlimit(0,11,(int)((e.position.y-top)/th));
        float x=e.position.x; double clicked=(x-left)/juce::jmax(1.0f,r.getWidth()-left-right)*duration;
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
        auto r=getLocalBounds().toFloat();double t=(e.position.x-48.0)/juce::jmax(1.0f,r.getWidth()-60.0f)*duration;moveMarker(t);
    }
    void mouseUp(const juce::MouseEvent&) override {
        if(dragging && dragRow>=0){const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)dragRow];std::sort(v.begin(),v.end());}
        dragging=false;dragIndex=-1;if(changed)changed();
    }
private:
    void moveMarker(double t){
        const juce::ScopedLock sl(state->lock);auto& v=state->markers[(size_t)dragRow];
        if(dragIndex>=0&&dragIndex<(int)v.size()){v[(size_t)dragIndex]=juce::jlimit(0.0,duration,t);}
        repaint();
    }
    std::shared_ptr<VideoState> state;double duration=60.0;bool dragging=false,eraseMode=false;int dragRow=-1,dragIndex=-1;
};

class VideoOutputView final : public juce::Component, private juce::Timer {
public:
    explicit VideoOutputView(std::shared_ptr<VideoState> s):state(std::move(s)){
        addAndMakeVisible(video);startTimerHz(30);
    }
    void resized() override {video.setBounds(getLocalBounds());}
private:
    void markProjectDirty(){projectDirty=true;nextAutoSaveMs=juce::Time::getMillisecondCounter()+1000;}
    void timerCallback() override {
        juce::File file;juce::uint64 serial;bool held,freeze;double release,fade;
        {const juce::ScopedLock sl(state->lock);file=state->file;serial=state->triggerSerial;held=state->held;freeze=state->freezeOnRelease;release=state->releaseMs;fade=state->fadeSeconds;}
        if(file!=loadedFile){
            loadedFile=file;video.stop();video.unload();seenSerial=0;fading=false;
            if(file.existsAsFile()){if(video.load(file)){video.setPlayPosition(0.0);video.setBlackoutAlpha(0);}else video.setBlackoutAlpha(0);}
            else video.setBlackoutAlpha(0);
        }
        if(serial!=seenSerial&&serial!=0){seenSerial=serial;double start=0;{const juce::ScopedLock sl(state->lock);start=state->playhead;}video.setPlayPosition(start);video.setBlackoutAlpha(0);video.play();fading=false;}
        if(seenSerial!=0&&!held&&release>0.0){
            if(!fading){fading=true;fadeStart=release;if(freeze)video.stop();}
            double elapsed=juce::Time::getMillisecondCounterHiRes()-fadeStart;
            float a=fade<=0.0?0.0f:(float)juce::jlimit(0.0,1.0,1.0-elapsed/(fade*1000.0));
            video.setBlackoutAlpha((int)((1.0f-a)*255.0f));
            if(a<=0.001f){video.stop();video.setBlackoutAlpha(255);fading=false;seenSerial=0;const juce::ScopedLock sl(state->lock);state->pitch=-1;state->releaseMs=0.0;}
        }
    }
    std::shared_ptr<VideoState> state;vlcplayer::VideoView video;juce::File loadedFile;juce::uint64 seenSerial=0;bool fading=false;double fadeStart=0.0;
};

class OutputWindow final : public juce::DocumentWindow {
public:
    explicit OutputWindow(std::shared_ptr<VideoState> s):DocumentWindow("MIDI Video Player | Sortie OBS",juce::Colours::black,allButtons){
        setUsingNativeTitleBar(true);setContentOwned(new VideoOutputView(std::move(s)),true);setResizable(true,true);
        getConstrainer()->setFixedAspectRatio(16.0/9.0);setResizeLimits(480,270,1920,1080);centreWithSize(960,540);
    }
    void closeButtonPressed() override {setVisible(false);}
};

class MainView final : public juce::Component, private juce::Timer {
public:
    MainView():state(std::make_shared<VideoState>()),router(state),timeline(state){
        juce::PropertiesFile::Options o;o.applicationName="MIDI Video Player";o.filenameSuffix="settings";o.folderName="MIDI Video Player";
        settings=std::make_unique<juce::PropertiesFile>(o);
        auto path=settings->getValue("video");if(path.isNotEmpty()){juce::File f(path);if(f.existsAsFile())loadVideo(f);}
        for(int i=0;i<12;++i){auto key="markers"+juce::String(i);state->markers[(size_t)i]=parseMarkers(settings->getValue(key));}
        state->fadeSeconds=settings->getDoubleValue("fade",0.6);state->freezeOnRelease=settings->getBoolValue("freeze",true);
        addAndMakeVisible(preview);addAndMakeVisible(timeline);preview.setOpaque(true);preview.setMuted(true);videoStatus.setColour(juce::Label::textColourId,juce::Colours::white);videoStatus.setText("Aucune vidéo chargée",juce::dontSendNotification);
        setLookAndFeel(&readableLookAndFeel);
        addAndMakeVisible(newProject);addAndMakeVisible(openProject);addAndMakeVisible(saveProject);addAndMakeVisible(saveAsProject);
        addAndMakeVisible(openVideo);addAndMakeVisible(play);addAndMakeVisible(addMarker);addAndMakeVisible(eraseMarker);addAndMakeVisible(noteChoice);addAndMakeVisible(videoStatus);
        newProject.setButtonText("Nouveau projet");newProject.onClick=[this]{newProjectAction();};
        openProject.setButtonText("Ouvrir projet");openProject.onClick=[this]{chooseProjectToOpen();};
        saveProject.setButtonText("Enregistrer");saveProject.onClick=[this]{saveProjectAction();};
        saveAsProject.setButtonText("Enregistrer sous");saveAsProject.onClick=[this]{chooseProjectToSave();};
        for(auto* b:{&newProject,&openProject,&saveProject,&saveAsProject,&openVideo,&play,&addMarker,&eraseMarker,&refresh,&outputButton}) b->setColour(juce::TextButton::buttonColourId,juce::Colour(0xff354252));
        addAndMakeVisible(inputLabel);addAndMakeVisible(outputLabel);addAndMakeVisible(inputBox);addAndMakeVisible(outputBox);addAndMakeVisible(refresh);
        addAndMakeVisible(modeLabel);addAndMakeVisible(modeBox);addAndMakeVisible(fadeLabel);addAndMakeVisible(fadeSlider);addAndMakeVisible(outputButton);
        openVideo.setButtonText("Ouvrir une vidéo");openVideo.onClick=[this]{chooseVideo();};
        play.setButtonText("Lire / Pause");play.onClick=[this]{if(preview.isPlaying())preview.stop();else preview.play();};
        addMarker.setButtonText("Ajouter un repère à la position");addMarker.onClick=[this]{addCurrentMarker();};
        eraseMarker.setButtonText("Gomme désactivée");eraseMarker.setClickingTogglesState(true);eraseMarker.onClick=[this]{bool on=eraseMarker.getToggleState();eraseMarker.setButtonText(on?"Gomme activée : cliquer un repère":"Gomme désactivée");timeline.setEraseMode(on);};
        for(int i=0;i<12;++i)noteChoice.addItem(notes[(size_t)i],i+1);
        noteChoice.setSelectedId(1);
        timeline.changed=[this]{saveMarkers();};
        timeline.seeked=[this](double t){preview.setPlayPosition(t);};
        inputLabel.setText("Entrée MIDI",juce::dontSendNotification);outputLabel.setText("Sortie MIDI (loopMIDI)",juce::dontSendNotification);
        modeLabel.setText("Au relâchement",juce::dontSendNotification);modeBox.addItem("Figer la dernière image puis fondu",1);modeBox.addItem("Continuer la vidéo pendant le fondu",2);
        modeBox.setSelectedId(state->freezeOnRelease?1:2);modeBox.onChange=[this]{const juce::ScopedLock sl(state->lock);state->freezeOnRelease=modeBox.getSelectedId()==1;settings->setValue("freeze",state->freezeOnRelease);settings->saveIfNeeded();};
        fadeLabel.setText("Durée du fondu",juce::dontSendNotification);fadeSlider.setRange(0,5,0.05);fadeSlider.setValue(state->fadeSeconds);fadeSlider.setTextBoxStyle(juce::Slider::TextBoxRight,false,65,22);fadeSlider.setTextValueSuffix(" s");
        fadeSlider.onValueChange=[this]{const juce::ScopedLock sl(state->lock);state->fadeSeconds=fadeSlider.getValue();settings->setValue("fade",state->fadeSeconds);settings->saveIfNeeded();};
        refresh.setButtonText("Actualiser");refresh.onClick=[this]{refreshMidi();};inputBox.onChange=[this]{selectInput();};outputBox.onChange=[this]{selectOutput();};
        outputButton.setButtonText("Ouvrir la sortie vidéo OBS");outputButton.onClick=[this]{if(!outputWindow)outputWindow=std::make_unique<OutputWindow>(state);outputWindow->setVisible(true);outputWindow->toFront(true);};
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
    void resized() override {
        auto a=getLocalBounds().reduced(22);a.removeFromTop(72);
        auto projectBar=a.removeFromTop(38);
        newProject.setBounds(projectBar.removeFromLeft(145));projectBar.removeFromLeft(7);
        openProject.setBounds(projectBar.removeFromLeft(130));projectBar.removeFromLeft(7);
        saveProject.setBounds(projectBar.removeFromLeft(115));projectBar.removeFromLeft(7);
        saveAsProject.setBounds(projectBar.removeFromLeft(145));
        a.removeFromTop(8);
        auto top=a.removeFromTop(300);auto left=top.removeFromLeft(520);videoStatus.setBounds(left.removeFromTop(22));preview.setBounds(left);auto controls=top.reduced(12,0);
        openVideo.setBounds(controls.removeFromTop(36));controls.removeFromTop(8);play.setBounds(controls.removeFromTop(34));controls.removeFromTop(8);
        inputLabel.setBounds(controls.removeFromTop(20));inputBox.setBounds(controls.removeFromTop(28));controls.removeFromTop(5);
        outputLabel.setBounds(controls.removeFromTop(20));outputBox.setBounds(controls.removeFromTop(28));refresh.setBounds(controls.removeFromTop(30));
        a.removeFromTop(8);auto timelineHead=a.removeFromTop(32);addMarker.setBounds(timelineHead.removeFromLeft(245));timelineHead.removeFromLeft(8);eraseMarker.setBounds(timelineHead.removeFromLeft(180));timelineHead.removeFromLeft(8);noteChoice.setBounds(timelineHead.removeFromLeft(110));
        timeline.setBounds(a.removeFromTop(250));a.removeFromTop(8);
        auto opts=a.removeFromTop(34);modeLabel.setBounds(opts.removeFromLeft(115));modeBox.setBounds(opts.removeFromLeft(300));opts.removeFromLeft(15);fadeLabel.setBounds(opts.removeFromLeft(100));fadeSlider.setBounds(opts.removeFromLeft(180));
        a.removeFromTop(10);outputButton.setBounds(a.removeFromTop(38).withWidth(250));
    }
private:
    void timerCallback() override {
        if(preview.isPlaying()){double p=preview.getPlayPosition();{const juce::ScopedLock sl(state->lock);state->playhead=p;}timeline.repaint();}
        if(projectDirty && juce::Time::getMillisecondCounter() >= nextAutoSaveMs){
            writeRecoveryProject();projectDirty=false;nextAutoSaveMs=juce::Time::getMillisecondCounter()+3000;
        }
    }
    void chooseVideo(){chooser=std::make_unique<juce::FileChooser>("Choisir une vidéo",juce::File(),"*.mp4;*.mov;*.m4v;*.avi;*.wmv");chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser& c){auto f=c.getResult();if(f.existsAsFile())loadVideo(f);chooser.reset();});}
    void loadVideo(const juce::File& f){
        preview.stop();preview.unload();videoStatus.setText("Chargement de : "+f.getFileName(),juce::dontSendNotification);
        if(preview.load(f)){
            {const juce::ScopedLock sl(state->lock);state->file=f;state->duration=preview.getVideoDuration();state->playhead=0.0;}
            timeline.setDuration(preview.getVideoDuration());preview.setPlayPosition(0.0);preview.play();
            videoStatus.setText("Vidéo chargée : "+f.getFileName(),juce::dontSendNotification);
            settings->setValue("video",f.getFullPathName());settings->saveIfNeeded();
        }else{
            videoStatus.setText("Erreur vidéo : "+preview.errorMessage(),juce::dontSendNotification);
        }
    }
    juce::File recoveryProjectFile() const {
        auto dir=juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MIDI Video Player");
        dir.createDirectory();return dir.getChildFile("Recovery.mvproj");
    }
    juce::var makeProjectData(){
        auto* obj=new juce::DynamicObject();juce::var root(obj);
        const juce::ScopedLock sl(state->lock);
        obj->setProperty("formatVersion",1);
        obj->setProperty("video",state->file.getFullPathName());
        obj->setProperty("fadeSeconds",state->fadeSeconds);
        obj->setProperty("freezeOnRelease",state->freezeOnRelease);
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
            state->fadeSeconds=(double)obj->getProperty("fadeSeconds");
            state->freezeOnRelease=(bool)obj->getProperty("freezeOnRelease");
            videoPath=obj->getProperty("video").toString();
            state->playhead=0.0;state->held=false;state->pitch=-1;
        }
        settings->setValue("fade",state->fadeSeconds);settings->setValue("freeze",state->freezeOnRelease);
        settings->setValue("input",obj->getProperty("midiInput").toString());settings->setValue("output",obj->getProperty("midiOutput").toString());
        settings->saveIfNeeded();currentProject=(file==recoveryProjectFile()?juce::File{}:file);projectDirty=false;
        fadeSlider.setValue(state->fadeSeconds,juce::dontSendNotification);modeBox.setSelectedId(state->freezeOnRelease?1:2,juce::dontSendNotification);
        auto videoFile=juce::File(videoPath);
        if(videoFile.existsAsFile()) loadVideo(videoFile); else {const juce::ScopedLock sl(state->lock);state->file=videoFile;videoStatus.setText("Vidéo introuvable : "+videoPath,juce::dontSendNotification);}
        timeline.setDuration(state->duration>0.0?state->duration:60.0);timeline.repaint();refreshMidi();
        if(videoPath.isEmpty()) videoStatus.setText("Projet chargé : aucune vidéo associée",juce::dontSendNotification);
        else if(!videoFile.existsAsFile()) videoStatus.setText("Projet chargé, vidéo introuvable : "+videoPath,juce::dontSendNotification);
        else if(preview.getVideoDuration()>0.0) videoStatus.setText("Projet chargé : "+file.getFileName(),juce::dontSendNotification);
    }
    void newProjectAction(){
        {const juce::ScopedLock sl(state->lock);for(auto& m:state->markers)m.clear();state->file={};state->duration=0;state->playhead=0;state->pitch=-1;state->held=false;}
        preview.stop();preview.unload();timeline.setDuration(60.0);timeline.repaint();currentProject={};projectDirty=true;
        videoStatus.setText("Nouveau projet",juce::dontSendNotification);writeRecoveryProject();
    }
    void addCurrentMarker(){int p=noteChoice.getSelectedId()-1;if(p<0||p>=12)return;double t=preview.getPlayPosition();{const juce::ScopedLock sl(state->lock);t=juce::jlimit(0.0,state->duration,t);auto& v=state->markers[(size_t)p];v.push_back(t);std::sort(v.begin(),v.end());}saveMarkers();markProjectDirty();timeline.repaint();}
    void saveMarkers(){const juce::ScopedLock sl(state->lock);for(int i=0;i<12;++i)settings->setValue("markers"+juce::String(i),markerString(state->markers[(size_t)i]));settings->saveIfNeeded();markProjectDirty();}
    void refreshMidi(){ins=router.inputs();outs=router.outputs();inputBox.clear();outputBox.clear();inputBox.addItem("Choisir une entrée...",1);outputBox.addItem("Choisir une sortie...",1);for(int i=0;i<ins.size();++i)inputBox.addItem(ins.getReference(i).name,i+2);for(int i=0;i<outs.size();++i)outputBox.addItem(outs.getReference(i).name,i+2);
        auto si=settings->getValue("input"),so=settings->getValue("output");for(int i=0;i<ins.size();++i)if(ins.getReference(i).identifier==si)inputBox.setSelectedId(i+2,juce::dontSendNotification);for(int i=0;i<outs.size();++i)if(outs.getReference(i).identifier==so)outputBox.setSelectedId(i+2,juce::dontSendNotification);}
    void selectInput(){int i=inputBox.getSelectedId()-2;if(i>=0&&i<ins.size()){auto d=ins.getReference(i);router.openInput(d.identifier);settings->setValue("input",d.identifier);settings->saveIfNeeded();}}
    void selectOutput(){int i=outputBox.getSelectedId()-2;if(i>=0&&i<outs.size()){auto d=outs.getReference(i);router.openOutput(d.identifier);settings->setValue("output",d.identifier);settings->saveIfNeeded();}}
    std::shared_ptr<VideoState> state;MidiRouter router;vlcplayer::VideoView preview;MarkerTimeline timeline;
    ReadableLookAndFeel readableLookAndFeel;
    juce::TextButton newProject,openProject,saveProject,saveAsProject;
    juce::File currentProject;bool projectDirty=false;juce::uint32 nextAutoSaveMs=0;std::unique_ptr<juce::PropertiesFile> settings;std::unique_ptr<juce::FileChooser> chooser;std::unique_ptr<OutputWindow> outputWindow;
    juce::TextButton openVideo,play,addMarker,eraseMarker,refresh,outputButton;juce::ComboBox noteChoice,inputBox,outputBox,modeBox;juce::Label inputLabel,outputLabel,modeLabel,fadeLabel,videoStatus;juce::Slider fadeSlider;juce::Array<juce::MidiDeviceInfo> ins,outs;
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
