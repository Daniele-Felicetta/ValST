#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "Params.h"

//==============================================================================
//  DSP di ValST MS EQ: tre campane (peaking EQ) che lavorano separatamente su Mid e Side.
//  Lo stesso codice calcola i coefficienti per l'audio e per il grafico, cosi' quello che
//  si vede nel display e' esattamente quello che si sente.
//==============================================================================
namespace valst
{
    //==========================================================================
    /** Biquad (forma trasposta II, in double) con filtro "peaking" di Robert Bristow-Johnson.
        La larghezza e' espressa in ottave, con pre-warping: l'ampiezza della campana
        resta quella impostata anche vicino a Nyquist. */
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // a0 normalizzato a 1
        double z1 = 0.0, z2 = 0.0;

        void reset() noexcept { z1 = z2 = 0.0; }

        inline double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void setPeaking (double sampleRate, double freqHz, double widthOct, double gainDb) noexcept
        {
            const double f    = juce::jlimit (10.0, 0.49 * sampleRate, freqHz);
            const double w0   = juce::MathConstants<double>::twoPi * f / sampleRate;
            const double sinW = std::sin (w0);
            const double cosW = std::cos (w0);
            const double arg  = juce::jmin (40.0, 0.34657359027997264 * widthOct * w0 / sinW);   // ln(2)/2 * BW * w0/sin(w0)
            const double alpha = sinW * std::sinh (arg);
            const double A     = std::pow (10.0, gainDb / 40.0);

            const double inv = 1.0 / (1.0 + alpha / A);
            b0 = (1.0 + alpha * A) * inv;
            b1 = -2.0 * cosW * inv;
            b2 = (1.0 - alpha * A) * inv;
            a1 = b1;
            a2 = (1.0 - alpha / A) * inv;
        }

        /** |H(f)|^2 */
        double magnitudeSquared (double freqHz, double sampleRate) const noexcept
        {
            const double w  = juce::MathConstants<double>::twoPi * freqHz / sampleRate;
            const double c1 = std::cos (w);
            const double c2 = std::cos (2.0 * w);
            const double num = b0 * b0 + b1 * b1 + b2 * b2 + 2.0 * (b0 * b1 + b1 * b2) * c1 + 2.0 * b0 * b2 * c2;
            const double den = 1.0 + a1 * a1 + a2 * a2 + 2.0 * (a1 + a1 * a2) * c1 + 2.0 * a2 * c2;
            return num / den;
        }

        double magnitudeDb (double freqHz, double sampleRate) const noexcept
        {
            return 10.0 * std::log10 (juce::jmax (1.0e-12, magnitudeSquared (freqHz, sampleRate)));
        }
    };

    //==========================================================================
    struct BandGains
    {
        double midDb  = 0.0;
        double sideDb = 0.0;
    };

    /** Come il pomello GAIN viene distribuito tra Mid e Side in funzione del fader.

        msValue = 0      -> neutro: il gain lavora sul segnale stereo (Mid e Side hanno lo stesso gain)
        msValue > 0      -> fader in alto: il gain lavora sul Side; il Mid ne riceve una quota sempre
                            minore (in dB), fino a 0 dB (invariato) a +100
        msValue < 0      -> fader in basso: il gain lavora sul Mid; il Side ne riceve una quota
                            sempre minore, fino a 0 dB (invariato) a -100

        La corsa logaritmica del fader e' gia' nella conversione posizione -> valore (vedi msRange()). */
    inline BandGains computeBandGains (double knobDb, double msValue) noexcept
    {
        const double amount = juce::jlimit (0.0, 1.0, std::abs (msValue) / (double) msMax);
        const double other  = knobDb * (1.0 - amount);

        return msValue >= 0.0 ? BandGains { other, knobDb }     // su   -> Side
                              : BandGains { knobDb, other };    // giu' -> Mid
    }

    //==========================================================================
    /** Motore stereo: codifica L/R -> Mid/Side, 3 campane per canale, decodifica -> L/R.
        I parametri vengono smussati (30 ms) e i coefficienti ricalcolati ogni 32 campioni:
        niente "zipper noise" e nessun click durante le automazioni. */
    class MsEqEngine
    {
    public:
        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate;

            for (auto& b : bands)
            {
                b.freq .reset (sampleRate, smoothingSeconds);
                b.width.reset (sampleRate, smoothingSeconds);
                b.midDb .reset (sampleRate, smoothingSeconds);
                b.sideDb.reset (sampleRate, smoothingSeconds);
                b.mid.reset();
                b.side.reset();
            }
        }

        void reset() noexcept
        {
            for (auto& b : bands)
            {
                b.mid.reset();
                b.side.reset();
            }
        }

        /** Imposta il valore di destinazione di una banda (chiamare a ogni blocco). */
        void setBand (int index, float freqHz, float widthOct, float gainDb, float msValue)
        {
            auto& b = bands[(size_t) index];
            const auto g = computeBandGains (gainDb, msValue);

            b.freq.setTargetValue (juce::jlimit (freqMin, freqMax, freqHz));
            b.width.setTargetValue (juce::jlimit (widthMin, widthMax, widthOct));
            b.midDb.setTargetValue (g.midDb);
            b.sideDb.setTargetValue (g.sideDb);
        }

        /** Salta subito ai valori di destinazione (senza rampa). */
        void snapToTargets()
        {
            for (auto& b : bands)
            {
                b.freq .setCurrentAndTargetValue (b.freq.getTargetValue());
                b.width.setCurrentAndTargetValue (b.width.getTargetValue());
                b.midDb .setCurrentAndTargetValue (b.midDb.getTargetValue());
                b.sideDb.setCurrentAndTargetValue (b.sideDb.getTargetValue());
                updateCoefficients (b);
            }
        }

        void process (float* left, float* right, int numSamples) noexcept
        {
            int pos = 0;

            while (pos < numSamples)
            {
                const int chunk = juce::jmin (subBlockSize, numSamples - pos);

                for (auto& b : bands)
                {
                    if (b.freq.isSmoothing() || b.width.isSmoothing() || b.midDb.isSmoothing() || b.sideDb.isSmoothing())
                    {
                        b.freq .skip (chunk);
                        b.width.skip (chunk);
                        b.midDb .skip (chunk);
                        b.sideDb.skip (chunk);
                        updateCoefficients (b);
                    }
                }

                for (int i = pos; i < pos + chunk; ++i)
                {
                    const double l = (double) left[i];
                    const double r = (double) right[i];
                    double m = 0.5 * (l + r);
                    double s = 0.5 * (l - r);

                    for (auto& b : bands)
                    {
                        m = b.mid.process (m);
                        s = b.side.process (s);
                    }

                    left[i]  = (float) (m + s);
                    right[i] = (float) (m - s);
                }

                pos += chunk;
            }
        }

    private:
        static constexpr int    subBlockSize     = 32;
        static constexpr double smoothingSeconds = 0.03;

        struct Band
        {
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> freq  { 1000.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> width { 1.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         midDb { 0.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         sideDb { 0.0 };
            Biquad mid, side;
        };

        void updateCoefficients (Band& b) noexcept
        {
            const double f = b.freq.getCurrentValue();
            const double w = b.width.getCurrentValue();
            b.mid .setPeaking (sampleRate, f, w, b.midDb.getCurrentValue());
            b.side.setPeaking (sampleRate, f, w, b.sideDb.getCurrentValue());
        }

        double sampleRate = 44100.0;
        std::array<Band, (size_t) numBands> bands;
    };
}
