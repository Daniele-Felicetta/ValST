#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <limits>
#include <memory>
#include "PluginProcessor.h"
#include "EqGraph.h"
#include "LookAndFeel.h"

//==============================================================================
/** Pannello di una banda: FREQ, WIDTH, GAIN e, accanto al GAIN, il fader Mid/Side.
    Sotto: la saturazione della banda (SAT) e, per le bande 1 e 3, il passa-alto (LOW CUT). */
class BandStrip : public juce::Component
{
public:
    BandStrip (ValstMsEqProcessor&, int bandIndex);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Aggiorna la lettura Mid/Side effettiva e il colore del pomello GAIN (chiamata dal timer). */
    void update();

    static constexpr int stripHeight = 314;

private:
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    static constexpr int extraRowHeight   = 64;
    static constexpr int compactCellWidth = 136;

    void setupKnob (juce::Slider&, juce::Label& caption, const juce::String& text, const juce::String& tip, bool bipolar);
    void setupCompactKnob (juce::Slider&, juce::Label& caption, const juce::String& text, const juce::String& tip, juce::Colour accent);
    static void layoutKnob (juce::Label& caption, juce::Slider&, juce::Rectangle<int> cell);
    static void layoutCompactKnob (juce::Label& caption, juce::Slider&, juce::Rectangle<int> cell);

    ValstMsEqProcessor& proc;
    const int band;
    const bool hasLowCut;             // solo le bande 1 e 3 hanno un passa-alto

    juce::Slider freq, width, gain, ms, sat, lowCut;
    juce::Label  freqCaption, widthCaption, gainCaption, satCaption, lowCutCaption, sideLabel, midLabel, msValue;

    Attachment freqAttachment, widthAttachment, gainAttachment, msAttachment, satAttachment;
    std::unique_ptr<Attachment> lowCutAttachment;

    // NaN = "non ancora mostrato": il primo update() aggiorna sempre testo e colori
    float shownMs      = std::numeric_limits<float>::quiet_NaN();
    float shownMidDb   = std::numeric_limits<float>::quiet_NaN();
    float shownSideDb  = std::numeric_limits<float>::quiet_NaN();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandStrip)
};

//==============================================================================
/** Pannello di uno stadio globale: INPUT (valvola) oppure OUTPUT (trasformatore).
    Pomello del gain + curva di trasferimento statica dello stadio (calcolata con le stesse funzioni dell'audio). */
class DriveStagePanel : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    enum class Stage { valve, transformer };

    DriveStagePanel (ValstMsEqProcessor&, Stage);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Chiamata dal timer: ridisegna la curva se il gain e' cambiato. */
    void update();

    static constexpr int panelWidth = 104;

private:
    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    float currentGainDb() const;
    juce::Colour accent() const;
    void drawCurve (juce::Graphics&, juce::Rectangle<float> box) const;

    ValstMsEqProcessor& proc;
    const Stage stage;

    juce::Slider knob;
    Attachment attachment;
    juce::Rectangle<float> curveBox;
    float shownGain = std::numeric_limits<float>::quiet_NaN();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DriveStagePanel)
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

    static constexpr int baseWidth  = 960;
    static constexpr int baseHeight = 664;

private:
    void timerCallback() override;
    void updateHighlight (const juce::MouseEvent&);

    ValstMsEqProcessor& proc;
    ValstLookAndFeel lookAndFeel;

    juce::Component content;          // viene scalato per rendere l'editor ridimensionabile
    HeaderBar header;
    EqGraph graph;
    DriveStagePanel inPanel, outPanel;
    std::array<std::unique_ptr<BandStrip>, (size_t) valst::numBands> strips;

    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValstMsEqEditor)
};
