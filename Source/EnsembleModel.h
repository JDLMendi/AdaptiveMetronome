#pragma once
#include <JuceHeader.h>
#include <vector>
#include <atomic>
#include <thread>
#include "Player.h"

using std::function;

class AdaptiveMetronomeAudioProcessor;

class EnsembleModel :
    private juce::OSCReceiver::ListenerWithOSCAddress <juce::OSCReceiver::MessageLoopCallback>,
    public juce::ActionBroadcaster
{
public:

    //==============================================================================
    // Constructor and Destructor
    EnsembleModel(AdaptiveMetronomeAudioProcessor *processorPtr);
    ~EnsembleModel();
    
    //==============================================================================
    // MIDI Handling
    bool loadMidiFile(const juce::File& file, int userPlayers);
    bool isMidiLoaded();
    juce::String getMidiFileName();
    juce::File getMidiFile();

    //==============================================================================
    // Processing and Playback
    void prepareToPlay(double newSampleRate);
    void releaseResources();
    void processMidiBlock(const juce::MidiBuffer& inMidi, juce::MidiBuffer& outMidi, int numSamples, double tempo);    
    bool reset();
    void resetPlayers();
    static void soundOffAllChannels(juce::MidiBuffer& midi);
    static bool checkMidiSequenceHasNotes(const juce::MidiMessageSequence* seq);

    //==============================================================================
    // Player Management
    int getNumPlayers();
    int getNumUserPlayers();
    bool isPlayerUserOperated(int playerIndex);
    void setAlphaBetaParams(float valueIn);
    juce::AudioParameterInt& getPlayerChannelParameter(int playerIndex);
    juce::AudioParameterFloat& getPlayerDelayParameter(int playerIndex);
    juce::AudioParameterFloat& getPlayerMotorNoiseParameter(int playerIndex);
    juce::AudioParameterFloat& getPlayerTimeKeeperNoiseParameter(int playerIndex);
    juce::AudioParameterFloat& getPlayerVolumeParameter(int playerIndex);
    juce::AudioParameterFloat& getAlphaParameter(int player1Index, int player2Index);
    juce::AudioParameterFloat& getBetaParameter(int player1Index, int player2Index);
    
    //==============================================================================
    // OSC Messaging
    juce::OSCSender OSCSender;
    juce::OSCReceiver OSCReceiver;
    bool isOSCInitialised = false;
    int currentReceivePort = -1;

    void initialiseOSC();
    void connectOSCSender(int portNumber, juce::String IPAddress);
    void connectOSCReceiver(int portNumber);
    void oscMessageReceived(const juce::OSCMessage& message);
    void oscAcknowledge(const juce::String& receivedAddress);
    void oscSendEnsembleDetails();
    bool isOscReceiverConnected();

    //==============================================================================
    // XML Configuration
    juce::String logSubfolder = "";
    juce::String logFilenameOverride = "";
    juce::String configSubfolder = "";

    void saveConfigToXmlFile();
    void loadConfigFromXml(juce::File configFile); // Loading from file directly
    void loadConfigFromXml(std::unique_ptr<juce::XmlElement> loadedConfig);
    std::unique_ptr<juce::XmlElement> parseXmlConfigFileToXmlElement(juce::File configFile);

    
private:
    //==============================================================================
    // Internal State
    AdaptiveMetronomeAudioProcessor *processor = nullptr;    
    int numUserPlayers = 1;
    juce::MidiFile midiFile;
    juce::File midiFilePath;
    bool midiLoaded = false;

    //==============================================================================
    // Timing and Playback
    double sampleRate = 44100.0;
    int samplesPerBeat = sampleRate / 4;
    int scoreCounter = 0;
    bool initialTempoSet = false;

    // Following functions are ammending timings for each player and called only within processMidiBlock()
    void setTempo(double bpm);
    void setInitialPlayerTempo();
    bool newOnsetsAvailable();
    void calculateNewIntervals();
    void clearOnsetsAvailable();
    
    //==============================================================================
    // Intro Tones
    const int introToneChannel = 16;
    int numIntroTones = 4;
    static const int introToneNoteFirst = 84;
    static const int introToneNoteOther = 72;
    static const juce::uint8 introToneVel = 100;
    int introCounter = 0;
    int introTonesPlayed = 0;
    
    void playIntroTones (juce::MidiBuffer &midi, int sampleIndex);
    void introToneOn (juce::MidiBuffer &midi, int sampleIndex);
    void introToneOff (juce::MidiBuffer &midi, int sampleIndex);
    void introToneOnOff (juce::MidiBuffer &midi, juce::MidiMessage (*function)(int, int, juce::uint8), int sampleIndex);
    
    //==============================================================================
    // Player Management
  
    // The following functions should only be called when the playersInUse
    // flag has been locked using the FlagLock class.

    class FlagLock
    {
    public:
        FlagLock(std::atomic_flag& f);
        ~FlagLock();

        std::atomic_flag& flag;
        bool locked;
    };

    std::vector <std::unique_ptr <Player> > players;
    std::atomic_flag playersInUse;
    std::atomic_flag resetFlag;

    void createPlayers(const juce::MidiFile& file);
    void createAlphaBetaParameters();
    void getLatestAlphas();
    void storeOnsetDetailsForPlayer (int bufferIndex, int playerIndex);
    void playScore (const juce::MidiBuffer &inMidi, juce::MidiBuffer &outMidi, int sampleIndex);
    void playUserIntro(const juce::MidiBuffer& inMidi, juce::MidiBuffer& outMidi, int sampleIndex);
    
    //==============================================================================
    // Logging
    
    // A bunch of stuff for safely logging onset times and sending them out to the
    // server. Functions defined in here are only safe to call from the logging thread.

    struct LogData
    {
        int onsetTime, onsetInterval;
        bool userInput;
        double delay;
        double motorNoise, timeKeeperNoise;
        std::vector <int> asyncs;
        std::vector <float> alphas;
        std::vector <float> betas;
        double tkNoiseStd, mNoiseStd;
        double volume;
    };

    std::unique_ptr <juce::AbstractFifo> loggingFifo;
    std::vector <LogData> loggingBuffer;
    std::thread loggerThread;
    std::atomic <bool> continueLogging;
    int logLineCounter = 0;

    void initialiseLoggingBuffer();
    void startLoggerLoop();
    void stopLoggerLoop();
    void loggerLoop();
    void writeLogHeader (juce::FileOutputStream &logStream);
    void logOnsetDetails (juce::FileOutputStream &logStream);
    void logOnsetDetailsForPlayer (int bufferIndex,
                                   juce::String &onsetLog,
                                   juce::String &intervalLog,
                                   juce::String &userInputLog,
                                   juce::String &delayLog,
                                   juce::String &mNoiseLog,
                                   juce::String &tkNoiseLog,
                                   juce::String &asyncLog,
                                   juce::String &alphaLog,
                                   juce::String &betaLog,
                                   juce::String &tkNoiseStdLog,
                                   juce::String &mNoiseStdLog,
                                   juce::String &velocityLog);
                                   
    void postLatestOnsets (const std::vector <int> &onsets, const std::vector <int> &delays);
                                   
    //==============================================================================
    // Polling for new Alpha Values
    std::unique_ptr <juce::AbstractFifo> pollingFifo;
    std::vector <std::vector <float> > pollingBuffer;
    std::thread pollingThread;
    std::atomic <bool> continuePolling;
    std::atomic_flag alphasUpToDate;

    void initialisePollingBuffers();
    void startPollingLoop();
    void stopPollingLoop();
    void pollingLoop();
    void getNewAlphas();     
};
