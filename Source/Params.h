#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

//==============================================================================
//  Definizione dei parametri di ValST MS EQ.
//  Per ogni banda (1..3):
//     freq  : frequenza centrale della campana, 20 Hz - 20 kHz (scala logaritmica)
//     width : larghezza della campana in ottave, 0.1 - 6 (scala logaritmica)
//     gain  : guadagno della banda, -18 / +18 dB
//     ms    : fader Mid/Side, da -100 (il gain lavora solo sul Mid) a +100 (solo sul Side),
//             0 = neutro (il gain lavora sul segnale stereo, cioe' su Mid e Side insieme).
//             La corsa del fader e' logaritmica: precisione fine intorno allo 0,
//             variazioni piu' rapide verso gli estremi.
//     sat   : (dalla 1.1) saturazione della banda, 0 - 100 %. 0 = spenta.
//
//  Parametri globali (dalla 1.1):
//     in_gain   : gain di ingresso con saturazione valvolare, -12 / +24 dB
//     out_gain  : gain di uscita con saturazione a trasformatore, -24 / +12 dB
//     hpf1_freq : passa-alto morbido (6 dB/ott) della banda 1, 20 - 400 Hz (il minimo = spento)
//     hpf3_freq : passa-alto piu' ripido (12 dB/ott) della banda 3, 20 - 1000 Hz (il minimo = spento)
//
//  I parametri della 1.0 hanno gli stessi ID, lo stesso significato e lo stesso ordine; quelli nuovi
//  sono accodati e hanno un valore predefinito neutro, quindi i preset e le automazioni vecchi
//  continuano a funzionare senza cambiare suono.
//==============================================================================
namespace valst
{
    constexpr int   numBands  = 3;
    constexpr float freqMin   = 20.0f;
    constexpr float freqMax   = 20000.0f;
    constexpr float widthMin  = 0.1f;    // ottave
    constexpr float widthMax  = 6.0f;    // ottave
    constexpr float gainMaxDb = 18.0f;
    constexpr float msMax     = 100.0f;

    constexpr float inGainMinDb  = -12.0f;
    constexpr float inGainMaxDb  = 24.0f;
    constexpr float outGainMinDb = -24.0f;
    constexpr float outGainMaxDb = 12.0f;
    constexpr float satMaxPercent = 100.0f;

    constexpr float hpfMinHz          = 20.0f;
    constexpr float hpfOffThresholdHz = 20.5f;     // <= : filtro spento
    constexpr float hpf1MaxHz         = 400.0f;
    constexpr float hpf3MaxHz         = 1000.0f;

    enum ParamKind { kFreq = 0, kWidth, kGain, kMs, kSat, numBandParams };

    inline juce::String paramId (int band, ParamKind kind)
    {
        static const char* const suffix[] = { "freq", "width", "gain", "ms", "sat" };
        return "b" + juce::String (band + 1) + "_" + suffix[(int) kind];
    }

    enum GlobalParam { kInGain = 0, kOutGain, kHpf1, kHpf3, numGlobalParams };

    inline juce::String globalId (GlobalParam p)
    {
        static const char* const ids[] = { "in_gain", "out_gain", "hpf1_freq", "hpf3_freq" };
        return ids[(int) p];
    }

    /** Q equivalente di una larghezza espressa in ottave (usato solo per la visualizzazione). */
    inline double widthToQ (double octaves)
    {
        return 1.0 / (2.0 * std::sinh (0.34657359027997264 * octaves));   // ln(2) / 2
    }

    //==============================================================================
    //  Range dei parametri

    inline juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        return juce::NormalisableRange<float> (
            lo, hi,
            [] (float s, float e, float p) { return s * std::pow (e / s, p); },
            [] (float s, float e, float v) { return std::log (juce::jlimit (s, e, v) / s) / std::log (e / s); },
            [] (float s, float e, float v) { return juce::jlimit (s, e, v); });
    }

    inline juce::NormalisableRange<float> gainRange()
    {
        return juce::NormalisableRange<float> (
            -gainMaxDb, gainMaxDb,
            [] (float s, float e, float p) { return s + (e - s) * p; },
            [] (float s, float e, float v) { return (juce::jlimit (s, e, v) - s) / (e - s); },
            [] (float s, float e, float v) { return juce::jlimit (s, e, std::round (v * 10.0f) / 10.0f) + 0.0f; });   // passi da 0.1 dB
    }

    /** Fader Mid/Side: -100 .. 0 .. +100.
        La posizione del fader (0..1) e' legata al valore da una legge logaritmica, separatamente
        per ciascuna meta': valore = 100 * (10^t - 1) / 9, con t = distanza dal centro (0..1).
        Risultato: vicino allo 0 ci sono molti millimetri di corsa per pochi punti (regolazione fine),
        verso gli estremi i valori cambiano piu' in fretta. Valori interi, 0 facile da centrare. */
    inline juce::NormalisableRange<float> msRange()
    {
        return juce::NormalisableRange<float> (
            -msMax, msMax,
            [] (float, float, float p)
            {
                const float u = (juce::jlimit (0.0f, 1.0f, p) - 0.5f) * 2.0f;          // -1 .. +1
                const float a = (std::pow (10.0f, std::abs (u)) - 1.0f) / 9.0f;         //  0 ..  1
                return (u < 0.0f ? -a : a) * msMax;
            },
            [] (float, float, float v)
            {
                const float a = juce::jlimit (0.0f, 1.0f, std::abs (v) / msMax);
                const float u = std::log10 (1.0f + 9.0f * a);                           //  0 ..  1
                return 0.5f + (v < 0.0f ? -0.5f : 0.5f) * u;
            },
            [] (float, float, float v) { return juce::jlimit (-msMax, msMax, std::round (v)) + 0.0f; });
    }

    //==============================================================================
    //  Testo <-> valore (usato dalla GUI e dalle liste di automazione del DAW)

    inline juce::String freqToText (float hz)
    {
        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";

        return juce::String (hz, hz >= 100.0f ? 0 : 1) + " Hz";
    }

    inline float textToFreq (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        float v = t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue();
        if (t.containsChar ('k'))
            v *= 1000.0f;
        return v;
    }

    inline juce::String widthToText (float oct)    { return juce::String (oct, 2) + " oct"; }

    inline juce::String gainToText (float db)
    {
        if (std::abs (db) < 0.05f)
            db = 0.0f;
        return juce::String (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
    }

    inline juce::String msToText (float ms)
    {
        const int v = juce::roundToInt (ms);
        if (v > 0) return "+" + juce::String (v) + " S";
        if (v < 0) return juce::String (v) + " M";
        return "0";
    }

    inline juce::String satToText (float percent)  { return juce::String (juce::roundToInt (percent)) + " %"; }

    inline juce::String hpfToText (float hz)       { return hz <= hpfOffThresholdHz ? juce::String ("OFF") : freqToText (hz); }

    inline float textToHpf (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        if (t.isEmpty() || t.startsWith ("off"))
            return hpfMinHz;
        return textToFreq (t);
    }

    //==============================================================================
    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using juce::AudioParameterFloat;
        using juce::AudioParameterFloatAttributes;
        using juce::ParameterID;
        using juce::String;

        static const float defaultFreq[numBands] = { 100.0f, 1000.0f, 8000.0f };

        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        for (int b = 0; b < numBands; ++b)
        {
            const String prefix = "Band " + String (b + 1) + " ";

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { paramId (b, kFreq), 1 }, prefix + "Freq", logRange (freqMin, freqMax), defaultFreq[b],
                AudioParameterFloatAttributes()
                    .withLabel ("Hz")
                    .withStringFromValueFunction ([] (float v, int) { return freqToText (v); })
                    .withValueFromStringFunction ([] (const String& s) { return textToFreq (s); })));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { paramId (b, kWidth), 1 }, prefix + "Width", logRange (widthMin, widthMax), 1.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("oct")
                    .withStringFromValueFunction ([] (float v, int) { return widthToText (v); })
                    .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue(); })));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { paramId (b, kGain), 1 }, prefix + "Gain", gainRange(), 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("dB")
                    .withStringFromValueFunction ([] (float v, int) { return gainToText (v); })
                    .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue(); })));

            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { paramId (b, kMs), 1 }, prefix + "Mid/Side", msRange(), 0.0f,
                AudioParameterFloatAttributes()
                    .withStringFromValueFunction ([] (float v, int) { return msToText (v); })
                    .withValueFromStringFunction ([] (const String& s)
                    {
                        // accetta "50", "+50", "-50", "50 S", "s50", "m 50" ...
                        const auto t = s.trim().toLowerCase();
                        const float v = t.retainCharacters ("0123456789.").getFloatValue();
                        if (t.containsChar ('m') || t.startsWithChar ('-'))
                            return -v;
                        return v;
                    })));
        }

        //----------------------------------------------------------------------
        //  Dalla 1.1: parametri accodati (ID nuovi, versione 2), tutti con valore predefinito neutro.
        const auto gainAttributes = [] (const char* label)
        {
            return AudioParameterFloatAttributes()
                       .withLabel (label)
                       .withStringFromValueFunction ([] (float v, int) { return gainToText (v); })
                       .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue(); });
        };

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { globalId (kInGain), 2 }, "Input Gain",
            juce::NormalisableRange<float> (inGainMinDb, inGainMaxDb, 0.1f), 0.0f, gainAttributes ("dB")));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { globalId (kOutGain), 2 }, "Output Gain",
            juce::NormalisableRange<float> (outGainMinDb, outGainMaxDb, 0.1f), 0.0f, gainAttributes ("dB")));

        for (int b = 0; b < numBands; ++b)
            layout.add (std::make_unique<AudioParameterFloat> (
                ParameterID { paramId (b, kSat), 2 }, "Band " + String (b + 1) + " Saturation",
                juce::NormalisableRange<float> (0.0f, satMaxPercent, 1.0f), 0.0f,
                AudioParameterFloatAttributes()
                    .withLabel ("%")
                    .withStringFromValueFunction ([] (float v, int) { return satToText (v); })
                    .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue(); })));

        const auto hpfAttributes = AudioParameterFloatAttributes()
                                       .withLabel ("Hz")
                                       .withStringFromValueFunction ([] (float v, int) { return hpfToText (v); })
                                       .withValueFromStringFunction ([] (const String& s) { return textToHpf (s); });

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { globalId (kHpf1), 2 }, "Band 1 Low Cut", logRange (hpfMinHz, hpf1MaxHz), hpfMinHz, hpfAttributes));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { globalId (kHpf3), 2 }, "Band 3 Low Cut", logRange (hpfMinHz, hpf3MaxHz), hpfMinHz, hpfAttributes));

        return layout;
    }
}
