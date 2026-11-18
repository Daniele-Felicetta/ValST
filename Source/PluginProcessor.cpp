#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <set>

namespace
{
    constexpr size_t defaultScratchSize = 4096;

    juce::Identifier seedProperty (int band)  { return juce::Identifier ("satSeed" + juce::String (band + 1)); }
    const juce::Identifier algorithmProperty ("satAlgo");
}

ValstMsEqProcessor::ValstMsEqProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", valst::createLayout())
{
    for (int b = 0; b < valst::numBands; ++b)
        for (int k = 0; k < valst::numBandParams; ++k)
        {
            params[(size_t) b][(size_t) k] = apvts.getRawParameterValue (valst::paramId (b, (valst::ParamKind) k));
            jassert (params[(size_t) b][(size_t) k] != nullptr);
        }

    for (int g = 0; g < valst::numGlobalParams; ++g)
    {
        globals[(size_t) g] = apvts.getRawParameterValue (valst::globalId ((valst::GlobalParam) g));
        jassert (globals[(size_t) g] != nullptr);
    }

    for (int b = 0; b < valst::numBands; ++b)
        seeds[(size_t) b].store (valst::sat::defaultSeeds[b]);

    monoScratch.assign (defaultScratchSize, 0.0f);
    writeSeedsToState();
}

bool ValstMsEqProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void ValstMsEqProcessor::pushParametersToEngine()
{
    engine.setInput  (getGlobal (valst::kInGain));
    engine.setOutput (getGlobal (valst::kOutGain));

    for (int b = 0; b < valst::numBands; ++b)
    {
        engine.setSeed (b, seeds[(size_t) b].load());
        engine.setBand (b,
                        getParam (b, valst::kFreq),
                        getParam (b, valst::kWidth),
                        getParam (b, valst::kGain),
                        getParam (b, valst::kMs),
                        getParam (b, valst::kSat) / valst::satMaxPercent);
    }

    engine.setHighPass (0, getGlobal (valst::kHpf1));
    engine.setHighPass (1, getGlobal (valst::kHpf3));
}

void ValstMsEqProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // unica allocazione: avviene qui, mai nel thread audio
    monoScratch.assign (juce::jmax (defaultScratchSize, (size_t) juce::jmax (0, samplesPerBlock)), 0.0f);

    engine.prepare (sampleRate);
    pushParametersToEngine();
    engine.snapToTargets();
}

void ValstMsEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    if (numChannels < 1 || numSamples < 1)
        return;

    pushParametersToEngine();

    if (numChannels >= 2)
    {
        engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples);
        return;
    }

    // Mono: il canale destro e' una copia del sinistro, quindi Side = 0 e lavora solo il Mid.
    // Il risultato e' quello che si avrebbe mandando lo stesso segnale su L e R.
    float* mono = buffer.getWritePointer (0);
    const int scratch = (int) monoScratch.size();

    for (int pos = 0; pos < numSamples;)
    {
        const int n = juce::jmin (numSamples - pos, scratch);
        std::copy (mono + pos, mono + pos + n, monoScratch.begin());
        engine.process (mono + pos, monoScratch.data(), n);
        pos += n;
    }
}

juce::AudioProcessorEditor* ValstMsEqProcessor::createEditor()
{
    return new ValstMsEqEditor (*this);
}

//==============================================================================
//  Stato: parametri + seed dell'identita' timbrica delle bande
void ValstMsEqProcessor::writeSeedsToState()
{
    apvts.state.setProperty (algorithmProperty, valst::sat::characterAlgorithmVersion, nullptr);

    for (int b = 0; b < valst::numBands; ++b)
        apvts.state.setProperty (seedProperty (b), juce::String::toHexString ((juce::int64) seeds[(size_t) b].load()), nullptr);
}

void ValstMsEqProcessor::readSeedsFromState()
{
    for (int b = 0; b < valst::numBands; ++b)
    {
        const auto text = apvts.state.getProperty (seedProperty (b)).toString();
        const uint64_t seed = text.isNotEmpty() ? (uint64_t) text.getHexValue64() : valst::sat::defaultSeeds[b];
        seeds[(size_t) b].store (seed);
    }

    writeSeedsToState();   // uno stato vecchio (senza seed) ora li contiene
}

void ValstMsEqProcessor::setSeed (int band, uint64_t seed)
{
    seeds[(size_t) band].store (seed);
    writeSeedsToState();
}

void ValstMsEqProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ValstMsEqProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // Quali parametri contiene lo stato caricato? Un preset della 1.0 non conosce quelli nuovi.
    std::set<juce::String> present;
    for (const auto* child : xml->getChildIterator())
        if (child->hasTagName ("PARAM"))
            present.insert (child->getStringAttribute ("id"));

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // I parametri introdotti dopo il salvataggio tornano al valore predefinito (neutro), invece di
    // restare al valore che avevano prima del caricamento: il preset suona come quando e' stato salvato.
    auto resetIfMissing = [&] (const juce::String& id)
    {
        if (present.count (id) == 0)
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (p->getDefaultValue());
    };

    resetIfMissing (valst::globalId (valst::kInGain));
    resetIfMissing (valst::globalId (valst::kOutGain));
    resetIfMissing (valst::globalId (valst::kHpf1));
    resetIfMissing (valst::globalId (valst::kHpf3));

    for (int b = 0; b < valst::numBands; ++b)
        resetIfMissing (valst::paramId (b, valst::kSat));

    readSeedsFromState();
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ValstMsEqProcessor();
}
