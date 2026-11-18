#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "Params.h"
#include "Saturation.h"

//==============================================================================
//  DSP di ValST MS EQ.
//
//  Percorso del segnale (per ogni canale, in double):
//
//    IN -> [valvola: gain + saturazione] -> L/R -> M/S
//       -> [passa-alto morbido, banda 1]
//       -> banda 1: campana -> saturazione di banda
//       -> banda 2: campana -> saturazione di banda        (ogni banda lavora sul segnale
//       -> banda 3: campana -> saturazione di banda         gia' elaborato da quella prima)
//       -> [passa-alto piu' ripido, banda 3]
//       -> M/S -> L/R -> [gain di uscita -> trasformatore] -> OUT
//
//  Lo stesso codice calcola i coefficienti per l'audio e per il grafico, cosi' quello che
//  si vede nel display e' esattamente quello che si sente.
//==============================================================================
namespace valst
{
    //==========================================================================
    /** Biquad (forma trasposta II, in double) con i filtri usati dal plugin.
        La larghezza delle campane e' in ottave, con pre-warping: l'ampiezza della campana
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

        void copyCoefficientsFrom (const Biquad& o) noexcept
        {
            b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2;
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

        /** Passa-banda a guadagno 0 dB al centro, stessa larghezza (in ottave) della campana: isola la fetta
            di spettro che entra nella saturazione della banda. */
        void setBandPass (double sampleRate, double freqHz, double widthOct) noexcept
        {
            const double f    = juce::jlimit (10.0, 0.49 * sampleRate, freqHz);
            const double w0   = juce::MathConstants<double>::twoPi * f / sampleRate;
            const double sinW = std::sin (w0);
            const double cosW = std::cos (w0);
            const double arg  = juce::jmin (40.0, 0.34657359027997264 * widthOct * w0 / sinW);
            const double alpha = sinW * std::sinh (arg);

            const double inv = 1.0 / (1.0 + alpha);
            b0 = alpha * inv;
            b1 = 0.0;
            b2 = -alpha * inv;
            a1 = -2.0 * cosW * inv;
            a2 = (1.0 - alpha) * inv;
        }

        /** Passa-alto del primo ordine (6 dB/ottava), trasformata bilineare con pre-warping. */
        void setHighPass1 (double sampleRate, double freqHz) noexcept
        {
            const double f   = juce::jlimit (1.0, 0.49 * sampleRate, freqHz);
            const double K   = std::tan (juce::MathConstants<double>::pi * f / sampleRate);
            const double inv = 1.0 / (1.0 + K);
            b0 = inv;
            b1 = -inv;
            b2 = 0.0;
            a1 = (K - 1.0) * inv;
            a2 = 0.0;
        }

        /** Passa-alto del secondo ordine (12 dB/ottava), RBJ. */
        void setHighPass2 (double sampleRate, double freqHz, double q) noexcept
        {
            const double f    = juce::jlimit (1.0, 0.49 * sampleRate, freqHz);
            const double w0   = juce::MathConstants<double>::twoPi * f / sampleRate;
            const double cosW = std::cos (w0);
            const double alpha = std::sin (w0) / (2.0 * q);

            const double inv = 1.0 / (1.0 + alpha);
            b0 = 0.5 * (1.0 + cosW) * inv;
            b1 = -(1.0 + cosW) * inv;
            b2 = b0;
            a1 = -2.0 * cosW * inv;
            a2 = (1.0 - alpha) * inv;
        }

        /** Shelf del primo ordine: guadagno (1 + boost) in continua, 1 in alta frequenza, angolo a freqHz.
            Con inverse = true i coefficienti sono quelli del filtro inverso (esatta cancellazione). */
        void setFluxShelf (double sampleRate, double freqHz, double boost, bool inverse) noexcept
        {
            const double K   = std::tan (juce::MathConstants<double>::pi * freqHz / sampleRate);
            const double nb0 = 1.0 + (1.0 + boost) * K;
            const double nb1 = (1.0 + boost) * K - 1.0;
            const double da0 = 1.0 + K;
            const double da1 = K - 1.0;

            if (! inverse) { b0 = nb0 / da0; b1 = nb1 / da0; a1 = da1 / da0; }
            else           { b0 = da0 / nb0; b1 = da1 / nb0; a1 = nb1 / nb0; }

            b2 = 0.0;
            a2 = 0.0;
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
    /** Filtri passa-alto delle bande 1 e 3 (usati sia dall'audio sia dal grafico).
        Banda 1: primo ordine, 6 dB/ottava: pendenza graduale, transizione molto dolce.
        Banda 3: secondo ordine, 12 dB/ottava (Q 0.6, senza picco): piu' ripido, attenuazione piu' marcata. */
    constexpr double hpf3Q = 0.6;

    inline Biquad makeBand1HighPass (double sampleRate, double freqHz) noexcept
    {
        Biquad b;
        b.setHighPass1 (sampleRate, freqHz);
        return b;
    }

    inline Biquad makeBand3HighPass (double sampleRate, double freqHz) noexcept
    {
        Biquad b;
        b.setHighPass2 (sampleRate, freqHz, hpf3Q);
        return b;
    }

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

    /** Stessa regola del gain, applicata alla SATURAZIONE della banda: lavora dove lavora il fader.
        Il canale scelto riceve la saturazione intera, l'altro la quota (1 - |fader|/100). */
    struct BandWeights
    {
        double mid = 1.0;
        double side = 1.0;
    };

    inline BandWeights computeBandWeights (double msValue) noexcept
    {
        const double amount = juce::jlimit (0.0, 1.0, std::abs (msValue) / (double) msMax);
        const double other  = 1.0 - amount;

        return msValue >= 0.0 ? BandWeights { other, 1.0 } : BandWeights { 1.0, other };
    }

    //==========================================================================
    /** Risposta in frequenza del percorso LINEARE (campane + passa-alto) per i segnali Mid e Side.
        E' il modello usato dal grafico e dai test: stessi coefficienti dell'audio. La saturazione non
        compare perche' a livelli bassi (dove una curva di risposta ha senso) e' trasparente. */
    class LinearResponse
    {
    public:
        struct BandSetting
        {
            float freq = 1000.0f, width = 1.0f, gainDb = 0.0f, ms = 0.0f;
        };

        void setup (double newSampleRate, const std::array<BandSetting, (size_t) numBands>& settings,
                    float hpf1Hz, float hpf3Hz) noexcept
        {
            sampleRate = newSampleRate;

            for (size_t b = 0; b < (size_t) numBands; ++b)
            {
                const auto g = computeBandGains (settings[b].gainDb, settings[b].ms);
                mid [b].setPeaking (sampleRate, settings[b].freq, settings[b].width, g.midDb);
                side[b].setPeaking (sampleRate, settings[b].freq, settings[b].width, g.sideDb);
            }

            hp1On = hpf1Hz > hpfOffThresholdHz;
            hp3On = hpf3Hz > hpfOffThresholdHz;

            if (hp1On) hp1 = makeBand1HighPass (sampleRate, juce::jlimit (hpfMinHz, hpf1MaxHz, hpf1Hz));
            if (hp3On) hp3 = makeBand3HighPass (sampleRate, juce::jlimit (hpfMinHz, hpf3MaxHz, hpf3Hz));
        }

        /** Contributo (dB) della campana di una banda sul canale Mid (sidePath = false) o Side (true). */
        double bandDb (int band, bool sidePath, double hz) const noexcept
        {
            const auto& f = sidePath ? side[(size_t) band] : mid[(size_t) band];
            return f.magnitudeDb (hz, sampleRate);
        }

        /** Contributo (dB) dei due passa-alto: e' uguale per Mid e Side. */
        double highPassDb (double hz) const noexcept
        {
            return (hp1On ? hp1.magnitudeDb (hz, sampleRate) : 0.0) + (hp3On ? hp3.magnitudeDb (hz, sampleRate) : 0.0);
        }

        /** Risposta complessiva del percorso lineare. */
        double totalDb (bool sidePath, double hz) const noexcept
        {
            double db = highPassDb (hz);
            for (int b = 0; b < numBands; ++b)
                db += bandDb (b, sidePath, hz);
            return db;
        }

    private:
        double sampleRate = 48000.0;
        std::array<Biquad, (size_t) numBands> mid, side;
        Biquad hp1, hp3;
        bool hp1On = false, hp3On = false;
    };

    //==========================================================================
    /** Motore stereo.
        I parametri vengono smussati (30 ms) e i coefficienti ricalcolati ogni 32 campioni:
        niente "zipper noise" e nessun click durante le automazioni. Nessuna allocazione, nessun lock. */
    class MsEqEngine
    {
    public:
        MsEqEngine()
        {
            for (size_t i = 0; i < bands.size(); ++i)
            {
                bands[i].seed = sat::defaultSeeds[i];
                bands[i].character = sat::makeCharacter (bands[i].seed);
            }

            highPass[0].secondOrder = false;
            highPass[1].secondOrder = true;
            valveShaper = sat::makeValveShaper();
            trafoShaper = sat::makeTransformerShaper();
        }

        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate;

            inGainDb .reset (sampleRate, smoothingSeconds);
            outGainDb.reset (sampleRate, smoothingSeconds);

            for (auto& v : valve)
            {
                v.dc.prepare (sampleRate);
                v.adaa.prepare (sampleRate);
            }

            for (auto& t : trafo)
            {
                t.pre .setFluxShelf (sampleRate, sat::trafoFluxCorner, sat::trafoFluxBoost, false);
                t.post.setFluxShelf (sampleRate, sat::trafoFluxCorner, sat::trafoFluxBoost, true);
                t.adaa.prepare (sampleRate);
            }

            for (auto& b : bands)
            {
                b.freq.reset (sampleRate, smoothingSeconds);
                b.width.reset (sampleRate, smoothingSeconds);
                b.midDb .reset (sampleRate, smoothingSeconds);
                b.sideDb.reset (sampleRate, smoothingSeconds);
                b.satAmount.reset (sampleRate, smoothingSeconds);
                b.wMid .reset (sampleRate, smoothingSeconds);
                b.wSide.reset (sampleRate, smoothingSeconds);
                b.mid.dc.prepare (sampleRate);
                b.side.dc.prepare (sampleRate);
                b.mid.adaa.prepare (sampleRate);
                b.side.adaa.prepare (sampleRate);
            }

            for (auto& h : highPass)
            {
                h.freq.reset (sampleRate, smoothingSeconds);
                h.mix .reset (sampleRate, smoothingSeconds);
            }

            reset();
        }

        void reset() noexcept
        {
            for (auto& v : valve)  { v.adaa.reset(); v.dc.reset(); }
            for (auto& t : trafo)  { t.pre.reset(); t.post.reset(); t.adaa.reset(); }
            for (auto& h : highPass) { h.mid.reset(); h.side.reset(); }

            for (auto& b : bands)
            {
                b.mid.eq.reset();
                b.side.eq.reset();
                b.mid.resetSaturation();
                b.side.resetSaturation();
            }
        }

        //----------------------------------------------------------------------
        //  Impostazioni (chiamare a ogni blocco: sono valori di destinazione, vengono smussati)

        void setInput (float gainDb)
        {
            inGainDb.setTargetValue (juce::jlimit (inGainMinDb, inGainMaxDb, gainDb));
        }

        void setOutput (float gainDb)
        {
            outGainDb.setTargetValue (juce::jlimit (outGainMinDb, outGainMaxDb, gainDb));
        }

        /** satAmount01: manopola SAT della banda, 0..1. */
        void setBand (int index, float freqHz, float widthOct, float gainDb, float msValue, float satAmount01)
        {
            auto& b = bands[(size_t) index];
            const auto g = computeBandGains (gainDb, msValue);
            const auto w = computeBandWeights (msValue);

            b.freq.setTargetValue (juce::jlimit (freqMin, freqMax, freqHz));
            b.width.setTargetValue (juce::jlimit (widthMin, widthMax, widthOct));
            b.midDb.setTargetValue (g.midDb);
            b.sideDb.setTargetValue (g.sideDb);
            b.satAmount.setTargetValue (juce::jlimit (0.0, 1.0, (double) satAmount01));
            b.wMid.setTargetValue (w.mid);
            b.wSide.setTargetValue (w.side);
        }

        /** which: 0 = passa-alto della banda 1 (6 dB/ott), 1 = passa-alto della banda 3 (12 dB/ott).
            Una frequenza <= hpfOffThresholdHz spegne il filtro (bypass esatto). */
        void setHighPass (int which, float freqHz)
        {
            auto& h = highPass[(size_t) which];
            const bool on = freqHz > hpfOffThresholdHz;

            h.freq.setTargetValue (juce::jlimit (hpfMinHz, which == 0 ? hpf1MaxHz : hpf3MaxHz, freqHz));
            h.mix.setTargetValue (on ? 1.0 : 0.0);
        }

        /** Seed persistente dell'identita' timbrica della banda (vedi Saturation.h). */
        void setSeed (int index, uint64_t seed) noexcept
        {
            auto& b = bands[(size_t) index];

            if (seed != b.seed)
            {
                b.seed = seed;
                b.character = sat::makeCharacter (seed);
                b.dirty = true;
            }
        }

        /** Solo per i test: disattiva l'anti-aliasing ADAA per misurarne l'effetto. */
        void setAntiAliasing (bool enabled) noexcept { antiAlias = enabled; }

        /** Salta subito ai valori di destinazione (senza rampa). */
        void snapToTargets()
        {
            inGainDb .setCurrentAndTargetValue (inGainDb.getTargetValue());
            outGainDb.setCurrentAndTargetValue (outGainDb.getTargetValue());
            updateValve (true);
            updateOutput (true);

            for (auto& b : bands)
            {
                b.freq .setCurrentAndTargetValue (b.freq.getTargetValue());
                b.width.setCurrentAndTargetValue (b.width.getTargetValue());
                b.midDb .setCurrentAndTargetValue (b.midDb.getTargetValue());
                b.sideDb.setCurrentAndTargetValue (b.sideDb.getTargetValue());
                b.satAmount.setCurrentAndTargetValue (b.satAmount.getTargetValue());
                b.wMid .setCurrentAndTargetValue (b.wMid.getTargetValue());
                b.wSide.setCurrentAndTargetValue (b.wSide.getTargetValue());
                updateBand (b);
            }

            for (auto& h : highPass)
            {
                h.freq.setCurrentAndTargetValue (h.freq.getTargetValue());
                h.mix .setCurrentAndTargetValue (h.mix.getTargetValue());
                updateHighPass (h);
            }
        }

        //----------------------------------------------------------------------
        void process (float* left, float* right, int numSamples) noexcept
        {
            int pos = 0;

            while (pos < numSamples)
            {
                const int chunk = juce::jmin (subBlockSize, numSamples - pos);

                advanceControl (chunk);

                for (int i = pos; i < pos + chunk; ++i)
                {
                    double l = (double) left[i];
                    double r = (double) right[i];

                    // un valore non numerico in ingresso non deve mai propagarsi negli stati dei filtri
                    if (! std::isfinite (l)) l = 0.0;
                    if (! std::isfinite (r)) r = 0.0;

                    // 1. gain di ingresso + saturazione valvolare (L e R separati, come un preamplificatore stereo)
                    const double pre  = preGain.next();
                    const double post = postGain.next();
                    l = runValve (valve[0], l * pre, post);
                    r = runValve (valve[1], r * pre, post);

                    // 2. Mid/Side
                    double m = 0.5 * (l + r);
                    double s = 0.5 * (l - r);

                    // 3. passa-alto morbido (banda 1)
                    runHighPass (highPass[0], m, s);

                    // 4. le tre bande in serie: campana + saturazione di banda
                    for (auto& b : bands)
                    {
                        m = runBandChannel (b, b.mid,  m);
                        s = runBandChannel (b, b.side, s);
                    }

                    // 5. passa-alto piu' ripido (banda 3)
                    runHighPass (highPass[1], m, s);

                    // 6. L/R, gain di uscita + saturazione a trasformatore
                    const double out = outGain.next();
                    left[i]  = (float) runTransformer (trafo[0], (m + s) * out);
                    right[i] = (float) runTransformer (trafo[1], (m - s) * out);
                }

                finishRamps();
                pos += chunk;
            }
        }

    private:
        static constexpr int    subBlockSize     = 32;
        static constexpr double smoothingSeconds = 0.03;

        //----------------------------------------------------------------------
        struct Ramp
        {
            double value = 1.0, target = 1.0, step = 0.0;

            void snap (double v) noexcept            { value = target = v; step = 0.0; }
            void aim (double t, int n) noexcept      { target = t; step = (t - value) / (double) n; }
            double next() noexcept                   { value += step; return value; }
            void land() noexcept                     { value = target; step = 0.0; }
        };

        struct ValveChannel
        {
            sat::AdaaResidual adaa;
            sat::DcBlocker dc;
        };

        struct BandChannel
        {
            Biquad eq, bp;
            sat::DcBlocker dc;
            sat::AdaaResidual adaa;
            sat::Shaper shaper;
            double drive = 1.0, weight = 0.0;

            void resetSaturation() noexcept { bp.reset(); dc.reset(); adaa.reset(); }
        };

        struct Band
        {
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> freq  { 1000.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> width { 1.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         midDb { 0.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         sideDb { 0.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         satAmount { 0.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         wMid  { 1.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         wSide { 1.0 };

            uint64_t seed = 0;
            sat::Character character;
            bool satActive = false;
            bool dirty = true;

            BandChannel mid, side;

            bool isSmoothing() const noexcept
            {
                return freq.isSmoothing() || width.isSmoothing() || midDb.isSmoothing() || sideDb.isSmoothing()
                    || satAmount.isSmoothing() || wMid.isSmoothing() || wSide.isSmoothing();
            }
        };

        struct HighPass
        {
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> freq { 100.0 };
            juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear>         mix  { 0.0 };
            Biquad mid, side;
            bool secondOrder = false;
            bool active = false;
        };

        struct TrafoChannel
        {
            Biquad pre, post;           // enfasi / de-enfasi del flusso (esattamente inversi)
            sat::AdaaResidual adaa;
        };

        //----------------------------------------------------------------------
        void advanceControl (int chunk) noexcept
        {
            if (inGainDb.isSmoothing())
            {
                inGainDb.skip (chunk);
                updateValve (false, chunk);
            }
            else
            {
                preGain.aim (preGain.target, chunk);       // gain costante: rampa piatta
                postGain.aim (postGain.target, chunk);
            }

            if (outGainDb.isSmoothing())
            {
                outGainDb.skip (chunk);
                updateOutput (false, chunk);
            }
            else
            {
                outGain.aim (outGain.target, chunk);
            }

            for (auto& b : bands)
            {
                if (b.isSmoothing() || b.dirty)
                {
                    b.freq .skip (chunk);
                    b.width.skip (chunk);
                    b.midDb .skip (chunk);
                    b.sideDb.skip (chunk);
                    b.satAmount.skip (chunk);
                    b.wMid .skip (chunk);
                    b.wSide.skip (chunk);
                    updateBand (b);
                }
            }

            for (auto& h : highPass)
            {
                if (h.freq.isSmoothing() || h.mix.isSmoothing())
                {
                    h.freq.skip (chunk);
                    h.mix .skip (chunk);
                    updateHighPass (h);
                }
            }
        }

        void finishRamps() noexcept
        {
            preGain.land();
            postGain.land();
            outGain.land();
        }

        //----------------------------------------------------------------------
        //  Valvola
        void updateValve (bool snap, int chunk = 1) noexcept
        {
            const double db    = inGainDb.getCurrentValue();
            const double pre   = std::pow (10.0, db / 20.0);
            const double post  = sat::valveCompensationLinear (db);
            const double kappa = sat::valveKappa (db);

            if (snap) { preGain.snap (pre); postGain.snap (post); }
            else      { preGain.aim (pre, chunk); postGain.aim (post, chunk); }

            valveOn = kappa > 1.0e-4;

            if (! valveOn)
            {
                for (auto& v : valve) { v.adaa.reset(); v.dc.reset(); }
                valveKappa = 0.0;
                return;
            }

            if (valveKappa > 0.0 && kappa != valveKappa)
                for (auto& v : valve)
                    v.adaa.rescale (kappa / valveKappa);      // la scala dell'ingresso della curva e' cambiata

            valveKappa = kappa;
            for (auto& v : valve)
                v.adaa.refresh (valveShaper);
        }

        inline double runValve (ValveChannel& v, double u, double post) noexcept
        {
            if (valveOn)
            {
                const double res = v.adaa.process (valveShaper, valveKappa * u, antiAlias) / valveKappa;
                u += v.dc.process (res);
            }

            return u * post;
        }

        //----------------------------------------------------------------------
        //  Uscita
        void updateOutput (bool snap, int chunk = 1) noexcept
        {
            const double gain = std::pow (10.0, outGainDb.getCurrentValue() / 20.0);

            if (snap) outGain.snap (gain);
            else      outGain.aim (gain, chunk);
        }

        inline double runTransformer (TrafoChannel& t, double x) noexcept
        {
            const double e   = t.pre.process (x);
            const double res = t.adaa.process (trafoShaper, sat::trafoDrive * e, antiAlias) / sat::trafoDrive;
            return x + t.post.process (res);
        }

        //----------------------------------------------------------------------
        //  Bande
        static void prepareSaturation (BandChannel& c, double drive, double weight, double freq, double width,
                                       double gainAbsDb, const sat::Character& character) noexcept
        {
            const double oldDrive = c.drive;

            double driveScale = 1.0;
            sat::computeBandShape (character, freq, width, gainAbsDb, c.shaper, driveScale);
            const double newDrive = drive * driveScale;

            if (oldDrive > 0.0 && newDrive != oldDrive)
                c.adaa.rescale (newDrive / oldDrive);  // la scala dell'ingresso della curva e' cambiata

            c.drive  = newDrive;
            c.weight = weight;
            c.adaa.refresh (c.shaper);
        }

        void updateBand (Band& b) noexcept
        {
            const double f  = b.freq.getCurrentValue();
            const double w  = b.width.getCurrentValue();
            const double gm = b.midDb.getCurrentValue();
            const double gs = b.sideDb.getCurrentValue();
            const double s  = b.satAmount.getCurrentValue();

            b.mid .eq.setPeaking (sampleRate, f, w, gm);
            b.side.eq.setPeaking (sampleRate, f, w, gs);
            b.dirty = false;

            if (! (s > 0.0 || b.satAmount.isSmoothing()))
            {
                b.satActive = false;
                b.mid.weight = b.side.weight = 0.0;
                b.mid.resetSaturation();
                b.side.resetSaturation();
                return;
            }

            b.satActive = true;
            b.mid.bp.setBandPass (sampleRate, f, w);
            b.side.bp.copyCoefficientsFrom (b.mid.bp);

            const double wet   = sat::bandWet (s);
            const double drive = sat::bandDriveLinear (b.character, s);

            prepareSaturation (b.mid,  drive, wet * b.wMid .getCurrentValue(), f, w, std::abs (gm), b.character);
            prepareSaturation (b.side, drive, wet * b.wSide.getCurrentValue(), f, w, std::abs (gs), b.character);
        }

        /** Campana (lineare) + saturazione di banda. La saturazione prende dal segnale uscito dalla campana
            la fetta di spettro della banda (passa-banda con la stessa larghezza), la deforma e aggiunge SOLO
            il residuo non lineare: a livelli bassi il residuo e' nullo e resta la sola campana. Il segnale che
            esce arriva alla banda successiva, che quindi tratta anche le armoniche create da questa. */
        inline double runBandChannel (Band& b, BandChannel& c, double x) noexcept
        {
            double y = c.eq.process (x);

            if (b.satActive && c.weight > 0.0)
            {
                const double u   = c.bp.process (y);
                const double res = c.adaa.process (c.shaper, c.drive * u, antiAlias) / c.drive;
                y += c.weight * c.dc.process (res);
            }

            return y;
        }

        //----------------------------------------------------------------------
        //  Passa-alto
        void updateHighPass (HighPass& h) noexcept
        {
            const double f = h.freq.getCurrentValue();
            const Biquad c = h.secondOrder ? makeBand3HighPass (sampleRate, f) : makeBand1HighPass (sampleRate, f);

            h.mid.copyCoefficientsFrom (c);
            h.side.copyCoefficientsFrom (c);

            h.active = h.mix.getCurrentValue() > 0.0 || h.mix.isSmoothing();
            if (! h.active)
            {
                h.mid.reset();
                h.side.reset();
            }
        }

        inline void runHighPass (HighPass& h, double& m, double& s) noexcept
        {
            if (! h.active)
                return;

            const double mix = h.mix.getCurrentValue();
            m += mix * (h.mid .process (m) - m);
            s += mix * (h.side.process (s) - s);
        }

        //----------------------------------------------------------------------
        double sampleRate = 44100.0;
        bool antiAlias = true;

        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> inGainDb  { 0.0 };
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> outGainDb { 0.0 };

        Ramp preGain, postGain, outGain;
        bool   valveOn = false;
        double valveKappa = 0.0;

        sat::Shaper valveShaper, trafoShaper;
        std::array<ValveChannel, 2> valve;
        std::array<TrafoChannel, 2> trafo;
        std::array<HighPass, 2> highPass;
        std::array<Band, (size_t) numBands> bands;
    };
}
