#include "OSCHandler.h"
#include "JuceHeader.h"
#include "EnsembleModel.h"

// OSC Handler's Constructor and Deconstructor

// Initilises with the ensemble model, as well as port and IP for Sender/Receiver
OSCHandler::OSCHandler(EnsembleModel* ensemblePtr, int portNumber):
    ensemble(ensemblePtr),
    currentPort(portNumber)
{
	// Initialises OSC Receiver and any patterns to listen for
	ConnectReceiver();

	// Initialises OSC Sender 
	ConnectSender();
}

// Disconnects the OSC Sender and Receivers
OSCHandler::~OSCHandler()
{
    receiver.disconnect();

    // Send a disconnect message to any listeners
    juce::OSCMessage pongMsg("/pong");
    sender.send(pongMsg);
    sender.disconnect();
}

// Sets the Port and IP Address of the Receiver to whatever is stored in the object
void OSCHandler::ConnectSender()
{
    // Connects locally to the port
    if (!sender.connect("127.0.0.1", currentPort))
        DBG("Error: Could not connect to UDP port " << currentPort);
    else {
        DBG("OSC Sender has been connected.");
    }
}

// Sets the Port of the Receiver to whatever is stored in the object
void OSCHandler::ConnectReceiver()
{
    if (!receiver.connect(currentPort))
    {
        currentPort = -1;
        DBG("Error: Could not connect to UDP.");
    }
    else
    {

        // Setting up specfic listeners so that it listens for these patterns
        receiver.addListener(this, "/reset");
        receiver.addListener(this, "/setLogname");
        receiver.addListener(this, "/numIntroTones");
        receiver.addListener(this, "/loadMidiFile");
        receiver.addListener(this, "/ensembleDetails");
        receiver.addListener(this, "/playerDetails");
        receiver.addListener(this, "/ping");

        DBG("OSC Connection succeeded");
    }
}

// Handles if an OSC Message has been received at provided port    
void MessageReceived(const juce::OSCMessage& message) {
    const juce::String oscAddress = message.getAddressPattern().toString();

    // Handle messages with parameters
    if (!message.isEmpty()) {
        if (message[0].isString()) {
            if (oscAddress == "/loadConfig") {}
            else if (oscAddress == "/setLogname") {}
            else if (oscAddress == "/loadMidiFile") {}
            else {
                DBG("Unknown OSC message: " + oscAddress);
            }
        }
        else if (message[0].isFloat32()) {
            if (oscAddress == "/numIntroTones") {}
            else if (oscAddress == "/ensembleDetails") {}
            else if (oscAddress == "/playerDetails") {}
            else {
                DBG("Unknown OSC float message: " + oscAddress);
            }
        }
        else {
            DBG("Unknown OSC message " + oscAddress);
        }
    }

    // Handle messages without parameters
    else {
        if (oscAddress == "/reset") {}
        else if (oscAddress == "/ensembleDetails") {}
        else if (oscAddress == "/ping") {}
        else {
            DBG("Unknown OSC message: " + oscAddress);
        }
    }
}


// MessageAcknowledgement sends a message to listeners outside the Metronome about what message it has received.
void OSCHandler::ConfirmedMessage(const juce::String& receivedAddress)
{
    juce::OSCMessage message("/confirmed", receivedAddress);
    sender.send(message);
}

// Checks if the port is connected by seeing what port we have used to intialise the sender/receiver
bool OSCHandler::isConnected() {
    return currentPort > 0;
}


//void OSCHandler::SendEnsembleDetails()
//{
//    return;
//}

