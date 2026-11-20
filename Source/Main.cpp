#include <JuceHeader.h>
#include <array>
#include <memory>
#include <cmath>

namespace {
void writeStartupLog(const juce::String& message) {
 auto logFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MIDI Image Player startup.log");
 logFile.appendText(juce::Time::getCurrentTime().toString(true, true) + " | " + message + "\r\n");
}
const std::array<const char*,12> noteNames{"Do","Do#","Ré","Ré#","Mi","Fa","Fa#","Sol","Sol#","La","La#","Si"};
struct State {
 juce::CriticalSection lock;
 std::array<juce::File,12> images;
 int pitch=-1, activeNote=-1;
 bool held=false;
 double fadeStart=0.0, fadeSeconds=0.5;
};

class Router final : private juce::MidiInputCallback {
public:
 explicit Router(std::shared_ptr<State> s):state(std::move(s)){}
 ~Router() override { stop(); output.reset(); }
 juce::Array<juce::MidiDeviceInfo> inputs() const { return juce::MidiInput::getAvailableDevices(); }
 juce::Array<juce::MidiDeviceInfo> outputs() const { return juce::MidiOutput::getAvailableDevices(); }
 void openInput(const juce::String& id){ stop(); if(id.isNotEmpty()){input=juce::MidiInput::openDevice(id,this); if(input) input->start();}}
 void openOutput(const juce::String& id){output.reset(); if(id.isNotEmpty()) output=juce::MidiOutput::openDevice(id);}
private:
 void stop(){if(input){input->stop();input.reset();}}
 void handleIncomingMidiMessage(juce::MidiInput*,const juce::MidiMessage& m) override {
  if(output) output->sendMessageNow(m);
  const juce::ScopedLock sl(state->lock);
  if(m.isNoteOn()){state->pitch=m.getNoteNumber()%12;state->activeNote=m.getNoteNumber();state->held=true;state->fadeStart=0;}
  else if(m.isNoteOff() && m.getNoteNumber()==state->activeNote){state->held=false;state->fadeStart=juce::Time::getMillisecondCounterHiRes();}
 }
 std::shared_ptr<State> state;
 std::unique_ptr<juce::MidiInput> input;
 std::unique_ptr<juce::MidiOutput> output;
};

class OutputView final:public juce::Component,private juce::Timer {
public:
 explicit OutputView(std::shared_ptr<State> s):state(std::move(s)){setOpaque(true);startTimerHz(60);}
 void paint(juce::Graphics& g) override {
  g.fillAll(juce::Colours::black);
  if(image.isNull()||alpha<=0.001f)return;
  auto area=getLocalBounds().toFloat().reduced(getWidth()*0.09f,getHeight()*0.09f);
  const float scale=juce::jmin(area.getWidth()/float(image.getWidth()),area.getHeight()/float(image.getHeight()));
  auto dest=juce::Rectangle<float>(image.getWidth()*scale,image.getHeight()*scale).withCentre(area.getCentre());
  g.setOpacity(alpha);g.drawImage(image,dest);
 }
private:
 void timerCallback() override {
  int p;juce::File f;float a=0;
  {const juce::ScopedLock sl(state->lock);p=state->pitch;if(p>=0)f=state->images[(size_t)p];
   if(p>=0){if(state->held)a=1;else{double d=state->fadeSeconds*1000.0;double e=juce::Time::getMillisecondCounterHiRes()-state->fadeStart;
    if(d<=0||e>=d){state->pitch=-1;state->activeNote=-1;p=-1;}else a=float(juce::jlimit(0.0,1.0,1.0-e/d));}}}
  if(p!=cachedPitch||f!=cachedFile){cachedPitch=p;cachedFile=f;image=f.existsAsFile()?juce::ImageFileFormat::loadFrom(f):juce::Image();}
  if(std::abs(alpha-a)>0.002f||p<0){alpha=a;repaint();}
 }
 std::shared_ptr<State> state;juce::Image image;juce::File cachedFile;int cachedPitch=-2;float alpha=0;
};

class OutputWindow final:public juce::DocumentWindow {
public:
 explicit OutputWindow(std::shared_ptr<State> s):DocumentWindow("MIDI Image Player | Sortie OBS",juce::Colours::black,allButtons){
  setUsingNativeTitleBar(true);setContentOwned(new OutputView(std::move(s)),true);setResizable(true,true);
  getConstrainer()->setFixedAspectRatio(16.0/9.0);setResizeLimits(480,270,1920,1080);centreWithSize(960,540);
 }
 void closeButtonPressed() override{setVisible(false);}
};

class MainView final:public juce::Component {
public:
 MainView():state(std::make_shared<State>()),router(state){
  writeStartupLog("MainView: début du constructeur");
  juce::PropertiesFile::Options o;o.applicationName="MIDI Image Player";o.filenameSuffix="settings";o.folderName="MIDI Image Player";
  settings=std::make_unique<juce::PropertiesFile>(o);
  for(int i=0;i<12;++i){
   labels[(size_t)i].setText(noteNames[(size_t)i],juce::dontSendNotification);labels[(size_t)i].setColour(juce::Label::textColourId,juce::Colours::white);
   addAndMakeVisible(labels[(size_t)i]);buttons[(size_t)i].setButtonText("Charger...");
   buttons[(size_t)i].onClick=[this,i]{loadImage(i);};addAndMakeVisible(buttons[(size_t)i]);
   auto path=settings->getValue("image"+juce::String(i));if(path.isNotEmpty())state->images[(size_t)i]=juce::File(path);
  }
  for(auto* l:{&inLabel,&outLabel,&fadeLabel}){l->setColour(juce::Label::textColourId,juce::Colours::white);addAndMakeVisible(*l);}
  inLabel.setText("Clavier MIDI (entrée)",juce::dontSendNotification);outLabel.setText("Sortie MIDI loopMIDI",juce::dontSendNotification);fadeLabel.setText("Fondu après relâchement",juce::dontSendNotification);
  addAndMakeVisible(inBox);addAndMakeVisible(outBox);addAndMakeVisible(refresh);addAndMakeVisible(showOutput);addAndMakeVisible(fade);
  refresh.setButtonText("Actualiser les appareils");refresh.onClick=[this]{refreshDevices();};
  showOutput.setButtonText("Ouvrir la fenêtre de sortie OBS");showOutput.onClick=[this]{openOutput();};
  fade.setRange(0,5,0.05);fade.setValue(settings->getDoubleValue("fadeSeconds",0.5),juce::dontSendNotification);
  fade.setTextBoxStyle(juce::Slider::TextBoxRight,false,70,24);fade.setTextValueSuffix(" s");
  {const juce::ScopedLock sl(state->lock);state->fadeSeconds=fade.getValue();}
  fade.onValueChange=[this]{{const juce::ScopedLock sl(state->lock);state->fadeSeconds=fade.getValue();}settings->setValue("fadeSeconds",fade.getValue());settings->saveIfNeeded();};
  inBox.onChange=[this]{chooseInput();};outBox.onChange=[this]{chooseOutput();};
  writeStartupLog("MainView: avant actualisation MIDI");
  refreshDevices();setSize(820,620);
  writeStartupLog("MainView: constructeur terminé");
 }
 ~MainView() override{outputWindow.reset();if(settings)settings->saveIfNeeded();}
 void paint(juce::Graphics& g) override{
  g.fillAll(juce::Colour(0xff202329));g.setColour(juce::Colours::white);g.setFont(juce::Font(22,juce::Font::bold));
  g.drawText("MIDI IMAGE PLAYER",24,16,getWidth()-48,32,juce::Justification::centredLeft);
  g.setColour(juce::Colour(0xffaeb6c2));g.setFont(14);g.drawText("Charge les images, choisis les appareils MIDI, puis ouvre la sortie OBS.",24,48,getWidth()-48,24,juce::Justification::centredLeft);
 }
 void resized() override{
  auto a=getLocalBounds().reduced(24);a.removeFromTop(82);auto d=a.removeFromTop(112);auto l=d.removeFromLeft((d.getWidth()-20)/2);
  inLabel.setBounds(l.removeFromTop(24));inBox.setBounds(l.removeFromTop(32));d.removeFromLeft(20);outLabel.setBounds(d.removeFromTop(24));outBox.setBounds(d.removeFromTop(32));
  refresh.setBounds(a.removeFromTop(34).withWidth(220));a.removeFromTop(10);auto s=a.removeFromTop(64);fadeLabel.setBounds(s.removeFromTop(22));fade.setBounds(s.removeFromTop(34));a.removeFromTop(8);
  auto grid=a.removeFromTop(260);int cw=grid.getWidth()/4,ch=58;
  for(int i=0;i<12;++i){int c=i%4,r=i/4;auto cell=juce::Rectangle<int>(grid.getX()+c*cw,grid.getY()+r*ch,cw-8,ch-6);labels[(size_t)i].setBounds(cell.removeFromLeft(58));buttons[(size_t)i].setBounds(cell);}
  showOutput.setBounds(a.removeFromTop(42).withWidth(280));
 }
private:
 void loadImage(int i){
  chooser=std::make_unique<juce::FileChooser>("Image pour "+juce::String(noteNames[(size_t)i]),juce::File(),"*.png;*.jpg;*.jpeg");
  chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,[this,i](const juce::FileChooser& c){
   auto f=c.getResult();if(f.existsAsFile()){{const juce::ScopedLock sl(state->lock);state->images[(size_t)i]=f;}settings->setValue("image"+juce::String(i),f.getFullPathName());settings->saveIfNeeded();}chooser.reset();
  });
 }
 void refreshDevices(){
  inDevices=router.inputs();outDevices=router.outputs();inBox.clear();outBox.clear();inBox.addItem("Sélectionner le clavier...",1);outBox.addItem("Sélectionner la sortie...",1);
  for(int i=0;i<inDevices.size();++i)inBox.addItem(inDevices.getReference(i).name,i+2);
  for(int i=0;i<outDevices.size();++i)outBox.addItem(outDevices.getReference(i).name,i+2);
  auto si=settings->getValue("midiInput"),so=settings->getValue("midiOutput");
  for(int i=0;i<inDevices.size();++i)if(inDevices.getReference(i).identifier==si)inBox.setSelectedId(i+2,juce::dontSendNotification);
  for(int i=0;i<outDevices.size();++i)if(outDevices.getReference(i).identifier==so)outBox.setSelectedId(i+2,juce::dontSendNotification);
 }
 void chooseInput(){int i=inBox.getSelectedId()-2;if(i>=0&&i<inDevices.size()){auto d=inDevices.getReference(i);router.openInput(d.identifier);settings->setValue("midiInput",d.identifier);settings->saveIfNeeded();}}
 void chooseOutput(){int i=outBox.getSelectedId()-2;if(i>=0&&i<outDevices.size()){auto d=outDevices.getReference(i);router.openOutput(d.identifier);settings->setValue("midiOutput",d.identifier);settings->saveIfNeeded();}}
 void openOutput(){if(!outputWindow)outputWindow=std::make_unique<OutputWindow>(state);outputWindow->setVisible(true);outputWindow->toFront(true);}
 std::shared_ptr<State> state;Router router;std::unique_ptr<juce::PropertiesFile> settings;std::unique_ptr<juce::FileChooser> chooser;std::unique_ptr<OutputWindow> outputWindow;
 std::array<juce::TextButton,12> buttons;std::array<juce::Label,12> labels;juce::Label inLabel,outLabel,fadeLabel;juce::ComboBox inBox,outBox;juce::TextButton refresh,showOutput;juce::Slider fade;
 juce::Array<juce::MidiDeviceInfo> inDevices,outDevices;
};
class MainWindow final:public juce::DocumentWindow {
public:MainWindow():DocumentWindow("MIDI Image Player",juce::Colour(0xff202329),allButtons){writeStartupLog("MainWindow: début");setUsingNativeTitleBar(true);setContentOwned(new MainView(),true);setResizable(true,true);centreWithSize(820,620);writeStartupLog("MainWindow: fenêtre créée");}
 void closeButtonPressed() override{juce::JUCEApplication::getInstance()->systemRequestedQuit();}
};
class App final:public juce::JUCEApplication {
public:const juce::String getApplicationName()override{return "MIDI Image Player";}const juce::String getApplicationVersion()override{return "1.0.0";}bool moreThanOneInstanceAllowed()override{return false;}
 void initialise(const juce::String&)override{writeStartupLog("Application: initialise");window=std::make_unique<MainWindow>();writeStartupLog("Application: fenêtre principale prête");}void shutdown()override{writeStartupLog("Application: shutdown");window.reset();}
 void unhandledException(const std::exception* e,const juce::String& source,int line) override {writeStartupLog("Exception: "+(e?juce::String(e->what()):juce::String("inconnue"))+" | "+source+" | ligne "+juce::String(line));}
private:std::unique_ptr<MainWindow> window;
};
}
START_JUCE_APPLICATION(App)
