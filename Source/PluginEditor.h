#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <limits>
#include <memory>
#include "PluginProcessor.h"
#include "EqGraph.h"
#include "LookAndFeel.h"

//==============================================================================
/** Pannello di una banda: FREQ, WIDTH, GAIN e, accanto al GAIN, il fader Mid/Side. */
class BandStrip : public juce::Component
{
public:
    BandStrip (ValstMsEqProcessor&, int bandIndex);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Aggiorna la lettura Mid/Side effettiva e il colore del pomello GAIN (chiamata dal timer). */
    void update();

private:
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    void setupKnob (juce::Slider&, juce::Label& caption, const juce::String& text, const juce::String& tip, bool bipolar);
    static void layoutKnob (juce::Label& caption, juce::Slider&, juce::Rectangle<int> cell);

    ValstMsEqProcessor& proc;
    const int band;

    juce::Slider freq, width, gain, ms;
    juce::Label  freqCaption, widthCaption, gainCaption, sideLabel, midLabel, msValue;

    Attachment freqAttachment, widthAttachment, gainAttachment, msAttachment;

    // NaN = "non ancora mostrato": il primo update() aggiorna sempre testo e colori
    float shownMs      = std::numeric_limits<float>::quiet_NaN();
    float shownMidDb   = std::numeric_limits<float>::quiet_NaN();
    float shownSideDb  = std::numeric_limits<float>::quiet_NaN();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandStrip)
};

//==============================================================================
class HeaderBar : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
};

//==============================================================================
class ValstMsEqEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit ValstMsEqEditor (ValstMsEqProcessor&);
    ~ValstMsEqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    EqGraph& getGraph() noexcept { return graph; }

    void mouseEnter (const juce::MouseEvent& e) override { updateHighlight (e); }
    void mouseMove (const juce::MouseEvent& e) override  { updateHighlight (e); }
    void mouseExit (const juce::MouseEvent& e) override  { updateHighlight (e); }

private:
    void timerCallback() override;
    void updateHighlight (const juce::MouseEvent&);

    static constexpr int baseWidth  = 960;
    static constexpr int baseHeight = 600;

    ValstMsEqProcessor& proc;
    ValstLookAndFeel lookAndFeel;

    juce::Component content;          // viene scalato per rendere l'editor ridimensionabile
    HeaderBar header;
    EqGraph graph;
    std::array<std::unique_ptr<BandStrip>, (size_t) valst::numBands> strips;

    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValstMsEqEditor)
};
