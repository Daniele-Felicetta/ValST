#include "PluginEditor.h"

using namespace valst;
using namespace valst::ui;

//==============================================================================
//  BandStrip
//==============================================================================
BandStrip::BandStrip (ValstMsEqProcessor& p, int bandIndex)
    : proc (p),
      band (bandIndex),
      freqAttachment  (p.apvts, paramId (bandIndex, kFreq),  freq),
      widthAttachment (p.apvts, paramId (bandIndex, kWidth), width),
      gainAttachment  (p.apvts, paramId (bandIndex, kGain),  gain),
      msAttachment    (p.apvts, paramId (bandIndex, kMs),    ms)
{
    setupKnob (freq,  freqCaption,  "FREQ",
               it ("Frequenza centrale della campana (20 Hz – 20 kHz).\nDoppio clic: valore predefinito."), false);
    setupKnob (width, widthCaption, "WIDTH",
               it ("Larghezza della campana, in ottave: più alto = campana più larga.\nDoppio clic: valore predefinito."), false);
    setupKnob (gain,  gainCaption,  "GAIN",
               it ("Guadagno della banda, da -18 a +18 dB.\n"
                   "Il fader accanto decide su quale segnale lavora:\n"
                   "0 = Mid e Side insieme (stereo), in alto = Side, in basso = Mid."), true);

    // fader Mid/Side
    ms.setSliderStyle (juce::Slider::LinearVertical);
    ms.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    ms.setSliderSnapsToMousePosition (false);
    ms.setTooltip (it ("Fader Mid/Side, scala logaritmica da -100 a +100.\n"
                       "0 (centro) = neutro: il GAIN lavora sul segnale stereo (Mid e Side insieme).\n"
                       "In alto = il GAIN lavora sul Side. In basso = il GAIN lavora sul Mid.\n"
                       "Più ci si avvicina agli estremi, meno l'altro canale riceve il guadagno.\n"
                       "Doppio clic: torna a 0."));
    addAndMakeVisible (ms);

    auto setupTag = [this] (juce::Label& label, const char* text, juce::Colour colour)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        label.setColour (juce::Label::textColourId, colour);
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
    };
    setupTag (sideLabel, "SIDE", sideColour());
    setupTag (midLabel,  "MID",  midColour());

    // valore numerico del fader (modificabile con un clic)
    msValue.setJustificationType (juce::Justification::centred);
    msValue.setFont (juce::FontOptions (12.5f));
    msValue.setEditable (true, false, false);
    msValue.setTooltip (ms.getTooltip());
    msValue.onTextChange = [this]
    {
        if (auto* parameter = proc.apvts.getParameter (paramId (band, kMs)))
            parameter->setValueNotifyingHost (parameter->getValueForText (msValue.getText()));

        shownMs = std::numeric_limits<float>::quiet_NaN();   // forza l'aggiornamento del testo
        update();
    };
    addAndMakeVisible (msValue);

    update();
}

void BandStrip::setupKnob (juce::Slider& s, juce::Label& caption, const juce::String& text,
                           const juce::String& tip, bool bipolar)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
    s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    s.getProperties().set ("bipolar", bipolar);
    s.setTooltip (tip);
    addAndMakeVisible (s);

    caption.setText (text, juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    caption.setColour (juce::Label::textColourId, dimTextColour());
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);
}

void BandStrip::layoutKnob (juce::Label& caption, juce::Slider& slider, juce::Rectangle<int> cell)
{
    caption.setBounds (cell.removeFromTop (14));
    slider.setBounds (cell);
}

void BandStrip::resized()
{
    auto area = getLocalBounds().reduced (8, 0);
    area.removeFromTop (34);       // titolo
    area.removeFromBottom (8);

    auto colLeft  = area.removeFromLeft (96);
    auto colGain  = area.removeFromLeft (112);
    auto colFader = area;

    const int half = colLeft.getHeight() / 2;
    layoutKnob (freqCaption,  freq,  colLeft.removeFromTop (half));
    layoutKnob (widthCaption, width, colLeft);
    layoutKnob (gainCaption,  gain,  colGain);

    sideLabel.setBounds (colFader.removeFromTop (14));
    msValue.setBounds   (colFader.removeFromBottom (20).reduced (6, 0));
    midLabel.setBounds  (colFader.removeFromBottom (14));
    ms.setBounds (colFader);
}

void BandStrip::update()
{
    const float msNow   = proc.getParam (band, kMs);
    const float gainNow = proc.getParam (band, kGain);
    const auto  gains   = computeBandGains (gainNow, msNow);

    const float midNow  = (float) gains.midDb;
    const float sideNow = (float) gains.sideDb;

    const bool changed = ! (msNow == shownMs && midNow == shownMidDb && sideNow == shownSideDb);   // NaN -> sempre "cambiato"
    if (! changed)
        return;

    shownMs = msNow;
    shownMidDb = midNow;
    shownSideDb = sideNow;

    // il pomello GAIN prende il colore del canale su cui lavora: bianco = stereo, ciano = Side, ambra = Mid
    gain.setColour (juce::Slider::rotarySliderFillColourId, msColour (msNow));
    gain.repaint();

    if (! msValue.isBeingEdited())
        msValue.setText (msToText (msNow), juce::dontSendNotification);

    repaint();
}

void BandStrip::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelColour());
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (edgeColour());
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    // numero della banda
    const juce::Rectangle<float> badge (12.0f, 8.0f, 20.0f, 20.0f);
    g.setColour (bgColour());
    g.fillEllipse (badge);
    g.setColour (msColour (shownMs));
    g.drawEllipse (badge.reduced (1.0f), 2.0f);
    g.setColour (textColour());
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.drawText (juce::String (band + 1), badge, juce::Justification::centred, false);

    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("BAND " + juce::String (band + 1), juce::Rectangle<float> (38.0f, 8.0f, 70.0f, 20.0f),
                juce::Justification::centredLeft, false);

    // guadagno effettivo applicato a Mid e Side
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    const float right = bounds.getRight() - 12.0f;

    g.setColour (sideColour());
    g.drawText ("SIDE " + gainToText (shownSideDb), juce::Rectangle<float> (right - 94.0f, 8.0f, 94.0f, 20.0f),
                juce::Justification::centredRight, false);
    g.setColour (midColour());
    g.drawText ("MID " + gainToText (shownMidDb), juce::Rectangle<float> (right - 94.0f - 98.0f, 8.0f, 94.0f, 20.0f),
                juce::Justification::centredRight, false);
}

//==============================================================================
//  HeaderBar
//==============================================================================
void HeaderBar::paint (juce::Graphics& g)
{
    juce::AttributedString title;
    title.setJustification (juce::Justification::centredLeft);
    title.append ("ValST", juce::Font (juce::FontOptions (22.0f, juce::Font::bold)), textColour());
    title.append ("  MS EQ", juce::Font (juce::FontOptions (22.0f)), dimTextColour());
    title.draw (g, getLocalBounds().toFloat());

    g.setColour (dimTextColour());
    g.setFont (juce::FontOptions (11.5f));
    g.drawText (it ("MID/SIDE PARAMETRIC EQUALIZER  ·  3 BANDS"), getLocalBounds().toFloat(),
                juce::Justification::centredRight, false);
}

//==============================================================================
//  Editor
//==============================================================================
ValstMsEqEditor::ValstMsEqEditor (ValstMsEqProcessor& p)
    : AudioProcessorEditor (&p), proc (p), graph (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (content);
    content.addAndMakeVisible (header);
    content.addAndMakeVisible (graph);

    for (size_t i = 0; i < strips.size(); ++i)
    {
        strips[i] = std::make_unique<BandStrip> (p, (int) i);
        content.addAndMakeVisible (*strips[i]);
        strips[i]->addMouseListener (this, true);   // per evidenziare la banda nel grafico
    }

    setResizable (true, true);
    setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 2, baseHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
    setSize (baseWidth, baseHeight);

    // le caselle di testo dei pomelli sono create prima che i componenti abbiano un genitore:
    // le ricreo ora che lo stile grafico (lookAndFeel) e' raggiungibile
    sendLookAndFeelChange();

    startTimerHz (30);
}

ValstMsEqEditor::~ValstMsEqEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void ValstMsEqEditor::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (bgColour().brighter (0.07f), 0.0f, 0.0f,
                                             bgColour(), 0.0f, (float) getHeight(), false));
    g.fillAll();
}

void ValstMsEqEditor::resized()
{
    const float scale = juce::jmin ((float) getWidth()  / (float) baseWidth,
                                    (float) getHeight() / (float) baseHeight);

    content.setTransform (juce::AffineTransform::scale (scale));
    content.setBounds (0, 0, baseWidth, baseHeight);

    auto area = juce::Rectangle<int> (0, 0, baseWidth, baseHeight).reduced (14, 10);
    header.setBounds (area.removeFromTop (30));
    area.removeFromTop (6);

    auto stripArea = area.removeFromBottom (250);
    area.removeFromBottom (10);
    graph.setBounds (area);

    const int gap = 10;
    const int w   = (stripArea.getWidth() - gap * ((int) strips.size() - 1)) / (int) strips.size();

    for (auto& strip : strips)
    {
        strip->setBounds (stripArea.removeFromLeft (w));
        stripArea.removeFromLeft (gap);
    }
}

void ValstMsEqEditor::timerCallback()
{
    graph.refresh();

    for (auto& strip : strips)
        strip->update();
}

void ValstMsEqEditor::updateHighlight (const juce::MouseEvent& e)
{
    const auto p = e.getEventRelativeTo (&content).getPosition();
    int band = -1;

    for (size_t i = 0; i < strips.size(); ++i)
        if (strips[i]->getBounds().contains (p))
            band = (int) i;

    graph.setExternalBand (band);
}
