#pragma once
#include <JuceHeader.h>
#include <vector>
#include <atomic>
#include <thread>

#include "OSCHandler.h"
#include "EnsembleModel.h"


class OSCHandler :
    private juce::OSCReceiver::ListenerWithOSCAddress <juce::OSCReceiver::MessageLoopCallback>
{
public:
    //==============================================================================
    // Constructor and Destructor
    OSCHandler(EnsembleModel *ensemblePtr, int portNumber);
    ~OSCHandler() final;

    void MessageRecieved(const juce::OSCMessage& message);
    void ConfirmedMessage(const juce::String& receivedAddress);
    void SendEnsembleDetails();
    bool isConnected();

private:
    EnsembleModel* ensemble;
    juce::OSCSender sender;
    juce::OSCReceiver receiver;
    bool isOSCInitialised = false;
    int currentPort = -1;

    void ConnectSender();
    void ConnectReceiver();
};

