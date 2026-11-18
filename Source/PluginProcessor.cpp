#include "PluginProcessor.h"
#include "PluginEditor.h"

ValstMsEqProcessor::ValstMsEqProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", valst::createLayout())
{
    for (int b = 0; b < valst::numBands; ++b)
        for (int k = 0; k < 4; ++k)
        {
            params[(size_t) b][(size_t) k] = apvts.getRawParameterValue (valst::paramId (b, (valst::ParamKind) k));
            jassert (params[(size_t) b][(size_t) k] != nullptr);
        }
}

bool ValstMsEqProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void ValstMsEqProcessor::pushParametersToEngine()
{
    for (int b = 0; b < valst::numBands; ++b)
        engine.setBand (b,
                        getParam (b, valst::kFreq),
                        getParam (b, valst::kWidth),
                        getParam (b, valst::kGain),
                        getParam (b, valst::kMs));
}

void ValstMsEqProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    pushParametersToEngine();
    engine.snapToTargets();
}

void ValstMsEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (buffer.getNumChannels() < 2)
        return;

    pushParametersToEngine();
    engine.process (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());
}

juce::AudioProcessorEditor* ValstMsEqProcessor::createEditor()
{
    return new ValstMsEqEditor (*this);
}

void ValstMsEqProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ValstMsEqProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ValstMsEqProcessor();
}
