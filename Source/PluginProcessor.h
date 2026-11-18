#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "MsEqDsp.h"
#include "Params.h"

class ValstMsEqProcessor : public juce::AudioProcessor
{
public:
    ValstMsEqProcessor();
    ~ValstMsEqProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ValST MS EQ"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override    { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** Valori "reali" correnti dei parametri di una banda (thread-safe, atomici). */
    float getParam (int band, valst::ParamKind kind) const noexcept
    {
        return params[(size_t) band][(size_t) kind]->load();
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    void pushParametersToEngine();

    valst::MsEqEngine engine;
    std::array<std::array<std::atomic<float>*, 4>, (size_t) valst::numBands> params {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValstMsEqProcessor)
};
