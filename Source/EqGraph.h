#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <vector>
#include "PluginProcessor.h"

//==============================================================================
/** Rappresentazione grafica dell'EQ.

    - curva ambra  = risposta in frequenza del segnale MID   (somma delle 3 bande)
    - curva ciano  = risposta in frequenza del segnale SIDE  (somma delle 3 bande)
    - un nodo per banda, trascinabile: orizzontale = frequenza, verticale = guadagno;
      rotella del mouse = larghezza; doppio clic = azzera il guadagno.
    - passando sopra un nodo (o sul pannello di una banda) vengono evidenziate le curve
      della singola banda e i valori effettivi su Mid e Side.

    Le curve sono calcolate con gli stessi coefficienti usati dall'audio. */
class EqGraph : public juce::Component,
                public juce::SettableTooltipClient
{
public:
    explicit EqGraph (ValstMsEqProcessor& processor);

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Chiamata dal timer dell'editor: ridisegna solo se qualcosa e' cambiato. */
    void refresh();

    /** Evidenzia una banda dall'esterno (mouse sul suo pannello). -1 = nessuna. */
    void setExternalBand (int band);

    /** Posizione (in pixel del componente) del nodo di una banda. Usata anche dai test. */
    juce::Point<float> getNodeCentre (int band) const { return nodePosition (readBand (band)); }

private:
    struct BandValues
    {
        float freq = 1000.0f, width = 1.0f, gainDb = 0.0f, ms = 0.0f;
    };

    BandValues readBand (int band) const;
    int   findNodeAt (juce::Point<float> p) const;
    int   highlightedBand() const;
    juce::Point<float> nodePosition (const BandValues&) const;

    float freqToX (float hz) const;
    float xToFreq (float x) const;
    float dbToY (float db) const;
    float yToDb (float y) const;

    void setParam (int band, valst::ParamKind kind, float realValue);
    void beginGesture (int band, valst::ParamKind kind);
    void endGesture (int band, valst::ParamKind kind);

    void drawGrid (juce::Graphics&) const;
    void drawCurve (juce::Graphics&, const std::vector<float>& db, juce::Colour, float thickness, float fillAlpha) const;
    void drawNodes (juce::Graphics&, const std::array<BandValues, (size_t) valst::numBands>&) const;
    void drawInfo (juce::Graphics&, int band, const BandValues&) const;
    void drawLegend (juce::Graphics&) const;

    ValstMsEqProcessor& proc;
    juce::Rectangle<float> plot;

    int hoverBand = -1, dragBand = -1, externalBand = -1;
    juce::Point<float> dragOffset;
    std::array<float, 13> lastValues {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqGraph)
};
