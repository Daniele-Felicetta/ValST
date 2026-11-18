#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "MsEqDsp.h"
#include "Params.h"
#include "Saturation.h"

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

    /** Valori "reali" correnti dei parametri di una banda (thread-safe, atomici). kSat e' in percentuale. */
    float getParam (int band, valst::ParamKind kind) const noexcept
    {
        return params[(size_t) band][(size_t) kind]->load();
    }

    /** Valori "reali" correnti dei parametri globali (gain di ingresso/uscita in dB, passa-alto in Hz). */
    float getGlobal (valst::GlobalParam p) const noexcept
    {
        return globals[(size_t) p]->load();
    }

    /** Seed persistente dell'identita' timbrica della saturazione di una banda. */
    uint64_t getSeed (int band) const noexcept { return seeds[(size_t) band].load(); }

    /** Imposta il seed di una banda (e lo scrive nello stato, cosi' viene salvato con il progetto). */
    void setSeed (int band, uint64_t seed);

    juce::AudioProcessorValueTreeState apvts;

private:
    void pushParametersToEngine();
    void writeSeedsToState();
    void readSeedsFromState();

    valst::MsEqEngine engine;
    std::array<std::array<std::atomic<float>*, (size_t) valst::numBandParams>, (size_t) valst::numBands> params {};
    std::array<std::atomic<float>*, (size_t) valst::numGlobalParams> globals {};
    std::array<std::atomic<uint64_t>, (size_t) valst::numBands> seeds {};
    std::vector<float> monoScratch;          // canale destro fittizio per l'elaborazione mono (preallocato)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValstMsEqProcessor)
};
