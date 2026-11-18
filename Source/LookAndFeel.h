#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "Params.h"

//==============================================================================
//  Colori e stile grafico di ValST MS EQ
//==============================================================================
namespace valst::ui
{
    inline juce::Colour bgColour()      { return juce::Colour (0xff0d1014); }
    inline juce::Colour panelColour()   { return juce::Colour (0xff161a21); }
    inline juce::Colour edgeColour()    { return juce::Colour (0xff272d38); }
    inline juce::Colour gridColour()    { return juce::Colour (0xff262c37); }
    inline juce::Colour textColour()    { return juce::Colour (0xffdbe2ec); }
    inline juce::Colour dimTextColour() { return juce::Colour (0xff8591a3); }
    inline juce::Colour midColour()     { return juce::Colour (0xffffae2b); }   // ambra  -> Mid
    inline juce::Colour sideColour()    { return juce::Colour (0xff35c6ff); }   // ciano  -> Side
    inline juce::Colour neutralColour() { return juce::Colour (0xffe9eef5); }   // bianco -> stereo (Mid + Side)
    inline juce::Colour knobAccent()    { return juce::Colour (0xffa9bad3); }

    /** Colore che indica su quale canale lavora il guadagno:
        bianco = stereo (fader a 0), ciano = Side (fader in alto), ambra = Mid (fader in basso). */
    inline juce::Colour msColour (float ms)
    {
        const float amount = std::sqrt (juce::jlimit (0.0f, 1.0f, std::abs (ms) / valst::msMax));
        return neutralColour().interpolatedWith (ms >= 0.0f ? sideColour() : midColour(), amount);
    }

    /** Stringa UTF-8 (per i testi italiani con le lettere accentate). */
    inline juce::String it (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

//==============================================================================
class ValstLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ValstLookAndFeel()
    {
        using namespace valst::ui;

        setColour (juce::Slider::textBoxTextColourId,       textColour());
        setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId,  sideColour().withAlpha (0.35f));
        setColour (juce::Slider::rotarySliderFillColourId,  knobAccent());
        setColour (juce::Label::textColourId,               textColour());
        setColour (juce::TextEditor::backgroundColourId,    bgColour());
        setColour (juce::TextEditor::textColourId,          textColour());
        setColour (juce::TextEditor::outlineColourId,       edgeColour());
        setColour (juce::TextEditor::focusedOutlineColourId, sideColour());
        setColour (juce::CaretComponent::caretColourId,     textColour());
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xf2202632));
        setColour (juce::TooltipWindow::textColourId,       textColour());
        setColour (juce::TooltipWindow::outlineColourId,    edgeColour());
    }

    //==========================================================================
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace valst::ui;

        const auto area    = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
        const float size   = juce::jmin (area.getWidth(), area.getHeight());
        const auto centre  = area.getCentre();
        const float radius = size * 0.5f;
        const float thick  = juce::jmax (2.5f, radius * 0.12f);
        const float arcR   = radius - thick * 0.5f;
        const float bodyR  = radius - thick - 2.5f;
        const float angle  = startAngle + pos * (endAngle - startAngle);
        const bool bipolar = (bool) slider.getProperties().getWithDefault ("bipolar", false);
        const auto accent  = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // corpo del pomello
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff303845), centre.x, centre.y - bodyR,
                                                 juce::Colour (0xff181c23), centre.x, centre.y + bodyR, false));
        g.fillEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
        g.setColour (juce::Colour (0xff3d4655));
        g.drawEllipse (centre.x - bodyR, centre.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

        // binario
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (juce::Colour (0xff262c37));
        g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // arco del valore (dal centro per i pomelli bipolari, come il GAIN)
        const float zeroAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        if (std::abs (angle - zeroAngle) > 0.002f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, zeroAngle, angle, true);
            g.setColour (accent);
            g.strokePath (value, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // lancetta
        const float inner = bodyR * 0.30f;
        const float outer = bodyR * 0.90f;
        juce::Path pointer;
        pointer.startNewSubPath (centre.x + inner * std::sin (angle), centre.y - inner * std::cos (angle));
        pointer.lineTo          (centre.x + outer * std::sin (angle), centre.y - outer * std::cos (angle));
        g.setColour (neutralColour());
        g.strokePath (pointer, juce::PathStrokeType (juce::jmax (2.0f, radius * 0.075f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    //==========================================================================
    int getSliderThumbRadius (juce::Slider&) override { return 8; }   // mezza altezza del cursore del fader + 1

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            return;
        }

        using namespace valst::ui;

        juce::ignoreUnused (minSliderPos, maxSliderPos);          // negli slider a valore singolo JUCE non li usa per gli estremi

        const float top     = (float) y;                          // posizione del valore massimo (in alto)
        const float bottom  = (float) (y + height);               // posizione del valore minimo (in basso)
        const float cx      = (float) x + (float) width * 0.34f;
        const float grooveW = 6.0f;

        // y di un valore sulla scala: tiene conto della legge logaritmica del fader
        auto yFor = [&] (double value)
        {
            return top + (bottom - top) * (1.0f - (float) slider.valueToProportionOfLength (value));
        };

        const float zeroY   = yFor (0.0);
        const float value   = (float) slider.getValue();
        const auto  channel = msColour (value);

        // binario
        const juce::Rectangle<float> groove (cx - grooveW * 0.5f, top, grooveW, bottom - top);
        g.setColour (juce::Colour (0xff090b0e));
        g.fillRoundedRectangle (groove, 3.0f);
        g.setColour (edgeColour());
        g.drawRoundedRectangle (groove, 3.0f, 1.0f);

        // tratto attivo: dal centro (neutro) fino al cursore
        if (std::abs (sliderPos - zeroY) > 0.5f)
        {
            g.setColour (channel.withAlpha (0.92f));
            g.fillRoundedRectangle (groove.getX() + 1.0f, juce::jmin (sliderPos, zeroY),
                                    groove.getWidth() - 2.0f, std::abs (sliderPos - zeroY), 2.0f);
        }

        // linea del centro (neutro)
        g.setColour (neutralColour().withAlpha (0.55f));
        g.drawHorizontalLine (juce::roundToInt (zeroY), groove.getX() - 6.0f, groove.getRight() + 6.0f);

        // scala: -100 ... 0 ... +100, spaziata secondo la legge logaritmica del fader
        struct Tick { int value; bool label; };
        static const Tick ticks[] = { { 100, true }, { 50, true }, { 25, false }, { 10, true }, { 0, true },
                                      { -10, true }, { -25, false }, { -50, true }, { -100, true } };

        g.setFont (juce::FontOptions (10.0f));
        for (const auto& t : ticks)
        {
            const float ty    = yFor ((double) t.value);
            const bool  major = (t.value == 0 || std::abs (t.value) == 100);
            const float x0    = groove.getRight() + 3.0f;

            g.setColour (dimTextColour().withAlpha (major ? 0.95f : 0.55f));
            g.drawHorizontalLine (juce::roundToInt (ty), x0, x0 + (major ? 8.0f : 5.0f));

            if (t.label)
            {
                const auto text = t.value > 0 ? "+" + juce::String (t.value) : juce::String (t.value);
                g.setColour (t.value > 0 ? sideColour().withAlpha (0.85f)
                                         : t.value < 0 ? midColour().withAlpha (0.85f)
                                                       : textColour());
                g.drawText (text, juce::Rectangle<float> (x0 + 10.0f, ty - 6.0f, 30.0f, 12.0f),
                            juce::Justification::centredLeft, false);
            }
        }

        // cursore
        const float thumbW = 24.0f, thumbH = 14.0f;
        const juce::Rectangle<float> thumb (cx - thumbW * 0.5f, sliderPos - thumbH * 0.5f, thumbW, thumbH);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff566074), thumb.getX(), thumb.getY(),
                                                 juce::Colour (0xff2b323e), thumb.getX(), thumb.getBottom(), false));
        g.fillRoundedRectangle (thumb, 3.0f);
        g.setColour (juce::Colour (0xff7a8599));
        g.drawRoundedRectangle (thumb.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (channel);
        g.fillRect (thumb.getX() + 4.0f, thumb.getCentreY() - 1.0f, thumbW - 8.0f, 2.0f);
    }

    //==========================================================================
    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox (slider);
        label->setFont (juce::FontOptions (12.5f));
        return label;
    }
};
