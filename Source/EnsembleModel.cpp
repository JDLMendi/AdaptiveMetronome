#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "EnsembleModel.h"
#include "UserPlayer.h"

using namespace std::chrono_literals;

//==============================================================================
// Constructor and Destructor

/**
 * @brief Construct a new Ensemble Model object.
 *
 * @details This constructor initializes the EnsembleModel by clearing the
 * playersInUse and resetFlag. In debug mode, it connects the OSC receiver
 * and sender to default ports and IP address for testing purposes.
 *
 * @param processorPtr Pointer to the AdaptiveMetronomeAudioProcessor used to
 * access parameters from the Metronome.
 */

EnsembleModel::EnsembleModel(AdaptiveMetronomeAudioProcessor *processorPtr)
    : processor(processorPtr)
{
    playersInUse.clear();
    resetFlag.clear();

#ifdef JUCE_DEBUG
    // Code here runs only in Debug mode
    connectOSCReceiver(8080);
    connectOSCSender(8090, "127.0.0.1");
#endif
}

/**
 * @brief Destructor for EnsembleModel
 *
 * @details This destructor stops the logger and polling threads when the
 * EnsembleModel object is destroyed.
 */
EnsembleModel::~EnsembleModel()
{
    stopLoggerLoop();
    stopPollingLoop();
}

//==============================================================================
// MIDI File Handling

/**
 * @brief Load a MIDI file into the EnsembleModel and create players for each track.
 *
 * @details This function loads a MIDI file, creates players for each track in the
 * file and resets the players. The userPlayers parameter determines how many user
 * operated players are created. The function returns true if successful, false if
 * the MIDI file is not loaded or if there is an error.
 *
 * @param file The file to load.
 * @param userPlayers The number of user operated players to create.
 *
 * @return true if successful, false if the MIDI file is not loaded or if there is
 * an error.
 */
bool EnsembleModel::loadMidiFile(const juce::File &file, int userPlayers)
{
    FlagLock lock(playersInUse);
    midiFilePath = file;
    midiLoaded = true;

    if (!lock.locked)
    {
        return false;
    }

    //==========================================================================
    // Read in content of MIDI file.
    juce::FileInputStream inStream(file);

    if (!inStream.openedOk())
        return false; // put some error handling here

    int fileType = 0;

    if (!midiFile.readFrom(inStream, true, &fileType))
        return false; // more error handling

    midiFile.convertTimestampTicksToSeconds();

    //==========================================================================
    // Create player for each track in the file.
    numUserPlayers = userPlayers;
    createPlayers(midiFile); // create new players
    resetPlayers();

    return true;
}

/**
 * @brief Get the name of the MIDI file loaded into the EnsembleModel.
 *
 * @details If no MIDI file has been loaded, an empty string is returned.
 *
 * @return The name of the loaded MIDI file.
 */
juce::String EnsembleModel::getMidiFileName()
{
    return midiFilePath.getFileName();
}

/**
 * @brief Check if a MIDI file has been loaded into the EnsembleModel.
 *
 * @details A MIDI file is considered loaded if it was successfully loaded
 *          into the EnsembleModel via the loadMidiFile function.
 *
 * @return true if a MIDI file has been loaded, false otherwise.
 */

bool EnsembleModel::isMidiLoaded()
{
    return midiLoaded;
}

/**
 * @brief Get the MIDI file loaded into the EnsembleModel.
 *
 * @details If no MIDI file has been loaded, an empty file object is returned.
 *
 * @return The loaded MIDI file, or an empty file object if no MIDI file has been loaded.
 */

juce::File EnsembleModel::getMidiFile()
{
    return midiFile;
}

//==============================================================================
// Processing and Playback

/**
 * @brief Prepare the ensemble model for playback.
 *
 * @details This function should be called before any calls to processMidiBlock.
 *
 * @param newSampleRate The sample rate at which the ensemble model should be
 * prepared for playback.
 */
void EnsembleModel::prepareToPlay(double newSampleRate)
{
    sampleRate = newSampleRate;
}

/**
 * @brief Release any resources allocated by the ensemble model.
 *
 * @details This function should be called when the ensemble model is no longer
 * needed. It is currently a no-op, but may be used in the future to release
 * resources allocated for the ensemble model.
 */
void EnsembleModel::releaseResources()
{
    return;
}

/**
 * @brief Processes a block of MIDI events and outputs the result.
 *
 * @details This function processes the input MIDI buffer for the specified number of samples,
 * updating the tempo and handling any reset flags. If intro tones are yet to be played,
 * it will prioritise them before processing the main score. The function locks the player
 * state to ensure thread safety during processing.
 *
 * @param inMidi The input MIDI buffer containing MIDI events.
 * @param outMidi The output MIDI buffer where processed MIDI events will be stored.
 * @param numSamples The number of samples to process in the MIDI buffer.
 * @param tempo The current tempo, in beats per minute, to be set for processing.
 */

void EnsembleModel::processMidiBlock(const juce::MidiBuffer &inMidi, juce::MidiBuffer &outMidi, int numSamples, double tempo)
{
    FlagLock lock(playersInUse);

    if (!lock.locked)
    {
        return;
    }

    //==============================================================================
    // Update tempo from DAW playhead.
    setTempo(tempo);

    //==============================================================================
    // Clear output if ensemble has been reset
    if (!resetFlag.test_and_set())
    {
        soundOffAllChannels(outMidi);
    }

    //==============================================================================
    // Process each sample of the buffer for each player.
    for (int i = 0; i < numSamples; ++i)
    {
        if (introTonesPlayed < numIntroTones)
        {
            playIntroTones(outMidi, i);
            playUserIntro(inMidi, outMidi, i);
            continue;
        }

        playScore(inMidi, outMidi, i);
    }
}

/**
 * @brief Reset the ensemble model and its players.
 *
 * @details This function resets the ensemble model and its players, clearing any
 * state that may have been set by processing MIDI events. The function will
 * return false if the player state is not locked, indicating that the reset
 * operation could not be completed.
 *
 * @returns true if the reset operation was successful, false if the player
 * state is not locked.
 */
bool EnsembleModel::reset()
{

    FlagLock lock(playersInUse);

    if (!lock.locked)
    {
        return false;
    }

    resetPlayers();

    return true;
}

/**
 * @brief Reset the players in the ensemble model.
 *
 * @details This function resets all players in the ensemble model, clearing any
 * state that may have been set by processing MIDI events. The following actions
 * are taken when resetting the players:
 *  - Intro countdown is reset
 *  - Score counter is reset
 *  - Player tempo is updated when playback starts
 *  - Logging of onset times is started
 *  - Polling for new alpha values is started
 *  - Each player is reset
 *  - The reset flag is cleared
 */
void EnsembleModel::resetPlayers()
{
    //==========================================================================
    // Initialise intro countdown
    introCounter = 0; //-sampleRate / 2;
    introTonesPlayed = 0;

    // Initialise score counter
    scoreCounter = 0;

    // make sure to update player tempo when playback starts
    initialTempoSet = false;

    // Start loop for logging onset times or each player
    startLoggerLoop();

    // Start loop which polls for new alpha values
    startPollingLoop();

    //==========================================================================
    // reset all players
    for (auto &player : players)
    {
        player->reset();
    }

    resetFlag.clear();
}

/**
 * @brief Sends MIDI messages to turn off all notes, sound, and controllers on all channels.
 *
 * @details This function iterates through all 16 MIDI channels and adds messages to
 * the provided MIDI buffer to ensure that all notes, sound, and controllers are turned off.
 * This is typically used to ensure that no lingering notes or sounds are present when
 * resetting or stopping playback.
 *
 * @param midi The MIDI buffer where the "all off" messages should be added.
 */

void EnsembleModel::soundOffAllChannels(juce::MidiBuffer &midi)
{
    for (int channel = 1; channel <= 16; ++channel)
    {
        midi.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        midi.addEvent(juce::MidiMessage::allSoundOff(channel), 0);
        midi.addEvent(juce::MidiMessage::allControllersOff(channel), 0);
    }
}

/**
 * @brief Check if a MIDI sequence contains any note on events.
 *
 * @details This function takes a pointer to a MIDI sequence and iterates through
 * all events in the sequence. If it finds a note on event, it immediately returns
 * `true`. If the entire sequence is iterated through and no note on events are
 * found, it returns `false`.
 *
 * @param seq The MIDI sequence to be checked.
 *
 * @returns `true` if the sequence contains any note on events, `false` otherwise.
 */
bool EnsembleModel::checkMidiSequenceHasNotes(const juce::MidiMessageSequence *seq)
{
    for (auto event : *seq)
    {
        if (event->message.isNoteOn())
        {
            return true;
        }
    }

    return false;
}

//==============================================================================
// Processing and Playback

/**
 * @brief Get the number of players in the ensemble model.
 *
 * @details This function simply returns the number of players in the ensemble
 * model. This is equivalent to the number of tracks in the loaded MIDI file.
 *
 * @returns The number of players in the ensemble model.
 */
int EnsembleModel::getNumPlayers()
{
    return static_cast<int>(players.size());
}

/**
 * @brief Get the number of user players in the ensemble model.
 *
 * @details This function returns the number of user players in the ensemble model.
 * This is equivalent to the number of players that are being controlled by the
 * user.
 *
 * @returns The number of user players in the ensemble model.
 */
int EnsembleModel::getNumUserPlayers()
{
    return static_cast<int>(numUserPlayers);
}

/**
 * @brief Determine if a player is user-operated or not.
 *
 * @details This function takes a player index and returns true if the player is
 * user-operated, false if the player is not user-operated.
 *
 * @param playerIndex The index of the player to check.
 *
 * @returns true if the player is user-operated, false if the player is not
 * user-operated.
 */
bool EnsembleModel::isPlayerUserOperated(int playerIndex)
{
    return players[playerIndex]->isUserOperated();
}

/**
 * @brief Set the alpha and beta parameters for all players to a given value.
 *
 * @details This function sets the alpha and beta parameters for all players to a
 * given value. This is a convenience function for setting all of the parameters
 * at once.
 *
 * @param valueIn The value to set the alpha and beta parameters to.
 */
void EnsembleModel::setAlphaBetaParams(float valueIn)
{
    for (int i = 0; i < processor->MAX_PLAYERS; i++)
    {
        for (int j = 0; j < processor->MAX_PLAYERS; j++)
        {
            *processor->alphaParameter(i, j) = valueIn;
        }
    }
}

/**
 * @brief Get the channel parameter for a given player.
 *
 * @param playerIndex The index of the player to get the channel parameter for.
 * @return juce::AudioParameterInt&
 */
juce::AudioParameterInt &EnsembleModel::getPlayerChannelParameter(int playerIndex)
{
    return *processor->channelParameter(playerIndex);
}

/**
 * @brief Get the delay parameter for a given player.
 *
 * @param playerIndex The index of the player to get the delay parameter for.
 * @return juce::AudioParameterFloat&
 */
juce::AudioParameterFloat &EnsembleModel::getPlayerDelayParameter(int playerIndex)
{
    return *processor->delayParameter(playerIndex);
}

/**
 * @brief Get the motor noise parameter for a given player.
 *
 * @param playerIndex The index of the player to get the motor noise parameter for.
 * @return juce::AudioParameterFloat&
 */
juce::AudioParameterFloat &EnsembleModel::getPlayerMotorNoiseParameter(int playerIndex)
{
    return *processor->mNoiseStdParameter(playerIndex);
}

/**
 * @brief Get the time keeper noise parameter for a given player.
 *
 * @param playerIndex The index of the player to get the time keeper noise parameter for.
 * @return juce::AudioParameterFloat&
 */
juce::AudioParameterFloat &EnsembleModel::getPlayerTimeKeeperNoiseParameter(int playerIndex)
{
    return *processor->tkNoiseStdParameter(playerIndex);
}

/**
 * @brief Get the volume parameter for a given player.
 *
 * @param playerIndex The index of the player to get the volume parameter for.
 * @return juce::AudioParameterFloat&
 */
juce::AudioParameterFloat &EnsembleModel::getPlayerVolumeParameter(int playerIndex)
{
    return *processor->volumeParameter(playerIndex);
}

/**
 * @brief Get the alpha parameter for a given player pair.
 *
 * @param player1Index The index of the first player.
 * @param player2Index The index of the second player.
 * @return juce::AudioParameterFloat&
 */
juce::AudioParameterFloat &EnsembleModel::getAlphaParameter(int player1Index, int player2Index)
{
    return *processor->alphaParameter(player1Index, player2Index);
}

/**
 * @brief Get the beta parameter for a given player pair.
 *
 * @param player1Index The index of the first player.
 * @param player2Index The index of the second player.
 * @return juce::AudioParameterFloat&
 */
juce::AudioParameterFloat &EnsembleModel::getBetaParameter(int player1Index, int player2Index)
{
    return *processor->betaParameter(player1Index, player2Index);
}

//==============================================================================
// OSC Messaging

/**
 * @brief Initialises the OSC listener for the ensemble model.
 *
 * @details This function is called when the OSC sender is connected for the
 * first time. It adds listeners for all of the OSC addresses that the ensemble
 * model is interested in. The listeners are used to receive messages from the
 * Max patcher that control the ensemble model.
 */
void EnsembleModel::initialiseOSC()
{
    // OSC Listener addresses
    OSCReceiver.addListener(this, "/loadConfig");
    OSCReceiver.addListener(this, "/reset");
    OSCReceiver.addListener(this, "/setLogname");
    OSCReceiver.addListener(this, "/numIntroTones");

    // New OSC Listener Addresses
    OSCReceiver.addListener(this, "/loadMidiFile");
    OSCReceiver.addListener(this, "/ensembleDetails");
    OSCReceiver.addListener(this, "/playerDetails");
    OSCReceiver.addListener(this, "/ping");
}

/**
 * @brief Connects the OSC sender to a given IP address and port number.
 *
 * @details This function connects the OSC sender to a given IP address and
 * port number. If the connection is successful, it also initialises the OSC
 * receiver for the ensemble model, if it has not already been initialised.
 *
 * @param portNumber The port number to connect to.
 * @param IPAddress The IP address to connect to.
 */
void EnsembleModel::connectOSCSender(int portNumber, juce::String IPAddress)
{
    if (!OSCSender.connect(IPAddress, portNumber))
        DBG("Error: could not connect to UDP port " << portNumber);
    else
    {
        DBG("OSC SENDER CONNECTED");

        // Initialises a reciever on the same port and localhost.
        if (!isOSCInitialised)
        {
            initialiseOSC();
        }
    }
}

/**
 * @brief Connects the OSC receiver to a given port number.
 *
 * @details This function connects the OSC receiver to a given port number.
 * If the connection is successful, it also initialises the OSC receiver for
 * the ensemble model, if it has not already been initialised.
 *
 * @param portNumber The port number to connect to.
 */
void EnsembleModel::connectOSCReceiver(int portNumber)
{
    // Connection can be established via config file parameter "OSCReceivePort"
    if (!OSCReceiver.connect(portNumber))
    {
        currentReceivePort = -1;
        DBG("Error: could not connect to UDP.");
    }
    else
    {
        sendActionMessage("OSC Received");
        currentReceivePort = portNumber;

        if (!isOSCInitialised)
        { // Initialises the OSC Receiver to respond to messages it receives.
            initialiseOSC();
        }

        DBG("Connection succeeded");
    }
}

/**
 * @brief Processes received OSC messages.
 *
 * @details This function handles incoming OSC messages by interpreting the
 * address pattern and parameters within the message. Depending on the OSC
 * address, it performs various actions, such as loading configuration files,
 * setting log filenames, loading MIDI files, sending player details, and more.
 * It also handles simple commands without parameters, like reset, ensemble
 * details, and ping. After processing, it acknowledges the OSC message.
 *
 * @param message The OSC message that has been received, containing the address
 * and parameters to be processed.
 */

void EnsembleModel::oscMessageReceived(const juce::OSCMessage &message)
{
    juce::OSCAddressPattern oscPattern = message.getAddressPattern();
    juce::String oscAddress = oscPattern.toString();

    if (message.size() > 0)
    {
        if (oscAddress == "/loadConfig" && message[0].isString())
        {
            auto configFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                  .getChildFile(configSubfolder)
                                  .getChildFile(message[0].getString());

            if (configFile.existsAsFile())
            {
                loadConfigFromXml(configFile);
            }
        }
        else if (oscAddress == "/setLogname" && message[0].isString())
        {
            logFilenameOverride = message[0].getString();
            if (!logFilenameOverride.endsWith(".csv"))
            {
                logFilenameOverride << ".csv";
            }
        }
        else if (oscAddress == "/numIntroTones" && message[0].isInt32())
        {
            numIntroTones = message[0].getInt32();
        }
        else if (oscAddress == "/loadMidiFile" && message[0].isString())
        {
            juce::File file(message[0].getString());
            if (file.existsAsFile())
            {
                juce::FileInputStream inputStream(file);
                if (inputStream.openedOk())
                {
                    loadMidiFile(file, numUserPlayers);
                    DBG("MIDI file successfully loaded");
                }
                else
                {
                    DBG("Failed to open MIDI file stream.");
                }
            }
        }
        else if (oscAddress == "/playerDetails" && message[0].isInt32())
        {
            int playerIndex = message[0].getInt32();
            DBG(playerIndex);
            OSCSender.send(getPlayerInfo(playerIndex).getOSCMessage());
        }
    }

    // Handle simple commands without parameters
    if (oscAddress == "/reset")
    {
        reset();
    }
    else if (oscAddress == "/ensembleDetails")
    {
        oscSendEnsembleDetails();
    }
    else if (oscAddress == "/ping")
    {
        juce::OSCMessage pongMsg("/pong");
        OSCSender.send(pongMsg);
    }

    oscAcknowledge(oscAddress);
    sendActionMessage("OSC Received");
}

/**
 * Send an OSC message to confirm that an OSC message was received.
 * @param receivedAddress the address of the OSC message received
 */
void EnsembleModel::oscAcknowledge(const juce::String &receivedAddress)
{
    juce::OSCMessage ackMessage("/oscAcknowledgment", receivedAddress);
    OSCSender.send(ackMessage);
}

/**
 * @brief Sends an OSC message with all ensemble model details to the connected
 * OSC receiver.
 *
 * @details This function creates an OSC message with all the relevant ensemble
 * model details and sends it to the connected OSC receiver. The ensemble model
 * details are: current receive port, number of user players, path to the MIDI file,
 * sample rate, samples per beat, score counter, initial tempo set, intro tone
 * channel, number of intro tones, intro tone note (first), intro tone note (other),
 * intro tone velocity, intro counter, and intro tones played.
 */
void EnsembleModel::oscSendEnsembleDetails()
{
    juce::OSCMessage message("/ensembleDetails");
    // Add all variables to the message
    message.addInt32(currentReceivePort);
    message.addInt32(numUserPlayers);
    message.addString(midiFilePath.getFullPathName());
    message.addFloat32(sampleRate);
    message.addInt32(samplesPerBeat);
    message.addInt32(scoreCounter);
    message.addInt32(initialTempoSet);
    message.addInt32(introToneChannel);
    message.addInt32(numIntroTones);
    message.addInt32(introToneNoteFirst);
    message.addInt32(introToneNoteOther);
    message.addInt32(introToneVel);
    message.addInt32(introCounter);
    message.addInt32(introTonesPlayed);

    // Send the OSC message
    OSCSender.send(message);
}

/**
 * @brief Checks if the OSC receiver is connected and ready to receive messages.
 *
 * @returns True if the OSC receiver is connected, false otherwise.
 */
bool EnsembleModel::isOscReceiverConnected()
{
    return (currentReceivePort > -1);
}

//==============================================================================
// XML Configuration
// Loading requires converting: xml file -> xmlDocument -> xmlElement

// Formats the current ensemble state to xml, and saves it to a file (currently a default file in user folder)
// Note: This currently only saves alpha and beta parameters.

/**
 * @brief Saves the current ensemble state to an XML file.
 *
 * This function saves the number of user players, and the alpha and beta parameters
 * for all players to an XML file. The XML file is currently saved in the user's
 * documents directory with the name "EnsembleModelConfig.xml".
 *
 * Note: This only works on Windows. On other platforms, the file is not saved.
 */
void EnsembleModel::saveConfigToXmlFile()
{
#ifdef JUCE_WINDOWS
    auto xmlOutput = &juce::XmlElement("EnsembleModelConfig");
    xmlOutput->setAttribute("numUserPlayers", numUserPlayers);

    auto xmlAlphas = xmlOutput->createNewChildElement("Alphas");
    auto xmlBetas = xmlOutput->createNewChildElement("Betas");
    for (int i = 0; i < players.size(); ++i)
    {
        for (int j = 0; j < players.size(); ++j)
        {
            float alpha = getAlphaParameter(i, j);
            float beta = getBetaParameter(i, j);

            juce::String xmlAlphaEntryName;
            juce::String xmlBetaEntryName;

            xmlAlphaEntryName << "Alpha_" << i << "_" << j;
            xmlBetaEntryName << "Beta_" << i << "_" << j;

            xmlAlphas->setAttribute(xmlAlphaEntryName, alpha);
            xmlBetas->setAttribute(xmlBetaEntryName, beta);
        }
    }

    auto ensembleConfigFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsembleModelConfig.xml");
    xmlOutput->writeTo(ensembleConfigFile);
#endif
}

// loadConfigFromXml can be called directly with XmlElement ... or from a File via parseXmlConfigFileToXmlElement

/**
 * @brief Loads an XML configuration file and updates the ensemble model with the values found in the file.
 *
 * @details This function takes a juce::File object pointing to the configuration file to be loaded. The file is parsed into an XML document, and then the ensemble model is updated with the values found in the document.
 *
 * @param configFile The juce::File object pointing to the configuration file to be loaded.
 */
void EnsembleModel::loadConfigFromXml(juce::File configFile)
{
    loadConfigFromXml(parseXmlConfigFileToXmlElement(configFile));
}

// Main method to load an XML config file

/**
 * @brief Loads an XML configuration file and updates the ensemble model with the values found in the file.
 *
 * @details This function takes a unique pointer to an XmlElement representing the configuration file to be loaded. The values in the document are used to update the ensemble model.
 *
 * @param loadedConfig The unique pointer to the XmlElement representing the configuration file to be loaded.
 */
void EnsembleModel::loadConfigFromXml(std::unique_ptr<juce::XmlElement> loadedConfig)
{
    if (loadedConfig == nullptr)
    {
        return;
    }

    // Flag to keep track if list of players needs to be reinitialised (e.g. number of user players has changed)
    bool playersNeedRecreating = false;
    bool ensembleNeedsResetting = false;

    // "LogSubfolder": Check if new config specifies a new subfolder to save logs to
    if (loadedConfig->hasAttribute("LogSubfolder"))
    {
        auto newLogSubfolder = loadedConfig->getStringAttribute("LogSubfolder", "");
        if (newLogSubfolder != "")
        {
            logSubfolder = newLogSubfolder;
        }
    }

    // "LogSubfolder": Check if new config specifies a new subfolder to save logs to
    if (loadedConfig->hasAttribute("numIntroTones"))
    {
        numIntroTones = loadedConfig->getIntAttribute("numIntroTones", 7);
        ;
    }

    // "ConfigSubfolder": Check if new config specifies new subfolder to look for config and midi files
    if (loadedConfig->hasAttribute("ConfigSubfolder"))
    {
        auto newConfigSubfolder = loadedConfig->getStringAttribute("ConfigSubfolder", "");
        if (newConfigSubfolder != "")
        {
            configSubfolder = newConfigSubfolder;
        }
    }

    // "LogFilename": Check if log filename should be overriden from default
    if (loadedConfig->hasAttribute("LogFilename"))
    {
        auto newLogFilename = loadedConfig->getStringAttribute("LogFilename", "");
        if (newLogFilename != "")
        {
            if (!newLogFilename.endsWith(".csv"))
            {
                newLogFilename << ".csv";
            }
            logFilenameOverride = newLogFilename;
        }
    }

    // "OSCReceivePort":
    // Check if new OSC connections requested
    if (loadedConfig->hasAttribute("OSCReceivePort"))
    {
        auto newOSCReceiverPort = loadedConfig->getIntAttribute("OSCReceivePort");
        if (newOSCReceiverPort != 0)
        {
            connectOSCReceiver(newOSCReceiverPort);
        }
    }

    // "OSCSenderPort":
    // Check if new OSC connections requested for sending messages
    if (loadedConfig->hasAttribute("OSCSenderPort"))
    {
        auto newOSCSenderrPort = loadedConfig->getIntAttribute("OSCSenderPort");
        if (newOSCSenderrPort != 0)
        {
            connectOSCSender(newOSCSenderrPort, "127.0.0.1");
        }
    }

    // "NumUserPlayers": Check if numUserPlayers has changed
    if (loadedConfig->hasAttribute("NumUserPlayers"))
    {
        numUserPlayers = loadedConfig->getIntAttribute("NumUserPlayers");
        DBG("User Players: " << numUserPlayers);
        playersNeedRecreating = true;
    }

    // "MidiFilename": Check if new midi file has been specified in config, and load it.
    if (loadedConfig->hasAttribute("MidiFilename"))
    {
        auto midiFilename = loadedConfig->getStringAttribute("MidiFilename");
        auto midiFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(configSubfolder).getChildFile(midiFilename);

        if (!midiFile.existsAsFile())
        {
            return;
        }

        loadMidiFile(midiFile, numUserPlayers);

        // Players are automatically reinitialised when a new midi file is loaded, so flag can be set back to false
        playersNeedRecreating = false;
    }

    // Limit number of user players to the number of available tracks in the loaded midi file
    if (numUserPlayers > midiFile.getNumTracks())
    {
        numUserPlayers = midiFile.getNumTracks();
    }

    if (playersNeedRecreating)
    {
        createPlayers(midiFile);
        reset();
    }

    // "Alphas" and "Betas":
    auto xmlAlphas = loadedConfig->getChildByName("Alphas");
    auto xmlBetas = loadedConfig->getChildByName("Betas");

    for (int i = 0; i < players.size(); ++i)
    {
        for (int j = 0; j < players.size(); ++j)
        {
            juce::String xmlAlphaEntryName;
            juce::String xmlBetaEntryName;

            xmlAlphaEntryName << "Alpha_" << i << "_" << j;
            xmlBetaEntryName << "Beta_" << i << "_" << j;

            // If corresponding entries are not found in xml, do not change value
            if (xmlAlphas != nullptr)
            {
                if (xmlAlphas->hasAttribute(xmlAlphaEntryName))
                {
                    *processor->alphaParameter(i, j) = xmlAlphas->getDoubleAttribute(xmlAlphaEntryName);
                }
            }
            if (xmlBetas != nullptr)
            {
                if (xmlBetas->hasAttribute(xmlBetaEntryName))
                {
                    *processor->betaParameter(i, j) = xmlBetas->getDoubleAttribute(xmlBetaEntryName);
                }
            }
        }
    }

    // "Motor" and "Timekeeper" noise:
    auto xmlTkNoise = loadedConfig->getChildByName("tkNoise");
    auto xmlMNoise = loadedConfig->getChildByName("mNoise");

    for (int i = 0; i < players.size(); ++i)
    {
        juce::String xmlTkNoiseEntryName;
        juce::String xmlMNoiseEntryName;

        xmlTkNoiseEntryName << "tkNoise_" << i;
        xmlMNoiseEntryName << "mNoise_" << i;

        // If corresponding entries are not found in xml, do not change value
        if (xmlTkNoise != nullptr)
        {
            if (xmlTkNoise->hasAttribute(xmlTkNoiseEntryName))
            {
                *processor->tkNoiseStdParameter(i) = xmlTkNoise->getDoubleAttribute(xmlTkNoiseEntryName);
            }
        }
        if (xmlMNoise != nullptr)
        {
            if (xmlMNoise->hasAttribute(xmlMNoiseEntryName))
            {
                *processor->mNoiseStdParameter(i) = xmlMNoise->getDoubleAttribute(xmlTkNoiseEntryName);
            }
        }
    }

    sendActionMessage("Ensemble Reset");

    if (ensembleNeedsResetting)
    {
        reset();
    }
}

// Converts a .xml file to xmlElement (to be used in loadConfigFromXml)

/**
 * @brief Converts an XML configuration file into an XmlElement.
 *
 * @details This function takes a juce::File object representing the XML configuration
 * file and parses it into an XmlElement. This XmlElement can then be used to
 * update the ensemble model configuration or for other processing needs.
 *
 * @param configFile The juce::File object representing the XML configuration file.
 * @return A unique pointer to the parsed XmlElement.
 */

std::unique_ptr<juce::XmlElement> EnsembleModel::parseXmlConfigFileToXmlElement(juce::File configFile)
{
    return juce::XmlDocument(configFile).getDocumentElement();
}

//==============================================================================
// Timing and Playback

/**
 * @brief Set the tempo of the ensemble model.
 *
 * @details This function sets the tempo of the ensemble model, specified in beats
 * per minute (bpm). It updates the number of samples per beat and resets the
 * initial tempo of all players.
 *
 * @param bpm The tempo in beats per minute.
 */
void EnsembleModel::setTempo(double bpm)
{
    int newSamplesPerBeat = 60.0 * sampleRate / bpm;

    // Check tempo has actually changed.
    if (newSamplesPerBeat == samplesPerBeat)
    {
        return;
    }

    // Update tempo of playback.
    samplesPerBeat = newSamplesPerBeat;

    setInitialPlayerTempo();
}

/**
 * @brief Set the initial tempo of all players in the ensemble model.
 *
 * @details This function sets the initial tempo of all players in the ensemble
 * model, specified in samples per beat. It is called by the setTempo function
 * after checking that the tempo has actually changed.
 */
void EnsembleModel::setInitialPlayerTempo()
{
    if (!initialTempoSet)
    {
        for (auto &player : players)
        {
            player->setOnsetInterval(samplesPerBeat);
        }

        initialTempoSet = true;
    }
}

/**
 * @brief Checks if new onsets are available for all players in the ensemble model.
 *
 * @details This function iterates over all players in the ensemble model and
 * checks if each player has a new onset available. If all players have a new onset
 * available, the function returns true. Otherwise, it returns false.
 *
 * @return True if new onsets are available for all players, false otherwise.
 */
bool EnsembleModel::newOnsetsAvailable()
{
    bool available = true;

    for (auto &player : players)
    {
        available = available && player->hasNotePlayed();
    }

    return available;
}

/**
 * @brief Calculate new onset times for players in the ensemble model.
 *
 * @details This function gets the most recent alphas from the processor and
 * then calculates new onset times for all players in the ensemble model. It
 * first updates the non-user players and then the user players. Finally, it
 * adds details of the most recent onsets to buffers to be logged.
 */
void EnsembleModel::calculateNewIntervals()
{
    //==========================================================================
    // Get most recent alphas
    getLatestAlphas();

    //==========================================================================
    // Calculate new onset times for players.
    // Make sure all non-user players update before the user players.
    for (int i = 0; i < players.size(); ++i)
    {
        if (!players[i]->isUserOperated())
        {
            //            players [i]->recalculateOnsetInterval (samplesPerBeat, players, alphaParams [i], betaParams [i]);
            players[i]->recalculateOnsetInterval(samplesPerBeat, players);
        }
    }

    for (int i = 0; i < players.size(); ++i)
    {
        if (players[i]->isUserOperated())
        {
            //            players [i]->recalculateOnsetInterval (samplesPerBeat, players, (*alphaParams) [i], (*betaParams) [i]);
            players[i]->recalculateOnsetInterval(samplesPerBeat, players);
        }
    }

    //==========================================================================
    // Add details of most recent onsets to buffers to be logged.
    if (loggingFifo)
    {
        auto writer = loggingFifo->write(static_cast<int>(players.size()));

        int p = 0;

        for (int i = 0; i < writer.blockSize1; ++i)
        {
            storeOnsetDetailsForPlayer(writer.startIndex1 + i, p++);
        }

        for (int i = 0; i < writer.blockSize2; ++i)
        {
            storeOnsetDetailsForPlayer(writer.startIndex2 + i, p++);
        }
    }
}

/**
 * @brief Reset note played status for all players in the ensemble model.
 *
 * @details This function iterates over all players in the ensemble model and
 * resets their note played status. It is called by the calculateNewIntervals
 * function after calculating new onset times for all players.
 */
void EnsembleModel::clearOnsetsAvailable()
{
    for (auto &player : players)
    {
        player->resetNotePlayed();
    }
}

//==============================================================================
// Intro Tones

/**
 * @brief Plays intro tones at the start of the piece.
 *
 * @details This function plays a repeating pattern of two notes at the start
 * of the piece. The pattern is played on a separate midi channel to the
 * rest of the piece. The function is called by the processBlock function and
 * is used to play the intro tones.
 *
 * @param [in] midi a reference to a juce::MidiBuffer object
 * @param [in] sampleIndex an integer representing the sample index to add
 * the midi event at
 */
void EnsembleModel::playIntroTones(juce::MidiBuffer &midi, int sampleIndex)
{

    if (introCounter == 0)
    {
        introToneOn(midi, sampleIndex);
    }
    else if (introCounter == samplesPerBeat / 4)
    {
        introToneOff(midi, sampleIndex);
    }
    else if (introCounter >= samplesPerBeat - 1)
    {
        ++introTonesPlayed;
        introCounter = -1;
    }

    ++introCounter;
}

/**
 * @brief Turns on the intro tone.
 *
 * @details This function turns on either the first or second note of the intro
 * tone, depending on whether the number of intro tones played is divisible by
 * 4. The function is called by the playIntroTones function and is used to play
 * the intro tones.
 *
 * @param [in] midi a reference to a juce::MidiBuffer object
 * @param [in] sampleIndex an integer representing the sample index to add
 * the midi event at
 */
void EnsembleModel::introToneOn(juce::MidiBuffer &midi, int sampleIndex)
{
    if (introTonesPlayed % 4 == 0)
    {
        midi.addEvent(juce::MidiMessage::noteOn(introToneChannel, introToneNoteFirst, introToneVel), sampleIndex);
    }
    else
    {
        midi.addEvent(juce::MidiMessage::noteOn(introToneChannel, introToneNoteOther, introToneVel), sampleIndex);
    }
}

/**
 * @brief Turns off the intro tone.
 *
 * @details This function sends a MIDI note-off message for the intro tone.
 * It determines whether to turn off the first or second note of the intro
 * tone based on the number of intro tones played. If the number of intro
 * tones played is divisible by 4, it turns off the first note; otherwise,
 * it turns off the second note. This function is called by the playIntroTones
 * function as part of the intro tones playback process.
 *
 * @param [in] midi a reference to a juce::MidiBuffer object
 * @param [in] sampleIndex an integer representing the sample index to add
 * the midi event at
 */

void EnsembleModel::introToneOff(juce::MidiBuffer &midi, int sampleIndex)
{
    if (introTonesPlayed % 4 == 0)
    {
        midi.addEvent(juce::MidiMessage::noteOff(introToneChannel, introToneNoteFirst, introToneVel), sampleIndex);
    }
    else
    {
        midi.addEvent(juce::MidiMessage::noteOff(introToneChannel, introToneNoteOther, introToneVel), sampleIndex);
    }
}

//==============================================================================
// Player Management

/**
 * @brief Class for locking the playersInUse flag.
 *
 * @details This class is used to lock the playersInUse flag when the
 * EnsembleModel needs to access or modify the player objects. The
 * flag is cleared when the destructor is called.
 *
 * @param [in] f a reference to a std::atomic_flag object
 */
EnsembleModel::FlagLock::FlagLock(std::atomic_flag &f)
    : flag(f),
      locked(!flag.test_and_set())
{
}

/**
 * @brief Destructor for FlagLock class.
 *
 * @details The destructor clears the locked flag to unlock the playersInUse
 * flag. This allows the EnsembleModel to access or modify the player objects
 * again.
 */
EnsembleModel::FlagLock::~FlagLock()
{
    flag.clear();
}

/**
 * @brief Create a Player for each track in the file which has note on events.
 *
 * @details This function is called by the loadMidiFile function and is used to
 * create a Player object for each track in the MIDI file that has note on
 * events. The function first deletes any existing players and then creates a
 * new Player object for each track in the file. The function assigns channels
 * to players in a cyclical manner and creates a UserPlayer object if the
 * player index is less than the number of user players; otherwise, it creates a
 * Player object.
 *
 * @param [in] file a reference to a juce::MidiFile object
 */
void EnsembleModel::createPlayers(const juce::MidiFile &file)
{
    //==========================================================================
    // Delete Old Players
    players.clear();

    //==========================================================================
    // Create a Player for each track in the file which has note on events.
    int nTracks = file.getNumTracks();
    int playerIndex = 0;

    for (int i = 0; i < nTracks; ++i)
    {
        auto track = file.getTrack(i);

        if (checkMidiSequenceHasNotes(track))
        {
            // Assing channels to players in a cyclical manner.
            int channelToUse = (playerIndex % 16) + 1;

            if (playerIndex < numUserPlayers)
            {
                players.push_back(std::make_unique<UserPlayer>(playerIndex++,
                                                               track,
                                                               channelToUse,
                                                               sampleRate,
                                                               scoreCounter,
                                                               samplesPerBeat,
                                                               processor));
            }
            else
            {
                players.push_back(std::make_unique<Player>(playerIndex++,
                                                           track,
                                                           channelToUse,
                                                           sampleRate,
                                                           scoreCounter,
                                                           samplesPerBeat,
                                                           processor));
            }
        }
    }

    //==========================================================================
    createAlphaBetaParameters(); // create matrix of parameters for alphas
}

// Initialise matrix of alpha and beta parameters

/**
 * @brief Initialise matrix of alpha and beta parameters
 *
 * This function is called immediately after the players vector is populated.
 * It sets the alpha and beta parameters for each player to a given value.
 * The value is set to a default value of 0.25 for alpha and 0.1 for beta.
 * If the user has specified a custom value for alpha and/or beta, then
 * this function is not called.
 *
 * @see setAlphaBetaParams
 */
void EnsembleModel::createAlphaBetaParameters()
{
    for (int i = 0; i < players.size(); ++i)
    {
        double alpha = 0.25;
        double beta = 0.1;

        for (int j = 0; j < players.size(); ++j)
        {
            *processor->alphaParameter(i, j) = alpha;
            *processor->betaParameter(i, j) = beta;
        }
    }
}

/**
 * @brief Poll the buffer for new alpha values and update the alpha parameters accordingly.
 *
 * This function is called by the processor to get the latest alpha values from the buffer.
 * It is used in the polling thread to update the parameters in the plugin.
 *
 * @see startPollingLoop
 * @see stopPollingLoop
 * @see initialisePollingBuffers
 */
void EnsembleModel::getLatestAlphas()
{
    //    if (pollingFifo)
    //    {
    //        // Consume everything in the buffer, only using the most recent set of alphas.
    //        auto reader = pollingFifo->read (pollingFifo->getNumReady());
    //
    //        for (int player1 = 0; player1 < pollingBuffer.size(); ++player1)
    //        {
    //            int player2 = 0;
    //
    //            int block1Start = std::max (reader.blockSize1 + reader.blockSize2 - static_cast <int> (players.size()), 0);
    //
    //            for (int i = block1Start; i < reader.blockSize1; ++i)
    //            {
    //                *(*alphaParams) [player1][player2++] = pollingBuffer [player1][reader.startIndex1 + i];
    //            }
    //
    //            int block2Start = std::max (block1Start - reader.blockSize1, 0);
    //
    //            for (int i = block2Start; i < reader.blockSize2; ++i)
    //            {
    //                *(*alphaParams) [player1][player2++] = pollingBuffer [player1][reader.startIndex2 + i];
    //            }
    //        }
    //    }
}

/**
 * @brief Store the onset details of a specific player into the logging buffer.
 *
 * @details This function retrieves the latest onset details of a specified player
 * and stores them in the logging buffer at the given buffer index. It logs various
 * parameters including onset time, interval, user input status, delay, motor noise,
 * time keeper noise, asynchronies with other players, alpha and beta parameters,
 * time keeper noise standard deviation, motor noise standard deviation, and volume.
 *
 * @param bufferIndex The index in the logging buffer where the onset details are to be stored.
 * @param playerIndex The index of the player whose onset details are being logged.
 */

void EnsembleModel::storeOnsetDetailsForPlayer(int bufferIndex, int playerIndex)
{
    // Store the log information about the latest onset from the given player
    // in the logging buffers.
    auto &data = loggingBuffer[bufferIndex];

    data.onsetTime = players[playerIndex]->getLatestOnsetTime();
    data.onsetInterval = players[playerIndex]->getPlayedOnsetInterval();
    data.userInput = players[playerIndex]->wasLatestOnsetUserInput();
    data.delay = players[playerIndex]->getLatestOnsetDelay();
    data.motorNoise = players[playerIndex]->getMotorNoise();
    data.timeKeeperNoise = players[playerIndex]->getTimeKeeperNoise();

    for (int i = 0; i < players.size(); ++i)
    {
        data.asyncs[i] = players[playerIndex]->getLatestOnsetTime() - players[i]->getLatestOnsetTime();
        //        data.alphas [i] = *(*alphaParams) [playerIndex][i];
        data.alphas[i] = processor->alphaParameter(playerIndex, i)->get();

        //        data.betas [i] = *(*betaParams) [playerIndex][i];
        data.betas[i] = processor->betaParameter(playerIndex, i)->get();
    }

    data.tkNoiseStd = players[playerIndex]->getTimeKeeperNoiseStd();
    data.mNoiseStd = players[playerIndex]->getMotorNoiseStd();
    data.volume = players[playerIndex]->getLatestVolume();
}

// Called from EnsembleModel::processMidiBlock

/**
 * @brief Processes the current sample index for all players and updates score timing.
 *
 * @details This function iterates over all players and processes their samples at the given
 * sample index. Once all players have played a note, it updates the timing intervals and
 * clears the onset availability status. The function also increments the score counter.
 *
 * @param inMidi The input MIDI buffer containing MIDI events.
 * @param outMidi The output MIDI buffer where processed MIDI events will be stored.
 * @param sampleIndex The current sample index being processed for each player.
 */

void EnsembleModel::playScore(const juce::MidiBuffer &inMidi, juce::MidiBuffer &outMidi, int sampleIndex)
{
    for (auto &player : players)
    {
        player->processSample(inMidi, outMidi, sampleIndex);
    }

    // If all players have played a note, update timings.
    if (newOnsetsAvailable())
    {
        calculateNewIntervals();
        clearOnsetsAvailable();
    }

    ++scoreCounter;
}

/**
 * @brief Plays the intro tone for all user-operated players.
 *
 * @details Given a sample index, this function iterates over all players and
 * plays the intro tone for those which are user-operated. The intro tone is
 * played on the note specified by the introToneNoteOther parameter.
 *
 * @param inMidi The input MIDI buffer containing MIDI events.
 * @param outMidi The output MIDI buffer where processed MIDI events will be stored.
 * @param sampleIndex The current sample index being processed for each player.
 */
void EnsembleModel::playUserIntro(const juce::MidiBuffer &inMidi, juce::MidiBuffer &outMidi, int sampleIndex)
{
    for (auto &player : players)
    {
        if (player->isUserOperated())
        {
            player->processIntroSample(inMidi, outMidi, sampleIndex, introToneNoteOther);
        }
    }
}

/**
 * @brief Gets the parameters of a player given a player index.
 *
 * @details Given a player index, this function returns the parameters of the
 * player as a PlayerParameters object. The parameters include the latest onset
 * delay, timekeeper noise standard deviation, motor noise standard deviation,
 * volume, MIDI channel, and alpha and beta values for all other players.
 *
 * @param playerIndex The index of the player whose parameters will be retrieved.
 * @return A PlayerParameters object containing the parameters of the specified
 * player.
 */
EnsembleModel::PlayerParameters EnsembleModel::getPlayerInfo(int playerIndex)
{
    DBG("Player Index: " << playerIndex);
    // Similar to storeOnsetDetailsForPlayers but only the parameters from the GUI we want to retrieve:
    double delay = players[playerIndex]->getLatestOnsetDelay();
    double tkNoiseStd = players[playerIndex]->getTimeKeeperNoiseStd();
    double mNoiseStd = players[playerIndex]->getMotorNoiseStd();
    double volume = players[playerIndex]->getLatestVolume();
    int midiChannel = players[playerIndex]->getMidiChannel();

    std::vector<float> alphas;
    std::vector<float> betas;

    for (int i = 0; i < players.size(); ++i)
    {
        alphas[i] = processor->alphaParameter(playerIndex, i)->get();
        betas[i] = processor->betaParameter(playerIndex, i)->get();
    }

    return PlayerParameters(delay, tkNoiseStd, mNoiseStd, midiChannel, volume, alphas, betas);
}

//==============================================================================
// Logging

/**
 * @brief Initialises the logging buffer and FIFO for storing player onset details.
 *
 * @details This function sets up the logging buffer and FIFO based on the number of
 * players. It calculates the buffer size, initialises the FIFO, resizes the logging
 * buffer, and initialises each element in the buffer with vectors sized to the number
 * of players for storing asynchronies, alpha, and beta parameters.
 */

void EnsembleModel::initialiseLoggingBuffer()
{
    int bufferSize = 0;
    if (players.size() > 0)
    {
        bufferSize = static_cast<int>(4 * players.size());
    }
    else
    {
        bufferSize = 4;
    }

    loggingFifo = std::make_unique<juce::AbstractFifo>(bufferSize);

    loggingBuffer.resize(bufferSize);

    for (int i = 0; i < loggingBuffer.size(); ++i)
    {
        loggingBuffer[i].asyncs.resize(players.size(), 0.0);
        loggingBuffer[i].alphas.resize(players.size(), 0.0);
        loggingBuffer[i].betas.resize(players.size(), 0.0);
    }
}

/**
 * @brief Starts the logging loop that continuously logs player onset details to
 * a file.
 *
 * @details This function stops any existing logging loop, initializes the
 * logging buffer and FIFO, sets a flag to continue logging, and spawns a new
 * thread to run the logger loop.
 */

void EnsembleModel::startLoggerLoop()
{
    stopLoggerLoop();
    initialiseLoggingBuffer();

    continueLogging = true;
    loggerThread = std::thread([this]()
                               { this->loggerLoop(); });
}

/**
 * @brief Stops the logger loop and joins the logger thread.
 *
 * @details This function sets the continueLogging flag to false, which signals
 * the logger loop to stop. If the loggerThread is joinable, it waits for the
 * thread to finish execution by calling join.
 */

void EnsembleModel::stopLoggerLoop()
{
    continueLogging = false;

    if (loggerThread.joinable())
    {
        loggerThread.join();
    }
}

/**
 * @brief Main loop for logging player onset details to a file.
 *
 * @details This function is run in a separate thread by startLoggerLoop(). It
 * continuously logs player onset details to a file until the continueLogging
 * flag is set to false. The log file is created in the default documents folder
 * with a name in the format "Log_<time>_<date>.csv". The file is truncated if it
 * already exists. The log file header is written, and then the loop continuously
 * logs player onset details at a rate of 20 Hz.
 */
void EnsembleModel::loggerLoop()
{
    //==========================================================================
    // Expose this option to UI at some point.
    auto time = juce::Time::getCurrentTime();

    // Start with default documents folder
    juce::File logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    // Add subfolder, if this is specified
    if (logSubfolder != "")
    {
        logFile = logFile.getChildFile(logSubfolder);
        if (!logFile.exists())
        {
            logFile.createDirectory();
        }
    }

    // Check if the log filename has also been overriden via config.
    if (logFilenameOverride != "")
    {
        // TODO What happens if overriden log file already exists? Override? Create a new one with slightly different name?
        logFile = logFile.getChildFile(logFilenameOverride).getNonexistentSibling();
    }
    else
    {
        auto logFileName = time.formatted("Log_%H-%M-%S_%d%b%Y.csv");
        logFile = logFile.getChildFile(logFileName);
    }

    juce::FileOutputStream logStream(logFile);

    logStream.setPosition(0);
    logStream.truncate();

    //==========================================================================
    // Write log file
    writeLogHeader(logStream);

    logLineCounter = 0;

    while (continueLogging)
    {
        logOnsetDetails(logStream);
        std::this_thread::sleep_for(50ms);
    }
}

/**
    Write the header line of the log file. This function is only called
    from the logging thread.

    @param logStream The file stream to write to.
*/
void EnsembleModel::writeLogHeader(juce::FileOutputStream &logStream)
{
    juce::String logLine("N");
    juce::String onsetLog, intervalLog, userInputLog, delayLog,
        mNoiseLog, tkNoiseLog, asyncLog, alphaLog, betaLog,
        tkNoiseStdLog, mNoiseStdLog, velocityLog;

    for (int i = 0; i < players.size(); ++i)
    {
        int playerId = i + 1;

        onsetLog += ", P" + juce::String(playerId) + (players[i]->isUserOperated() ? " (input)" : "");
        intervalLog += ", P" + juce::String(playerId) + " Int";
        userInputLog += ", P" + juce::String(playerId) + " User Input";
        delayLog += ", P" + juce::String(playerId) + " Delay";
        mNoiseLog += ", P" + juce::String(playerId) + " MVar";
        tkNoiseLog += ", P" + juce::String(playerId) + " TKVar";

        for (int j = 0; j < players.size(); ++j)
        {
            int otherPlayerId = j + 1;

            asyncLog += ", Async " + juce::String(playerId) + juce::String(otherPlayerId);
            alphaLog += ", Alpha " + juce::String(playerId) + juce::String(otherPlayerId);
            betaLog += ", Beta " + juce::String(playerId) + juce::String(otherPlayerId);
        }

        tkNoiseStdLog += ", P" + juce::String(playerId) + " TKStd";
        mNoiseStdLog += ", P" + juce::String(playerId) + " MStd";
        velocityLog += ", P" + juce::String(playerId) + " Vol";
    }

    logLine += onsetLog + ", " +
               intervalLog + ", " +
               userInputLog + ", " +
               delayLog + ", " +
               mNoiseLog + ", " +
               tkNoiseLog + ", " +
               asyncLog + ", " +
               alphaLog + ", " +
               betaLog + ", " +
               tkNoiseStdLog + ", " +
               mNoiseStdLog + ", " +
               velocityLog + "\n";

    logStream.writeText(logLine, false, false, nullptr);
}

/**
 * @brief Logs details of player onsets to a file.
 *
 * @details This function reads onset data from a logging FIFO and writes the
 * details to a specified log stream. It processes the data for each player,
 * updating logs for onset times, intervals, user input status, delays, motor
 * noise, and other parameters. The function appends the processed data to a CSV
 * format log line and writes it to the log stream. After logging, it sends the
 * latest onset and delay information to a server or other destination.
 *
 * @param logStream The file stream where the log data is written.
 */

void EnsembleModel::logOnsetDetails(juce::FileOutputStream &logStream)
{
    while (loggingFifo->getNumReady() > 0)
    {
        std::vector<int> latestOnsets(players.size()), latestDelays(players.size());
        juce::String logLine(logLineCounter++);
        juce::String onsetLog, intervalLog, userInputLog, delayLog,
            mNoiseLog, tkNoiseLog, asyncLog, alphaLog, betaLog,
            tkNoiseStdLog, mNoiseStdLog, velocityLog;

        int p = 0;

        auto reader = loggingFifo->read(static_cast<int>(players.size()));

        for (int i = 0; i < reader.blockSize1; ++i)
        {
            // Append to array to send to server.
            int bufferIndex = reader.startIndex1 + i;

            auto &data = loggingBuffer[bufferIndex];
            latestOnsets[p] = data.onsetTime;
            latestDelays[p] = data.delay;
            ++p;

            // Log to log file
            logOnsetDetailsForPlayer(bufferIndex,
                                     onsetLog,
                                     intervalLog,
                                     userInputLog,
                                     delayLog,
                                     mNoiseLog,
                                     tkNoiseLog,
                                     asyncLog,
                                     alphaLog,
                                     betaLog,
                                     tkNoiseStdLog,
                                     mNoiseStdLog,
                                     velocityLog);
        }

        for (int i = 0; i < reader.blockSize2; ++i)
        {
            // Append to array to send to server.
            int bufferIndex = reader.startIndex2 + i;

            auto &data = loggingBuffer[bufferIndex];
            latestOnsets[p] = data.onsetTime;
            latestDelays[p] = data.delay;
            ++p;

            // Log to log file
            logOnsetDetailsForPlayer(bufferIndex,
                                     onsetLog,
                                     intervalLog,
                                     userInputLog,
                                     delayLog,
                                     mNoiseLog,
                                     tkNoiseLog,
                                     asyncLog,
                                     alphaLog,
                                     betaLog,
                                     tkNoiseStdLog,
                                     mNoiseStdLog,
                                     velocityLog);
        }

        logLine += onsetLog + ", " +
                   intervalLog + ", " +
                   userInputLog + ", " +
                   delayLog + ", " +
                   mNoiseLog + ", " +
                   tkNoiseLog + ", " +
                   asyncLog + ", " +
                   alphaLog + ", " +
                   betaLog + ", " +
                   tkNoiseStdLog + ", " +
                   mNoiseStdLog + ", " +
                   velocityLog + "\n";

        logStream.writeText(logLine, false, false, nullptr);

        // Send onset detail to wherever they need to go.
        postLatestOnsets(latestOnsets, latestDelays);
    }
}

/**
    Add the onset details for a given player to a set of logs. This function is
    only called from the logging thread.

    @param bufferIndex The index into the loggingBuffer array.
    @param onsetLog The log to which to add the onset time.
    @param intervalLog The log to which to add the onset interval.
    @param userInputLog The log to which to add whether the onset was user input.
    @param delayLog The log to which to add the delay of the onset.
    @param mNoiseLog The log to which to add the motor noise.
    @param tkNoiseLog The log to which to add the time keeper noise.
    @param asyncLog The log to which to add the async values.
    @param alphaLog The log to which to add the alpha values.
    @param betaLog The log to which to add the beta values.
    @param tkNoiseStdLog The log to which to add the standard deviation of the
                         time keeper noise.
    @param mNoiseStdLog The log to which to add the standard deviation of the
                        motor noise.
    @param velocityLog The log to which to add the velocity of the onset.
*/
void EnsembleModel::logOnsetDetailsForPlayer(int bufferIndex,
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
                                             juce::String &velocityLog)
{
    auto &data = loggingBuffer[bufferIndex];

    onsetLog += ", " + juce::String(data.onsetTime / sampleRate);
    intervalLog += ", " + juce::String(data.onsetInterval / sampleRate);
    userInputLog += ", " + juce::String(data.userInput ? "true" : "false");
    delayLog += ", " + juce::String(data.delay / sampleRate);
    mNoiseLog += ", " + juce::String(data.motorNoise);
    tkNoiseLog += ", " + juce::String(data.timeKeeperNoise);

    for (int i = 0; i < data.asyncs.size(); ++i)
    {
        asyncLog += "," + juce::String(data.asyncs[i] / sampleRate);
        alphaLog += "," + juce::String(data.alphas[i]);
        betaLog += "," + juce::String(data.betas[i]);
    }

    tkNoiseStdLog += ", " + juce::String(data.tkNoiseStd);
    mNoiseStdLog += ", " + juce::String(data.mNoiseStd);
    velocityLog += ", " + juce::String(data.volume);
}

// NOT IMPLEMENTED
void EnsembleModel::postLatestOnsets(const std::vector<int> &onsets, const std::vector<int> &delays)
{
    //==========================================================================
    // onsets contains the onset time in samples for each of the players' most
    // recently played notes.
    //
    // delays contains the delays for each player in samples
    //
    // Send these to the server however you want here. The current sampling
    // frequency is available in the sampleRate variable.

    // Once you've sent those to the server indicate to the polling thread that
    // it should start to poll for new alphas.
    alphasUpToDate.clear();
}

//==============================================================================
// Polling for new Alpha Values

/**
 * @brief Initialises the buffers for polling for new alpha values.
 *
 * @details The polling FIFO is created with a size of 10 times the number of
 * players. The polling buffer is resized to the same size as the number of
 * players and each player's buffer is resized to the size of the FIFO and
 * initialised to 0.0.
 */
void EnsembleModel::initialisePollingBuffers()
{
    int bufferSize = static_cast<int>(10 * players.size());

    pollingFifo = std::make_unique<juce::AbstractFifo>(bufferSize);

    pollingBuffer.resize(players.size());

    for (int i = 0; i < pollingBuffer.size(); ++i)
    {
        pollingBuffer[i].resize(bufferSize, 0.0);
    }
}

/**
 * @brief Starts the polling loop which polls for new alpha values.
 *
 * @details This function stops any existing polling loop, initialises the
 * polling buffers, sets the flag to continue polling, sets the flag to indicate
 * that the alphas are up to date and starts the polling thread.
 */
void EnsembleModel::startPollingLoop()
{
    stopPollingLoop();
    initialisePollingBuffers();

    continuePolling = true;
    alphasUpToDate.test_and_set();
    pollingThread = std::thread([this]()
                                { this->pollingLoop(); });
}

/**
 * @brief Stops the polling loop for new alpha values.
 *
 * @details This function sets the flag to stop polling and waits for the
 * polling thread to finish before returning.
 */
void EnsembleModel::stopPollingLoop()
{
    continuePolling = false;

    if (pollingThread.joinable())
    {
        pollingThread.join();
    }
}

/**
 * @brief Runs the polling loop to get new alpha values.
 *
 * @details This function runs until the continuePolling flag is false. It
 * checks if the alphas are up to date, if not it calls getNewAlphas to get the
 * new values. It then waits for 50ms before checking again. This loop is
 * started in the startPollingLoop function and stopped in the stopPollingLoop
 * function.
 */
void EnsembleModel::pollingLoop()
{
    while (continuePolling)
    {
        if (!alphasUpToDate.test_and_set())
        {
            // getNewAlphas();
        }

        std::this_thread::sleep_for(50ms);
    }
}

void EnsembleModel::getNewAlphas() // NOT USED
{
    //==========================================================================
    // In here you should make a request to your server to ask for new alpha
    // values. If you get some updated values set the following value to true.
    // If not, set the value to false and the plug-in will poll again after
    // short time.
    bool newAlphas = false;

    if (newAlphas)
    {
        auto writer = pollingFifo->write(static_cast<int>(players.size()));

        for (int player1 = 0; player1 < pollingBuffer.size(); ++player1)
        {
            int player2 = 0;

            for (int i = 0; i < writer.blockSize1; ++i)
            {
                // Replace the 0.2 with the alpha parameter for player1_player2
                pollingBuffer[player1][writer.startIndex1 + i] = 0.2;
                ++player2;
            }

            for (int i = 0; i < writer.blockSize2; ++i)
            {
                // Replace the 0.2 with the alpha parameter for player1_player2
                pollingBuffer[player1][writer.startIndex2 + i] = 0.2;
                ++player2;
            }
        }
    }
    else
    {
        alphasUpToDate.clear();
    }
}
