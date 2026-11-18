#include "EqGraph.h"
#include "LookAndFeel.h"

using namespace valst;
using namespace valst::ui;

namespace
{
    constexpr float dbRange    = 24.0f;    // il grafico copre +/-24 dB
    constexpr float nodeRadius = 9.0f;
    constexpr float hitRadius  = 16.0f;

    struct BandFilters
    {
        Biquad mid, side;
    };

    BandFilters makeFilters (float freq, float width, float gainDb, float ms, double sampleRate)
    {
        const auto g = computeBandGains (gainDb, ms);
        BandFilters f;
        f.mid .setPeaking (sampleRate, freq, width, g.midDb);
        f.side.setPeaking (sampleRate, freq, width, g.sideDb);
        return f;
    }
}

//==============================================================================
EqGraph::EqGraph (ValstMsEqProcessor& p) : proc (p)
{
    setTooltip (it ("Trascina i nodi: orizzontale = frequenza, verticale = guadagno.\n"
                    "Rotella del mouse su un nodo: larghezza della campana.\n"
                    "Doppio clic su un nodo: azzera il guadagno.\n"
                    "Curva ambra = segnale Mid, curva ciano = segnale Side."));
}

EqGraph::BandValues EqGraph::readBand (int band) const
{
    BandValues v;
    v.freq   = proc.getParam (band, kFreq);
    v.width  = proc.getParam (band, kWidth);
    v.gainDb = proc.getParam (band, kGain);
    v.ms     = proc.getParam (band, kMs);
    return v;
}

//==============================================================================
//  Conversioni coordinate <-> valori
float EqGraph::freqToX (float hz) const
{
    return plot.getX() + plot.getWidth() * std::log (hz / freqMin) / std::log (freqMax / freqMin);
}

float EqGraph::xToFreq (float x) const
{
    const float p = juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth());
    return freqMin * std::pow (freqMax / freqMin, p);
}

float EqGraph::dbToY (float db) const
{
    return plot.getY() + plot.getHeight() * (1.0f - (db + dbRange) / (2.0f * dbRange));
}

float EqGraph::yToDb (float y) const
{
    return (1.0f - (y - plot.getY()) / plot.getHeight()) * 2.0f * dbRange - dbRange;
}

juce::Point<float> EqGraph::nodePosition (const BandValues& v) const
{
    return { freqToX (v.freq), dbToY (v.gainDb) };
}

int EqGraph::highlightedBand() const
{
    if (dragBand >= 0)  return dragBand;
    if (hoverBand >= 0) return hoverBand;
    return externalBand;
}

void EqGraph::setExternalBand (int band)
{
    if (band != externalBand)
    {
        externalBand = band;
        repaint();
    }
}

void EqGraph::resized()
{
    plot = getLocalBounds().toFloat().withTrimmedLeft (38.0f).withTrimmedRight (16.0f)
                                     .withTrimmedTop (12.0f).withTrimmedBottom (24.0f);
}

void EqGraph::refresh()
{
    std::array<float, 13> now {};
    size_t i = 0;

    for (int b = 0; b < numBands; ++b)
        for (int k = 0; k < 4; ++k)
            now[i++] = proc.getParam (b, (ParamKind) k);

    now[i] = (float) proc.getSampleRate();

    if (now != lastValues)
    {
        lastValues = now;
        repaint();
    }
}

//==============================================================================
//  Disegno
void EqGraph::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelColour());
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (edgeColour());
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    if (plot.isEmpty())
        return;

    const double sampleRate = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;

    std::array<BandValues, (size_t) numBands>  bands;
    std::array<BandFilters, (size_t) numBands> filters;

    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        bands[b]   = readBand ((int) b);
        filters[b] = makeFilters (bands[b].freq, bands[b].width, bands[b].gainDb, bands[b].ms, sampleRate);
    }

    drawGrid (g);

    const int highlighted = highlightedBand();

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt());

        const int n = juce::jmax (64, (int) (plot.getWidth() / 2.0f));
        std::vector<float> sumMid ((size_t) n, 0.0f), sumSide ((size_t) n, 0.0f);
        std::vector<float> bandMid ((size_t) n, 0.0f), bandSide ((size_t) n, 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const double f = (double) freqMin * std::pow ((double) (freqMax / freqMin), (double) i / (double) (n - 1));

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const float dm = (float) filters[b].mid .magnitudeDb (f, sampleRate);
                const float ds = (float) filters[b].side.magnitudeDb (f, sampleRate);

                sumMid[(size_t) i]  += dm;
                sumSide[(size_t) i] += ds;

                if ((int) b == highlighted)
                {
                    bandMid[(size_t) i]  = dm;
                    bandSide[(size_t) i] = ds;
                }
            }
        }

        if (highlighted >= 0)
        {
            drawCurve (g, bandMid,  midColour(),  1.0f, 0.08f);
            drawCurve (g, bandSide, sideColour(), 1.0f, 0.08f);
        }

        drawCurve (g, sumMid,  midColour(),  2.8f, 0.14f);
        drawCurve (g, sumSide, sideColour(), 1.8f, 0.10f);
    }

    drawNodes (g, bands);
    drawLegend (g);

    if (highlighted >= 0)
        drawInfo (g, highlighted, bands[(size_t) highlighted]);
}

void EqGraph::drawGrid (juce::Graphics& g) const
{
    // linee verticali (frequenza)
    static const float lines[] = { 20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900,
                                   1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 20000 };

    for (const float f : lines)
    {
        const bool major = (f == 100.0f || f == 1000.0f || f == 10000.0f);
        g.setColour (gridColour().withAlpha (major ? 1.0f : 0.55f));
        g.drawVerticalLine (juce::roundToInt (freqToX (f)), plot.getY(), plot.getBottom());
    }

    struct Label { float freq; const char* text; };
    static const Label labels[] = { { 20, "20" }, { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" },
                                    { 1000, "1k" }, { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" } };

    g.setFont (juce::FontOptions (10.5f));
    g.setColour (dimTextColour());
    for (const auto& l : labels)
        g.drawText (l.text, juce::Rectangle<float> (freqToX (l.freq) - 16.0f, plot.getBottom() + 5.0f, 32.0f, 14.0f),
                    juce::Justification::centred, false);

    // linee orizzontali (dB)
    for (int db = -24; db <= 24; db += 6)
    {
        const float y = dbToY ((float) db);
        const bool zero = (db == 0);

        g.setColour (zero ? neutralColour().withAlpha (0.38f) : gridColour().withAlpha (0.9f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());

        g.setColour (zero ? textColour() : dimTextColour());
        g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                    juce::Rectangle<float> (2.0f, y - 7.0f, plot.getX() - 7.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }
}

void EqGraph::drawCurve (juce::Graphics& g, const std::vector<float>& db, juce::Colour colour,
                         float thickness, float fillAlpha) const
{
    const int n = (int) db.size();
    juce::Path line;

    for (int i = 0; i < n; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
        const float y = dbToY (juce::jlimit (-dbRange - 8.0f, dbRange + 8.0f, db[(size_t) i]));

        if (i == 0) line.startNewSubPath (x, y);
        else        line.lineTo (x, y);
    }

    if (fillAlpha > 0.0f)
    {
        juce::Path fill (line);
        const float y0 = dbToY (0.0f);
        fill.lineTo (plot.getRight(), y0);
        fill.lineTo (plot.getX(), y0);
        fill.closeSubPath();

        g.setColour (colour.withAlpha (fillAlpha));
        g.fillPath (fill);
    }

    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void EqGraph::drawNodes (juce::Graphics& g, const std::array<BandValues, (size_t) numBands>& bands) const
{
    const int highlighted = highlightedBand();

    for (int b = 0; b < numBands; ++b)
    {
        const auto& v      = bands[(size_t) b];
        const auto  pos    = nodePosition (v);
        const bool  active = (b == highlighted);
        const float r      = nodeRadius + (active ? 2.0f : 0.0f);
        const auto  colour = msColour (v.ms);

        if (active)
        {
            g.setColour (colour.withAlpha (0.22f));
            g.fillEllipse (pos.x - r - 5.0f, pos.y - r - 5.0f, (r + 5.0f) * 2.0f, (r + 5.0f) * 2.0f);
        }

        g.setColour (bgColour());
        g.fillEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f);
        g.setColour (colour);
        g.drawEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f, active ? 2.6f : 2.0f);

        g.setColour (textColour());
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String (b + 1), juce::Rectangle<float> (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f),
                    juce::Justification::centred, false);
    }
}

void EqGraph::drawLegend (juce::Graphics& g) const
{
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));

    float x = plot.getRight() - 8.0f;
    const float y = plot.getY() + 6.0f;

    auto item = [&] (const char* text, juce::Colour colour, float width)
    {
        x -= width;
        g.setColour (colour);
        g.fillEllipse (x, y + 3.0f, 8.0f, 8.0f);
        g.drawText (text, juce::Rectangle<float> (x + 13.0f, y, width - 13.0f, 14.0f), juce::Justification::centredLeft, false);
        x -= 6.0f;
    };

    item ("SIDE", sideColour(), 50.0f);
    item ("MID",  midColour(),  44.0f);
}

void EqGraph::drawInfo (juce::Graphics& g, int band, const BandValues& v) const
{
    const auto gains = computeBandGains (v.gainDb, v.ms);
    const juce::Font normal (juce::FontOptions (12.0f));
    const juce::Font bold   (juce::FontOptions (12.0f, juce::Font::bold));

    juce::AttributedString text;
    text.setJustification (juce::Justification::centredLeft);
    text.append ("BAND " + juce::String (band + 1) + "    ",                                                   bold,   textColour());
    text.append (freqToText (v.freq) + "    " + gainToText (v.gainDb) + "    ",                                normal, textColour());
    text.append ("W " + widthToText (v.width) + " (Q " + juce::String (widthToQ (v.width), 2) + ")    ",        normal, dimTextColour());
    text.append ("MID " + gainToText ((float) gains.midDb) + "    ",                                           bold,   midColour());
    text.append ("SIDE " + gainToText ((float) gains.sideDb),                                                  bold,   sideColour());

    juce::TextLayout layout;
    layout.createLayout (text, 2000.0f);

    const juce::Rectangle<float> chip (plot.getX() + 8.0f, plot.getY() + 5.0f, layout.getWidth() + 18.0f, 22.0f);
    g.setColour (juce::Colour (0xe60d1014));
    g.fillRoundedRectangle (chip, 6.0f);
    g.setColour (edgeColour());
    g.drawRoundedRectangle (chip.reduced (0.5f), 6.0f, 1.0f);
    layout.draw (g, chip.reduced (9.0f, 3.0f));
}

//==============================================================================
//  Interazione
int EqGraph::findNodeAt (juce::Point<float> p) const
{
    int best = -1;
    float bestDistance = hitRadius * hitRadius;

    for (int b = 0; b < numBands; ++b)
    {
        const float d = nodePosition (readBand (b)).getDistanceSquaredFrom (p);
        if (d <= bestDistance)
        {
            bestDistance = d;
            best = b;
        }
    }

    return best;
}

void EqGraph::setParam (int band, ParamKind kind, float realValue)
{
    if (auto* p = proc.apvts.getParameter (paramId (band, kind)))
        p->setValueNotifyingHost (p->convertTo0to1 (realValue));
}

void EqGraph::beginGesture (int band, ParamKind kind)
{
    if (auto* p = proc.apvts.getParameter (paramId (band, kind)))
        p->beginChangeGesture();
}

void EqGraph::endGesture (int band, ParamKind kind)
{
    if (auto* p = proc.apvts.getParameter (paramId (band, kind)))
        p->endChangeGesture();
}

void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    const int hit = findNodeAt (e.position);

    if (hit != hoverBand)
    {
        hoverBand = hit;
        setMouseCursor (hit >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    if (dragBand < 0 && hoverBand >= 0)
    {
        hoverBand = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    dragBand = e.getNumberOfClicks() > 1 ? -1 : findNodeAt (e.position);

    if (dragBand < 0)
        return;

    dragOffset = nodePosition (readBand (dragBand)) - e.position;
    beginGesture (dragBand, kFreq);
    beginGesture (dragBand, kGain);
    repaint();
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    const auto p = e.position + dragOffset;
    setParam (dragBand, kFreq, xToFreq (juce::jlimit (plot.getX(), plot.getRight(), p.x)));
    setParam (dragBand, kGain, yToDb (juce::jlimit (plot.getY(), plot.getBottom(), p.y)));
    repaint();
}

void EqGraph::mouseUp (const juce::MouseEvent&)
{
    if (dragBand < 0)
        return;

    endGesture (dragBand, kFreq);
    endGesture (dragBand, kGain);
    dragBand = -1;
    repaint();
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int hit = findNodeAt (e.position);
    if (hit < 0)
        return;

    if (auto* p = proc.apvts.getParameter (paramId (hit, kGain)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getDefaultValue());
        p->endChangeGesture();
    }
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    int band = findNodeAt (e.position);
    if (band < 0)
        band = highlightedBand();
    if (band < 0)
        return;

    const float factor = std::exp (juce::jlimit (-0.5f, 0.5f, wheel.deltaY) * 0.5f);
    const float width  = juce::jlimit (widthMin, widthMax, readBand (band).width * factor);

    beginGesture (band, kWidth);
    setParam (band, kWidth, width);
    endGesture (band, kWidth);
    repaint();
}
