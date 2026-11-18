#include "PluginEditor.h"

using namespace valst;
using namespace valst::ui;

//==============================================================================
//  BandStrip
//==============================================================================
BandStrip::BandStrip (ValstMsEqProcessor& p, int bandIndex)
    : proc (p),
      band (bandIndex),
      hasLowCut (bandIndex != 1),
      freqAttachment  (p.apvts, paramId (bandIndex, kFreq),  freq),
      widthAttachment (p.apvts, paramId (bandIndex, kWidth), width),
      gainAttachment  (p.apvts, paramId (bandIndex, kGain),  gain),
      msAttachment    (p.apvts, paramId (bandIndex, kMs),    ms),
      satAttachment   (p.apvts, paramId (bandIndex, kSat),   sat)
{
    setupKnob (freq,  freqCaption,  "FREQ",
               it ("Frequenza centrale della campana (20 Hz – 20 kHz).\nDoppio clic: valore predefinito."), false);
    setupKnob (width, widthCaption, "WIDTH",
               it ("Larghezza della campana, in ottave: più alto = campana più larga.\nDoppio clic: valore predefinito."), false);
    setupKnob (gain,  gainCaption,  "GAIN",
               it ("Guadagno della banda, da -18 a +18 dB.\n"
                   "Il fader accanto decide su quale segnale lavora:\n"
                   "0 = Mid e Side insieme (stereo), in alto = Side, in basso = Mid."), true);

    // saturazione della banda
    sat.setComponentID ("sat");
    setupCompactKnob (sat, satCaption, "SAT",
                      it ("Saturazione analogica di questa banda, 0 – 100 % (0 = spenta).\n"
                          "Agisce sulla fetta di spettro della banda e segue il fader Mid/Side: lavora sul canale scelto.\n"
                          "Il suo carattere dipende da frequenza, larghezza e guadagno della banda:\n"
                          "più bassa e stretta = più dura e ricca di armoniche, più alta e larga = più morbida;\n"
                          "più guadagno = più armoniche pari. Ogni banda ha una sua identità timbrica fissa.\n"
                          "L'audio passa dalla banda 1 alla 3: ciascuna tratta anche le armoniche create dalla precedente.\n"
                          "Doppio clic: 0 %."), satColour());

    // passa-alto (solo bande 1 e 3)
    if (hasLowCut)
    {
        const bool first = (band == 0);
        lowCutAttachment = std::make_unique<Attachment> (p.apvts, globalId (first ? kHpf1 : kHpf3), lowCut);
        lowCut.setComponentID ("lowcut");

        setupCompactKnob (lowCut, lowCutCaption, "LOW CUT",
                          first ? it ("Filtro passa-alto morbido della banda 1: 6 dB per ottava, transizione molto graduale.\n"
                                      "Agisce su Mid e Side prima della banda 1 (e della sua saturazione).\n"
                                      "Al minimo (OFF) è spento. Doppio clic: OFF.")
                                : it ("Filtro passa-alto della banda 3: 12 dB per ottava, più ripido e con attenuazione più evidente\n"
                                      "sotto la frequenza di taglio. Agisce su Mid e Side dopo la banda 3.\n"
                                      "Al minimo (OFF) è spento. Doppio clic: OFF."),
                          lowCutColour());
    }

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

void BandStrip::setupCompactKnob (juce::Slider& s, juce::Label& caption, const juce::String& text,
                                  const juce::String& tip, juce::Colour accent)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 18);
    s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    s.setColour (juce::Slider::rotarySliderFillColourId, accent);
    s.setTooltip (tip);
    addAndMakeVisible (s);

    caption.setText (text, juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    caption.setColour (juce::Label::textColourId, accent);
    caption.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (caption);
}

void BandStrip::layoutKnob (juce::Label& caption, juce::Slider& slider, juce::Rectangle<int> cell)
{
    caption.setBounds (cell.removeFromTop (14));
    slider.setBounds (cell);
}

void BandStrip::layoutCompactKnob (juce::Label& caption, juce::Slider& slider, juce::Rectangle<int> cell)
{
    // pomello a sinistra, a destra la casella del valore con il nome appena sopra
    const int textWidth = 64, textHeight = 18;
    slider.setBounds (cell);
    caption.setBounds (cell.getRight() - textWidth, cell.getCentreY() - textHeight / 2 - 15, textWidth, 14);
}

void BandStrip::resized()
{
    auto area = getLocalBounds().reduced (8, 0);
    area.removeFromTop (34);       // titolo
    area.removeFromBottom (8);

    // riga inferiore: SAT (e LOW CUT per le bande 1 e 3)
    auto extra = area.removeFromBottom (extraRowHeight);

    if (hasLowCut)
    {
        auto left = extra.removeFromLeft (extra.getWidth() / 2);
        layoutCompactKnob (satCaption,    sat,    left.reduced (4, 0));
        layoutCompactKnob (lowCutCaption, lowCut, extra.reduced (4, 0));
    }
    else
    {
        layoutCompactKnob (satCaption, sat, extra.withSizeKeepingCentre (compactCellWidth, extra.getHeight()));
    }

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

    // separatore sopra la riga SAT / LOW CUT
    g.setColour (edgeColour());
    g.drawHorizontalLine (getHeight() - 8 - extraRowHeight - 2, 14.0f, (float) getWidth() - 14.0f);

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
//  DriveStagePanel
//==============================================================================
DriveStagePanel::DriveStagePanel (ValstMsEqProcessor& p, Stage s)
    : proc (p),
      stage (s),
      attachment (p.apvts, globalId (s == Stage::valve ? kInGain : kOutGain), knob)
{
    const bool valve = (stage == Stage::valve);

    knob.setComponentID (valve ? "in" : "out");
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
    knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    knob.setColour (juce::Slider::rotarySliderFillColourId, accent());
    knob.getProperties().set ("zeroValue", 0.0);        // tacca e punto di partenza dell'arco a 0 dB
    knob.setTooltip (valve
        ? it ("Gain di ingresso con saturazione a valvola, da -12 a +24 dB.\n"
              "Fino a 0 dB il segnale resta pulito. Salendo la valvola comprime i picchi in modo morbido e\n"
              "aggiunge armoniche pari e dispari sempre più evidenti: il gain regola livello e saturazione insieme.\n"
              "Il livello è compensato solo in parte (metà dei dB aggiunti), così la saturazione si sente.\n"
              "Doppio clic: 0 dB.")
        : it ("Gain di uscita con saturazione a trasformatore, da -24 a +12 dB.\n"
              "Il trasformatore è trasparente fino a 0 dBFS; oltre comincia ad addolcire e comprimere i picchi\n"
              "aggiungendo armoniche dispari (prima sui bassi, dove il nucleo magnetico satura per primo).\n"
              "Non è un limitatore: la compressione cresce senza muri. Usalo anche per compensare il livello finale.\n"
              "Doppio clic: 0 dB."));
    addAndMakeVisible (knob);

    setTooltip (valve
        ? it ("Curva di trasferimento statica della valvola (ingresso -> uscita) con il gain impostato:\n"
              "la linea tenue è la retta senza distorsione. Calcolata con le stesse funzioni dell'audio.")
        : it ("Curva di trasferimento statica del trasformatore (ingresso -> uscita) con il gain impostato:\n"
              "le linee tratteggiate sono 0 dBFS. Sotto 0 dBFS coincide con la retta; oltre si piega, senza mai appiattirsi.\n"
              "Calcolata con le stesse funzioni dell'audio (senza l'enfasi sui bassi)."));
}

juce::Colour DriveStagePanel::accent() const
{
    return stage == Stage::valve ? satColour() : trafoColour();
}

float DriveStagePanel::currentGainDb() const
{
    return proc.getGlobal (stage == Stage::valve ? kInGain : kOutGain);
}

void DriveStagePanel::update()
{
    const float now = currentGainDb();
    if (now != shownGain)        // NaN al primo giro: ridisegna sempre
    {
        shownGain = now;
        repaint (curveBox.toNearestInt().expanded (2));
    }
}

void DriveStagePanel::resized()
{
    auto area = getLocalBounds().reduced (8, 8);
    area.removeFromTop (18 + 16);                          // titolo e sottotitolo (disegnati in paint)
    knob.setBounds (area.removeFromTop (112));
    area.removeFromTop (8);

    const int side = juce::jmin (area.getWidth(), area.getHeight() - 16);
    curveBox = area.removeFromTop (juce::jmax (0, side)).withSizeKeepingCentre (juce::jmax (0, side), juce::jmax (0, side)).toFloat();
}

void DriveStagePanel::paint (juce::Graphics& g)
{
    const bool valve = (stage == Stage::valve);
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (panelColour());
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (edgeColour());
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (textColour());
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (valve ? "INPUT" : "OUTPUT", juce::Rectangle<float> (0.0f, 8.0f, bounds.getWidth(), 18.0f), juce::Justification::centred, false);

    g.setColour (accent());
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.drawText (valve ? "VALVE" : "TRANSFORMER", juce::Rectangle<float> (0.0f, 26.0f, bounds.getWidth(), 16.0f), juce::Justification::centred, false);

    if (! curveBox.isEmpty())
    {
        drawCurve (g, curveBox);

        g.setColour (dimTextColour());
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (it ("curva statica"), juce::Rectangle<float> (0.0f, curveBox.getBottom() + 2.0f, bounds.getWidth(), 14.0f),
                    juce::Justification::centred, false);
    }
}

void DriveStagePanel::drawCurve (juce::Graphics& g, juce::Rectangle<float> box) const
{
    const bool valve = (stage == Stage::valve);
    const double gainDb = (double) currentGainDb();
    const double xRange = 1.0;                          // ingresso: da -1 a +1 (fondo scala)
    const double yRange = valve ? 1.5 : 2.0;            // uscita: semi-altezza del riquadro

    // funzione di trasferimento statica: le stesse formule dell'audio, senza i filtri di enfasi / blocco DC
    std::function<double (double)> transfer;

    if (valve)
    {
        const double pre   = std::pow (10.0, gainDb / 20.0);
        const double post  = valst::sat::valveCompensationLinear (gainDb);
        const double kappa = valst::sat::valveKappa (gainDb);
        const auto shaper  = valst::sat::makeValveShaper();

        transfer = [=] (double x)
        {
            const double u = x * pre;
            return post * (kappa > 1.0e-4 ? u + shaper.h (kappa * u) / kappa : u);
        };
    }
    else
    {
        const double gain  = std::pow (10.0, gainDb / 20.0);
        const auto shaper  = valst::sat::makeTransformerShaper();

        transfer = [=] (double x)
        {
            const double u = x * gain;
            return u + shaper.h (valst::sat::trafoDrive * u) / valst::sat::trafoDrive;
        };
    }

    auto toX = [&] (double x) { return box.getCentreX() + (float) (x / xRange) * box.getWidth() * 0.5f; };
    auto toY = [&] (double y) { return box.getCentreY() - (float) (juce::jlimit (-yRange, yRange, y) / yRange) * box.getHeight() * 0.5f; };

    g.setColour (bgColour());
    g.fillRoundedRectangle (box, 4.0f);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (box.toNearestInt());

        // assi
        g.setColour (gridColour());
        g.drawHorizontalLine (juce::roundToInt (box.getCentreY()), box.getX(), box.getRight());
        g.drawVerticalLine   (juce::roundToInt (box.getCentreX()), box.getY(), box.getBottom());

        // 0 dBFS in uscita (trasformatore)
        if (! valve)
        {
            const float dashes[] = { 3.0f, 3.0f };
            g.setColour (neutralColour().withAlpha (0.28f));
            g.drawDashedLine (juce::Line<float> (box.getX(), toY (1.0),  box.getRight(), toY (1.0)),  dashes, 2, 1.0f);
            g.drawDashedLine (juce::Line<float> (box.getX(), toY (-1.0), box.getRight(), toY (-1.0)), dashes, 2, 1.0f);
        }

        // retta di riferimento: nessuna distorsione, guadagno unitario
        {
            juce::Path ref;
            ref.startNewSubPath (toX (-xRange), toY (-xRange));
            ref.lineTo (toX (xRange), toY (xRange));
            g.setColour (dimTextColour().withAlpha (0.40f));
            g.strokePath (ref, juce::PathStrokeType (1.0f));
        }

        // curva
        juce::Path curve;
        const int n = 120;
        for (int i = 0; i <= n; ++i)
        {
            const double x = -xRange + 2.0 * xRange * (double) i / (double) n;
            const float px = toX (x), py = toY (transfer (x));

            if (i == 0) curve.startNewSubPath (px, py);
            else        curve.lineTo (px, py);
        }

        g.setColour (accent());
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    g.setColour (edgeColour());
    g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.0f);
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
    g.drawText (it ("MID/SIDE PARAMETRIC EQUALIZER  ·  3 BANDS  ·  ANALOG SATURATION"), getLocalBounds().toFloat(),
                juce::Justification::centredRight, false);
}

//==============================================================================
//  Editor
//==============================================================================
ValstMsEqEditor::ValstMsEqEditor (ValstMsEqProcessor& p)
    : AudioProcessorEditor (&p), proc (p), graph (p),
      inPanel (p, DriveStagePanel::Stage::valve), outPanel (p, DriveStagePanel::Stage::transformer)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (content);
    content.addAndMakeVisible (header);
    content.addAndMakeVisible (graph);
    content.addAndMakeVisible (inPanel);
    content.addAndMakeVisible (outPanel);

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

    auto stripArea = area.removeFromBottom (BandStrip::stripHeight);
    area.removeFromBottom (10);

    // gain di ingresso (valvola) e di uscita (trasformatore) ai lati del grafico
    inPanel.setBounds (area.removeFromLeft (DriveStagePanel::panelWidth));
    area.removeFromLeft (10);
    outPanel.setBounds (area.removeFromRight (DriveStagePanel::panelWidth));
    area.removeFromRight (10);
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
    inPanel.update();
    outPanel.update();

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
