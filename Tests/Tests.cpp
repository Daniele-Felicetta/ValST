//==============================================================================
//  Test di ValST MS EQ
//
//  Versione 1.0 (EQ Mid/Side):
//    1. matematica del mapping Mid/Side e del fader logaritmico
//    2. DSP: guadagni su Mid / Side, accordo tra audio e grafico, assenza di click
//    3. stato (salvataggio / ripristino)
//    4. il VST3 compilato, caricato come farebbe un DAW (se si passa --vst3 <percorso>)
//    5. GUI: trascinamento dei nodi, rotella, snapshot PNG (se si passa --out <cartella>)
//
//  Versione 1.1 (saturazione), una sezione per fase di sviluppo:
//    F1  valvola in ingresso            F5  trasformatore in uscita
//    F2  saturazione di una banda       F6  filtri passa-alto
//    F3  le tre bande                   F7  verifica complessiva
//    F4  interazione tra le bande
//  Dopo ogni fase si rieseguono TUTTI i test precedenti.
//==============================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <functional>
#include <new>
#include <random>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MsEqDsp.h"
#include "Saturation.h"

using namespace valst;

//==============================================================================
//  Contatore delle allocazioni: serve a dimostrare che processBlock() non alloca memoria.
//  Si arma solo attorno alle chiamate audio (vedi testPhase7Integration).
//==============================================================================
namespace allocationProbe
{
    thread_local bool armed = false;               // vale solo per il thread che esegue processBlock: altri thread (JUCE) non contano
    std::atomic<long> count { 0 };
    std::atomic<std::size_t> lastSize { 0 };
}

void* operator new (std::size_t size)
{
    if (allocationProbe::armed)
    {
        allocationProbe::count.fetch_add (1, std::memory_order_relaxed);
        allocationProbe::lastSize.store (size, std::memory_order_relaxed);
    }

    if (void* p = std::malloc (size != 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)                   { return operator new (size); }
void  operator delete (void* p) noexcept                  { std::free (p); }
void  operator delete (void* p, std::size_t) noexcept     { std::free (p); }
void  operator delete[] (void* p) noexcept                { std::free (p); }
void  operator delete[] (void* p, std::size_t) noexcept   { std::free (p); }

namespace
{
    int checks = 0, failures = 0;

    void check (bool ok, const juce::String& what)
    {
        ++checks;
        if (! ok)
            ++failures;
        std::cout << (ok ? "  [ ok ] " : "  [FAIL] ") << what << std::endl;
    }

    void checkNear (double actual, double expected, double tolerance, const juce::String& what)
    {
        check (std::abs (actual - expected) <= tolerance,
               what + "  (atteso " + juce::String (expected, 3) + ", ottenuto " + juce::String (actual, 3) + ")");
    }

    void section (const juce::String& name) { std::cout << "\n== " << name << " ==" << std::endl; }

    //--------------------------------------------------------------------------
    void setReal (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (ranged->paramID == id)
                {
                    ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
                    return;
                }

        jassertfalse;
    }

    void setBand (juce::AudioProcessor& p, int band, float freq, float width, float gain, float ms)
    {
        setReal (p, paramId (band, kFreq),  freq);
        setReal (p, paramId (band, kWidth), width);
        setReal (p, paramId (band, kGain),  gain);
        setReal (p, paramId (band, kMs),    ms);
    }

    void flatten (juce::AudioProcessor& p)
    {
        static const float defaultFreq[numBands] = { 100.0f, 1000.0f, 8000.0f };
        for (int b = 0; b < numBands; ++b)
            setBand (p, b, defaultFreq[b], 1.0f, 0.0f, 0.0f);
    }

    /** Guadagno (dB) a una data frequenza per un segnale puro Mid (L=R) o puro Side (L=-R).
        I parametri vanno impostati prima; con prepare=true i valori vengono applicati senza rampa. */
    double measureGainDb (juce::AudioProcessor& p, double freq, bool sidePath, double sampleRate = 48000.0,
                          int blockSize = 512, bool prepare = true)
    {
        if (prepare)
        {
            p.setRateAndBufferSizeDetails (sampleRate, blockSize);
            p.prepareToPlay (sampleRate, blockSize);
        }

        const int total   = (int) (sampleRate * 2.0);
        const int analyze = 16384;
        const double amplitude = 0.25;

        std::vector<float> out ((size_t) total, 0.0f);
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int n = 0; n < total; n += blockSize)
        {
            const int num = juce::jmin (blockSize, total - n);

            for (int i = 0; i < num; ++i)
            {
                const float x = (float) (amplitude * std::sin (juce::MathConstants<double>::twoPi * freq * (double) (n + i) / sampleRate));
                buffer.setSample (0, i, x);
                buffer.setSample (1, i, sidePath ? -x : x);
            }

            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, num);
            p.processBlock (view, midi);

            for (int i = 0; i < num; ++i)
                out[(size_t) (n + i)] = buffer.getSample (0, i);
        }

        // analisi: DFT a bin singolo con finestra di Hann sugli ultimi "analyze" campioni
        // (la finestra evita l'errore di dispersione quando nel segmento ci sono pochi periodi)
        const int start = total - analyze;
        double re = 0.0, im = 0.0, windowSum = 0.0;
        for (int i = 0; i < analyze; ++i)
        {
            const double hann = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) (analyze - 1));
            const double w    = juce::MathConstants<double>::twoPi * freq * (double) (start + i) / sampleRate;
            const double x    = (double) out[(size_t) (start + i)];
            re += hann * x * std::cos (w);
            im += hann * x * std::sin (w);
            windowSum += hann;
        }

        const double amp = 2.0 * std::hypot (re, im) / windowSum;
        return 20.0 * std::log10 (juce::jmax (1.0e-9, amp / amplitude));
    }

    //==========================================================================
    //  Strumenti di misura per la saturazione
    //==========================================================================
    using Signal = std::vector<float>;

    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    void preparePlayback (juce::AudioProcessor& p, double fs, int block)
    {
        p.setRateAndBufferSizeDetails (fs, block);
        p.prepareToPlay (fs, block);
    }

    /** Elabora un segnale stereo con il processore. I parametri vanno impostati prima: con prepare = true
        vengono applicati senza rampa (come all'apertura del progetto). */
    void renderStereo (juce::AudioProcessor& p, const Signal& inL, const Signal& inR, Signal& outL, Signal& outR,
                       double fs = 48000.0, int block = 512, bool prepare = true)
    {
        if (prepare)
            preparePlayback (p, fs, block);

        const int total = (int) inL.size();
        outL.assign ((size_t) total, 0.0f);
        outR.assign ((size_t) total, 0.0f);

        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;

        for (int n = 0; n < total; n += block)
        {
            const int num = juce::jmin (block, total - n);

            for (int i = 0; i < num; ++i)
            {
                buffer.setSample (0, i, inL[(size_t) (n + i)]);
                buffer.setSample (1, i, inR[(size_t) (n + i)]);
            }

            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, num);
            p.processBlock (view, midi);

            for (int i = 0; i < num; ++i)
            {
                outL[(size_t) (n + i)] = buffer.getSample (0, i);
                outR[(size_t) (n + i)] = buffer.getSample (1, i);
            }
        }
    }

    Signal makeSine (double freq, double amplitude, int total, double fs, double phase = 0.0)
    {
        Signal s ((size_t) total);
        for (int i = 0; i < total; ++i)
            s[(size_t) i] = (float) (amplitude * std::sin (twoPi * freq * (double) i / fs + phase));
        return s;
    }

    /** Segnale Mid puro (L = R): restituisce il canale sinistro. */
    Signal renderMid (juce::AudioProcessor& p, const Signal& in, double fs = 48000.0, int block = 512)
    {
        Signal l, r;
        renderStereo (p, in, in, l, r, fs, block);
        return l;
    }

    /** Segnale Side puro (L = -R): restituisce il canale sinistro. */
    Signal renderSide (juce::AudioProcessor& p, const Signal& in, double fs = 48000.0, int block = 512)
    {
        Signal neg (in.size());
        for (size_t i = 0; i < in.size(); ++i)
            neg[i] = -in[i];

        Signal l, r;
        renderStereo (p, in, neg, l, r, fs, block);
        return l;
    }

    /** Layout mono (1 ingresso / 1 uscita): come lo vedrebbe un DAW su una traccia mono. */
    Signal renderMono (juce::AudioProcessor& p, const Signal& in, double fs = 48000.0, int block = 512)
    {
        p.setPlayConfigDetails (1, 1, fs, block);
        p.prepareToPlay (fs, block);

        Signal out (in.size(), 0.0f);
        juce::AudioBuffer<float> buffer (1, block);
        juce::MidiBuffer midi;

        for (int n = 0; n < (int) in.size(); n += block)
        {
            const int num = juce::jmin (block, (int) in.size() - n);

            for (int i = 0; i < num; ++i)
                buffer.setSample (0, i, in[(size_t) (n + i)]);

            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 1, num);
            p.processBlock (view, midi);

            for (int i = 0; i < num; ++i)
                out[(size_t) (n + i)] = buffer.getSample (0, i);
        }

        return out;
    }

    double dbOf (double ratio)    { return 20.0 * std::log10 (juce::jmax (1.0e-12, ratio)); }
    double linOf (double db)      { return std::pow (10.0, db / 20.0); }

    double blackmanHarris (int i, int n)
    {
        const double x = twoPi * (double) i / (double) (n - 1);
        return 0.35875 - 0.48829 * std::cos (x) + 0.14128 * std::cos (2.0 * x) - 0.01168 * std::cos (3.0 * x);
    }

    /** Ampiezza (di picco) della componente a "f" Hz in x[start, start+len), finestra Blackman-Harris (lobi a -92 dB). */
    double toneAmplitude (const Signal& x, int start, int len, double f, double fs)
    {
        double re = 0.0, im = 0.0, wsum = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double w = blackmanHarris (i, len);
            const double a = twoPi * f * (double) (start + i) / fs;
            const double v = (double) x[(size_t) (start + i)];
            re += w * v * std::cos (a);
            im += w * v * std::sin (a);
            wsum += w;
        }
        return 2.0 * std::hypot (re, im) / wsum;
    }

    struct Harmonics
    {
        std::vector<double> amp;     // amp[0] = fondamentale, amp[k-1] = k-esima armonica
        double thdPercent = 0.0;

        double db (int k) const { return k <= (int) amp.size() ? dbOf (amp[(size_t) (k - 1)] / amp[0]) : -300.0; }
    };

    Harmonics analyseHarmonics (const Signal& x, double f0, double fs, int numHarmonics = 12, int len = 32768)
    {
        Harmonics h;
        const int start = (int) x.size() - len;

        double sumSq = 0.0;
        for (int k = 1; k <= numHarmonics && (double) k * f0 < 0.49 * fs; ++k)
        {
            const double a = toneAmplitude (x, start, len, (double) k * f0, fs);
            h.amp.push_back (a);
            if (k > 1)
                sumSq += a * a;
        }

        h.thdPercent = 100.0 * std::sqrt (sumSq) / juce::jmax (1.0e-12, h.amp[0]);
        return h;
    }

    double meanOf (const Signal& x, int start, int len)
    {
        double s = 0.0;
        for (int i = 0; i < len; ++i)
            s += (double) x[(size_t) (start + i)];
        return s / (double) len;
    }

    double peakOf (const Signal& x, int start, int len)
    {
        double m = 0.0;
        for (int i = 0; i < len; ++i)
            m = juce::jmax (m, (double) std::abs (x[(size_t) (start + i)]));
        return m;
    }

    double rmsOf (const Signal& x, int start, int len)
    {
        double s = 0.0;
        for (int i = 0; i < len; ++i)
            s += (double) x[(size_t) (start + i)] * (double) x[(size_t) (start + i)];
        return std::sqrt (s / (double) len);
    }

    void fft (std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (a[i], a[j]);
        }

        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = -twoPi / (double) len;
            const std::complex<double> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    /** Potenza fuori dalle armoniche vere (k * f0, k <= kmax) rispetto alla fondamentale, in dB.
        Con una fondamentale non commensurabile con fs, l'aliasing cade lontano dalle armoniche vere. */
    double inharmonicDb (const Signal& x, double f0, double fs, int kmax, int len = 65536)
    {
        const int start = (int) x.size() - len;
        std::vector<std::complex<double>> a ((size_t) len);
        for (int i = 0; i < len; ++i)
            a[(size_t) i] = blackmanHarris (i, len) * (double) x[(size_t) (start + i)];
        fft (a);

        const double binHz = fs / (double) len;
        std::vector<char> masked ((size_t) len / 2 + 1, 0);
        auto maskAround = [&] (double f, int halfWidth)
        {
            const int centre = (int) std::lround (f / binHz);
            for (int b = centre - halfWidth; b <= centre + halfWidth; ++b)
                if (b >= 0 && b < (int) masked.size())
                    masked[(size_t) b] = 1;
        };

        maskAround (0.0, 12);
        for (int k = 1; k <= kmax; ++k)
            maskAround ((double) k * f0, 12);

        double fundamental = 0.0, rest = 0.0;
        const int fb = (int) std::lround (f0 / binHz);
        for (int b = 0; b < (int) masked.size(); ++b)
        {
            const double p = std::norm (a[(size_t) b]);
            if (std::abs (b - fb) <= 8)
                fundamental += p;
            else if (! masked[(size_t) b])
                rest += p;
        }

        return 10.0 * std::log10 (juce::jmax (1.0e-30, rest) / juce::jmax (1.0e-30, fundamental));
    }

    //--------------------------------------------------------------------------
    void setInGain (juce::AudioProcessor& p, float db)      { setReal (p, globalId (kInGain), db); }
    void setOutGain (juce::AudioProcessor& p, float db)     { setReal (p, globalId (kOutGain), db); }
    void setSat (juce::AudioProcessor& p, int band, float percent) { setReal (p, paramId (band, kSat), percent); }
    void setHpf (juce::AudioProcessor& p, int which, float hz)     { setReal (p, globalId (which == 0 ? kHpf1 : kHpf3), hz); }

    /** Accesso (solo test) ai due metodi statici protetti di AudioProcessor che convertono XML <-> blocco binario. */
    struct XmlBlob : juce::AudioProcessor
    {
        using juce::AudioProcessor::getXmlFromBinary;
        using juce::AudioProcessor::copyXmlToBinary;
    };

    //==========================================================================
    //  FASE 1: gain di ingresso con saturazione valvolare
    //==========================================================================
    void testPhase1Valve()
    {
        section ("F1  Ingresso: gain + saturazione valvolare");

        const double fs = 48000.0;
        const double f0 = fs * 211.0 / 32768.0;                 // ~309 Hz, periodi interi nella finestra di analisi
        const int total = 65536;

        auto run = [&] (float gainDb, double amplitude)
        {
            ValstMsEqProcessor p;
            setInGain (p, gainDb);
            return renderMid (p, makeSine (f0, amplitude, total, fs), fs);
        };

        // --- 1. gain <= 0: gain puro, perfettamente lineare
        {
            const auto y = run (-12.0f, 0.5);
            const auto h = analyseHarmonics (y, f0, fs);
            checkNear (dbOf (h.amp[0] / 0.5), -12.0, 0.01, "gain -12 dB: attenuazione esatta (nessuna saturazione sotto 0 dB)");
            check (h.thdPercent < 1.0e-4, "gain -12 dB: nessuna distorsione (THD " + juce::String (h.thdPercent, 6) + " %)");
        }

        // --- 2. gain positivo: il livello sale della meta' dei dB (compensazione parziale, dopo la non linearita')
        for (float g : { 6.0f, 12.0f, 24.0f })
        {
            const auto y = run (g, 0.003);                       // -50 dBFS: curva ancora lineare
            const auto h = analyseHarmonics (y, f0, fs);
            checkNear (dbOf (h.amp[0] / 0.003), 0.5 * g, 0.2, "gain +" + juce::String (g, 0) + " dB a livello basso: il livello sale di soli +"
                                                                  + juce::String (0.5 * g, 1) + " dB (compensazione parziale)");
        }

        // --- 3. saturazione progressiva col gain (sinusoide a -18 dB RMS)
        {
            const double amp = linOf (-18.0) * std::sqrt (2.0);
            double previous = 0.0;
            bool monotonic = true;
            std::cout << "     THD valvola @ -18 dB RMS:";
            double thd6 = 0.0, thd12 = 0.0, thd24 = 0.0;

            for (float g : { 1.0f, 3.0f, 6.0f, 9.0f, 12.0f, 18.0f, 24.0f })
            {
                const auto h = analyseHarmonics (run (g, amp), f0, fs);
                std::cout << "  +" << (int) g << " dB: " << juce::String (h.thdPercent, 3) << " %";
                monotonic = monotonic && h.thdPercent > previous;
                previous = h.thdPercent;
                if (g == 6.0f) thd6 = h.thdPercent;
                if (g == 12.0f) thd12 = h.thdPercent;
                if (g == 24.0f) thd24 = h.thdPercent;
            }
            std::cout << std::endl;

            check (monotonic, "la distorsione cresce in modo monotono col gain (transizione graduale lineare -> non lineare)");
            check (thd6 < 0.3, "+6 dB: saturazione molto leggera (" + juce::String (thd6, 3) + " %)");
            check (thd12 > 0.5 && thd12 < 3.0, "+12 dB: saturazione media (" + juce::String (thd12, 2) + " %)");
            check (thd24 > 8.0, "+24 dB: saturazione pronunciata (" + juce::String (thd24, 1) + " %)");
        }

        // --- 4. armoniche pari E dispari, spettro morbido
        {
            const auto h = analyseHarmonics (run (12.0f, linOf (-18.0) * std::sqrt (2.0)), f0, fs);
            const double ratio = h.amp[1] / h.amp[2];
            std::cout << "     +12 dB: H2 " << juce::String (h.db (2), 1) << " dBc, H3 " << juce::String (h.db (3), 1)
                      << " dBc, H4 " << juce::String (h.db (4), 1) << " dBc, H5 " << juce::String (h.db (5), 1)
                      << " dBc, H9 " << juce::String (h.db (9), 1) << " dBc" << std::endl;
            check (h.db (2) > -60.0 && h.db (3) > -60.0, "armoniche PARI (2a) e DISPARI (3a) entrambe presenti");
            check (ratio > 0.3 && ratio < 3.0, "pari e dispari bilanciati: H2/H3 = " + juce::String (ratio, 2));
            check (h.db (9) < -60.0, "spettro morbido: la 9a armonica e' a " + juce::String (h.db (9), 1) + " dBc");
        }

        // --- 5. le armoniche dipendono dal livello del segnale
        {
            double previous = 0.0;
            bool monotonic = true;
            double thdLow = 0.0;
            for (double level : { -40.0, -30.0, -20.0, -10.0 })
            {
                const auto h = analyseHarmonics (run (12.0f, linOf (level)), f0, fs);
                monotonic = monotonic && h.thdPercent > previous;
                previous = h.thdPercent;
                if (level == -40.0) thdLow = h.thdPercent;
            }
            check (monotonic, "a gain fisso, piu' il segnale e' forte piu' armoniche (generazione progressiva)");
            check (thdLow < 0.05, "a -40 dBFS la valvola e' quasi lineare (THD " + juce::String (thdLow, 4) + " %)");
        }

        // --- 6. compressione progressiva dei picchi
        {
            const double smallSignal = analyseHarmonics (run (18.0f, 0.003), f0, fs).amp[0] / 0.003;
            std::cout << "     compressione a +18 dB:";
            double previous = 1.0e9;
            bool monotonic = true;
            double at6 = 0.0;
            for (double level : { -40.0, -30.0, -20.0, -12.0, -6.0, 0.0 })
            {
                const double a = linOf (level);
                const double g = analyseHarmonics (run (18.0f, a), f0, fs).amp[0] / a;
                std::cout << "  " << (int) level << " dBFS: " << juce::String (dbOf (g / smallSignal), 1) << " dB";
                monotonic = monotonic && g < previous;
                previous = g;
                if (level == -6.0) at6 = dbOf (g / smallSignal);
            }
            std::cout << std::endl;
            check (monotonic, "all'aumentare del livello il guadagno diminuisce in modo continuo (compressione progressiva)");
            check (at6 < -3.0, "a -6 dBFS il picco e' gia' compresso di " + juce::String (-at6, 1) + " dB");
        }

        // --- 7. nessun clipping netto, nessun DC
        {
            const auto y = run (24.0f, 1.0);                     // massima spinta, segnale a fondo scala
            const double peak = peakOf (y, 0, total);
            check (peak < 0.4, "a +24 dB con segnale a 0 dBFS l'uscita resta limitata (picco " + juce::String (peak, 3) + ")");

            // nessun clipping brusco: la curva della valvola e' liscia (la derivata non ha salti)
            // Un clipper netto avrebbe un salto di derivata pari a 1.0 nel punto di intervento.
            {
                const auto shaper = sat::makeValveShaper();
                const double h = 1.0e-3;
                double maxSlopeChange = 0.0;
                for (double s = -8.0; s < 8.0; s += h)
                {
                    const double d0 = (shaper.f (s + h) - shaper.f (s)) / h;
                    const double d1 = (shaper.f (s + 2.0 * h) - shaper.f (s + h)) / h;
                    maxSlopeChange = juce::jmax (maxSlopeChange, std::abs (d1 - d0));
                }
                check (maxSlopeChange < 0.002, "curva valvolare continua e liscia: variazione massima della pendenza "
                                                   + juce::String (maxSlopeChange, 5) + " per passo (un clipper netto: 1.0)");
            }

            const double dc = meanOf (y, total - 32768, 32768);
            check (std::abs (dc) < 1.0e-3, "il DC generato dall'asimmetria viene eliminato (media " + juce::String (dc, 6) + ")");

            const auto y2 = run (12.0f, linOf (-12.0));
            check (std::abs (meanOf (y2, total - 32768, 32768)) < 1.0e-4, "nessun DC anche a +12 dB");
        }

        // --- 8. anti-aliasing (ADAA): meno aliasing con la stessa saturazione, senza sovracampionamento
        {
            const double fsLow = 44100.0;
            const double f = 3010.3;
            const int n = 131072;
            const auto in = makeSine (f, linOf (-6.0), n, fsLow);
            Signal outOn (in), outOff (in), dummy (in);

            auto runEngine = [&] (bool adaa, Signal& out)
            {
                MsEqEngine e;
                e.prepare (fsLow);
                e.setInput (24.0f);
                e.setAntiAliasing (adaa);
                e.snapToTargets();
                out = in;
                dummy = in;
                e.process (out.data(), dummy.data(), n);
            };
            runEngine (true, outOn);
            runEngine (false, outOff);

            const int kmax = (int) (0.5 * fsLow / f);
            const double on = inharmonicDb (outOn, f, fsLow, kmax), off = inharmonicDb (outOff, f, fsLow, kmax);
            std::cout << "     aliasing (potenza inarmonica / fondamentale) a 3 kHz, 44.1 kHz, +24 dB: senza ADAA "
                      << juce::String (off, 1) << " dB, con ADAA " << juce::String (on, 1) << " dB" << std::endl;
            check (off - on > 6.0, "l'ADAA riduce l'aliasing di " + juce::String (off - on, 1) + " dB (senza sovracampionamento)");
        }

        // --- 9. automazione del gain mentre l'audio scorre
        {
            ValstMsEqProcessor p;
            preparePlayback (p, fs, 256);
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            double last = 0.0, maxStep = 0.0;
            bool finite = true;

            for (int n = 0; n < (int) fs * 3; n += 256)
            {
                // il gain di ingresso scorre da -12 a +24 dB in modo continuo, con salti improvvisi a 1 s e 2 s
                const double t = (double) n / fs;
                setInGain (p, t < 1.0 ? (float) (-12.0 + 36.0 * t) : (t < 2.0 ? -12.0f : 24.0f));

                for (int i = 0; i < 256; ++i)
                {
                    const float x = (float) (0.1 * std::sin (twoPi * 440.0 * (double) (n + i) / fs));
                    buffer.setSample (0, i, x);
                    buffer.setSample (1, i, x);
                }
                p.processBlock (buffer, midi);

                for (int i = 0; i < 256; ++i)
                {
                    const double y = buffer.getSample (0, i);
                    finite = finite && std::isfinite (y);
                    if (n > 0 || i > 0)
                        maxStep = juce::jmax (maxStep, std::abs (y - last));
                    last = y;
                }
            }
            check (finite, "automazione del gain: uscita sempre finita");
            check (maxStep < 0.12, "automazione del gain: nessun click (salto massimo " + juce::String (maxStep, 4) + ")");
        }

        // --- 10. canali: L e R sono indipendenti, mono = Mid
        {
            ValstMsEqProcessor p;
            setInGain (p, 18.0f);
            const auto in = makeSine (f0, 0.2, 16384, fs);
            const Signal silent (in.size(), 0.0f);
            Signal ol, orr;
            renderStereo (p, in, silent, ol, orr, fs);
            check (peakOf (orr, 0, (int) orr.size()) < 1.0e-9, "segnale solo a sinistra: il canale destro resta esattamente silenzioso");
            check (peakOf (ol, 0, (int) ol.size()) > 0.1, "segnale solo a sinistra: il canale sinistro passa");

            ValstMsEqProcessor stereo, mono;
            setInGain (stereo, 18.0f);
            setInGain (mono, 18.0f);
            const auto ref = renderMid (stereo, in, fs);

            mono.setPlayConfigDetails (1, 1, fs, 512);
            mono.prepareToPlay (fs, 512);
            check (mono.getTotalNumInputChannels() == 1 && mono.getTotalNumOutputChannels() == 1, "il plugin accetta anche il layout mono");

            juce::AudioBuffer<float> buffer (1, 512);
            juce::MidiBuffer midi;
            double maxDiff = 0.0;
            for (int n = 0; n < (int) in.size(); n += 512)
            {
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (0, i, in[(size_t) (n + i)]);
                mono.processBlock (buffer, midi);
                for (int i = 0; i < 512; ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (buffer.getSample (0, i) - ref[(size_t) (n + i)]));
            }
            check (maxDiff < 1.0e-7, "mono: stesso risultato del canale Mid stereo (diff max " + juce::String (maxDiff, 9) + ")");
        }

        // --- 11. silenzio dopo un segnale forte: nessun denormal / NaN, uscita a zero
        {
            ValstMsEqProcessor p;
            setInGain (p, 24.0f);
            preparePlayback (p, fs, 512);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            bool finite = true;
            double tail = 0.0;

            for (int n = 0; n < (int) fs * 14; n += 512)
            {
                const bool loud = n < (int) fs;
                for (int i = 0; i < 512; ++i)
                {
                    const float x = loud ? (float) (0.9 * std::sin (twoPi * 200.0 * (double) (n + i) / fs)) : 0.0f;
                    buffer.setSample (0, i, x);
                    buffer.setSample (1, i, x);
                }
                p.processBlock (buffer, midi);
                for (int i = 0; i < 512; ++i)
                    finite = finite && std::isfinite (buffer.getSample (0, i)) && std::isfinite (buffer.getSample (1, i));
                if (n > (int) fs * 13)
                    tail = juce::jmax (tail, (double) buffer.getMagnitude (0, 512));
            }
            check (finite && tail == 0.0, "dopo 13 s di silenzio l'uscita e' esattamente 0 (nessun denormal residuo)");
        }

        // --- 12. NaN / inf in ingresso non contaminano il filtro
        {
            ValstMsEqProcessor p;
            setInGain (p, 12.0f);
            auto in = makeSine (f0, 0.1, 8192, fs);
            in[1000] = std::numeric_limits<float>::quiet_NaN();
            in[2000] = std::numeric_limits<float>::infinity();
            const auto y = renderMid (p, in, fs);
            bool finite = true;
            for (float v : y)
                finite = finite && std::isfinite (v);
            check (finite, "NaN e infinito in ingresso non producono NaN in uscita");
            check (peakOf (y, 4000, 4000) > 0.05, "dopo un NaN il segnale riprende normalmente");
        }

        // --- 13. risultato indipendente dalla dimensione del blocco (valvola attiva)
        {
            std::mt19937 rng (7);
            std::uniform_real_distribution<float> dist (-0.3f, 0.3f);
            Signal in (9600);
            for (auto& v : in) v = dist (rng);

            auto runBlock = [&] (int block)
            {
                ValstMsEqProcessor p;
                setInGain (p, 15.0f);
                return renderMid (p, in, fs, block);
            };

            const auto ref = runBlock (512);
            for (int block : { 1, 7, 64, 480, 4096 })
            {
                const auto other = runBlock (block);
                double maxDiff = 0.0;
                for (size_t i = 0; i < ref.size(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (ref[i] - other[i]));
                check (maxDiff < 1.0e-6, "valvola attiva, blocchi da " + juce::String (block) + ": identico (diff max " + juce::String (maxDiff, 9) + ")");
            }
        }
    }

    //==========================================================================
    //  FASE 2: saturazione di una banda (banda 1)
    //==========================================================================
    void configureBand (juce::AudioProcessor& p, int band, float freq, float width, float gain, float ms, float satPercent)
    {
        setBand (p, band, freq, width, gain, ms);
        setSat (p, band, satPercent);
    }

    /** Riferimento esterno: la sola campana lineare di una banda, per un segnale Mid (L = R) o Side (L = -R). */
    Signal referenceBell (const Signal& in, double fs, float freq, float width, float gain, float ms, bool sidePath = false)
    {
        const auto g = computeBandGains (gain, ms);
        Biquad bell;
        bell.setPeaking (fs, freq, width, sidePath ? g.sideDb : g.midDb);

        Signal out (in.size());
        for (size_t i = 0; i < in.size(); ++i)
            out[i] = (float) bell.process ((double) in[i]);
        return out;
    }

    /** Quanto "decade piano" lo spettro: sqrt(H7^2 + H9^2 + H11^2) / H3.
        Alto = armoniche alte ancora forti (aggressivo, tipo onda quadra: ~1/n); basso = spettro che decade in
        fretta (morbido, progressivo: ~1/n^2 o piu' ripido). E' la differenza tra gli estremi della famiglia di curve. */
    double highRatio (const Harmonics& h)
    {
        return std::sqrt (h.amp[6] * h.amp[6] + h.amp[8] * h.amp[8] + h.amp[10] * h.amp[10]) / juce::jmax (1.0e-12, h.amp[2]);
    }

    struct ToneSetup
    {
        float bandFreq, width = 1.0f, gain = 0.0f, ms = 0.0f, sat = 0.0f;
        double tone, amp = 0.1, rate = 48000.0;
        bool side = false;
        int band = 0;
    };

    Signal runTone (const ToneSetup& s, int total = 65536)
    {
        ValstMsEqProcessor p;
        configureBand (p, s.band, s.bandFreq, s.width, s.gain, s.ms, s.sat);
        const auto in = makeSine (s.tone, s.amp, total, s.rate);
        return s.side ? renderSide (p, in, s.rate) : renderMid (p, in, s.rate);
    }

    void testPhase2BandSaturation()
    {
        section ("F2  Saturazione di una banda (banda 1)");

        const double fs = 48000.0;
        const double f0 = fs * 683.0 / 32768.0;                 // ~1000.4 Hz: periodi interi nella finestra di analisi
        const int total = 65536;

        auto harmonicsOf = [&] (const ToneSetup& s) { return analyseHarmonics (runTone (s, total), s.tone, s.rate, 12); };
        ToneSetup base { (float) f0 };
        base.tone = f0;

        // --- 1. SAT = 0: la banda e' esattamente la campana lineare (nessuna differenza dalla 1.0)
        {
            std::mt19937 rng (5);
            std::uniform_real_distribution<float> dist (-0.2f, 0.2f);
            Signal inL (20000), inR (20000);
            for (size_t i = 0; i < inL.size(); ++i) { inL[i] = dist (rng); inR[i] = dist (rng); }

            ValstMsEqProcessor p;
            configureBand (p, 0, 1000.0f, 1.3f, 7.0f, -30.0f, 0.0f);
            Signal outL, outR;
            renderStereo (p, inL, inR, outL, outR, fs);

            const auto gains = computeBandGains (7.0, -30.0);
            Biquad mid, side;
            mid.setPeaking (fs, 1000.0, 1.3, gains.midDb);
            side.setPeaking (fs, 1000.0, 1.3, gains.sideDb);

            double maxDiff = 0.0;
            for (size_t i = 0; i < inL.size(); ++i)
            {
                const double m = mid.process (0.5 * ((double) inL[i] + (double) inR[i]));
                const double s = side.process (0.5 * ((double) inL[i] - (double) inR[i]));
                maxDiff = juce::jmax (maxDiff, std::abs (m + s - (double) outL[i]), std::abs (m - s - (double) outR[i]));
            }
            check (maxDiff < 1.0e-6, "SAT 0 %: l'uscita coincide con la campana lineare calcolata a parte (diff max " + juce::String (maxDiff, 9) + ")");
        }

        // --- 2. a livelli bassi anche SAT 100 % e' trasparente (il residuo e' ~0)
        {
            ToneSetup s = base;
            s.sat = 100.0f; s.amp = linOf (-70.0); s.gain = 6.0f;
            const auto y = runTone (s);
            const auto ref = referenceBell (makeSine (f0, s.amp, total, fs), fs, (float) f0, 1.0f, 6.0f, 0.0f);
            double maxDiff = 0.0;
            for (size_t i = 0; i < y.size(); ++i)
                maxDiff = juce::jmax (maxDiff, (double) std::abs (y[i] - ref[i]));
            check (maxDiff < 2.0e-7, "SAT 100 % a -70 dBFS: nessun effetto, solo la campana (diff max " + juce::String (maxDiff, 10) + ")");
        }

        // --- 3. la saturazione cresce con SAT (tono al centro della banda, -20 dBFS)
        {
            double previous = 0.0;
            bool monotonic = true;
            std::cout << "     THD banda 1 @ -20 dBFS, tono al centro:";
            double thd25 = 0.0, thd100 = 0.0;
            for (float sat : { 5.0f, 10.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            {
                ToneSetup s = base; s.sat = sat;
                const auto h = harmonicsOf (s);
                std::cout << "  " << (int) sat << " %: " << juce::String (h.thdPercent, 3) << " %";
                monotonic = monotonic && h.thdPercent > previous;
                previous = h.thdPercent;
                if (sat == 25.0f) thd25 = h.thdPercent;
                if (sat == 100.0f) thd100 = h.thdPercent;
            }
            std::cout << std::endl;
            check (monotonic, "la distorsione cresce in modo monotono con SAT");
            check (thd25 > 0.05 && thd25 < 5.0, "SAT 25 %: colorazione delicata (" + juce::String (thd25, 2) + " %)");
            check (thd100 > 8.0, "SAT 100 %: saturazione pronunciata (" + juce::String (thd100, 1) + " %)");
        }

        // --- 4. le armoniche dipendono dal livello (generazione progressiva)
        {
            double previous = 0.0;
            bool monotonic = true;
            double lowest = 0.0;
            for (double level : { -50.0, -40.0, -30.0, -20.0, -10.0 })
            {
                ToneSetup s = base; s.sat = 50.0f; s.amp = linOf (level);
                const auto h = harmonicsOf (s);
                monotonic = monotonic && h.thdPercent > previous;
                previous = h.thdPercent;
                if (level == -50.0) lowest = h.thdPercent;
            }
            check (monotonic, "a SAT fisso piu' il segnale e' forte, piu' armoniche");
            check (lowest < 0.05, "a -50 dBFS la banda e' quasi lineare (THD " + juce::String (lowest, 4) + " %)");
        }

        // --- 5. la saturazione e' selettiva in frequenza: solo la fetta di spettro della banda viene deformata
        {
            ToneSetup centre = base; centre.sat = 50.0f;
            ToneSetup far = centre;
            far.tone = fs * 43.0 / 32768.0;                      // ~63 Hz: 4 ottave sotto la banda
            const double thdCentre = harmonicsOf (centre).thdPercent;
            const double thdFar = harmonicsOf (far).thdPercent;
            check (thdFar < thdCentre / 20.0, "tono 4 ottave sotto la banda: distorsione " + juce::String (thdFar, 4)
                                                  + " % contro " + juce::String (thdCentre, 3) + " % al centro");
        }

        // --- 6. il rapporto pari/dispari dipende dal gain della banda
        {
            // stesso livello DENTRO la curva: il gain della campana viene compensato in ingresso
            ToneSetup lowGain = base;  lowGain.sat = 40.0f; lowGain.gain = 0.0f;   lowGain.amp = 0.05;
            ToneSetup highGain = base; highGain.sat = 40.0f; highGain.gain = 12.0f; highGain.amp = 0.05 / linOf (12.0);
            const auto a = harmonicsOf (lowGain), b = harmonicsOf (highGain);
            const double ra = a.amp[1] / a.amp[2], rb = b.amp[1] / b.amp[2];
            std::cout << "     H2/H3 a +0 dB: " << juce::String (ra, 3) << "   a +12 dB: " << juce::String (rb, 3) << std::endl;
            check (rb > 3.0 * ra, "piu' gain = piu' armoniche pari rispetto alle dispari (H2/H3: " + juce::String (ra, 2) + " -> " + juce::String (rb, 2) + ")");

            // anche un taglio aumenta il contenuto pari (dipende dal modulo del gain)
            ToneSetup cut = base; cut.sat = 40.0f; cut.gain = -12.0f; cut.amp = 0.05 / linOf (-12.0);
            const auto c = harmonicsOf (cut);
            check (c.amp[1] / c.amp[2] > 3.0 * ra, "anche un taglio di -12 dB aumenta le pari (H2/H3 = " + juce::String (c.amp[1] / c.amp[2], 2) + ")");
        }

        // --- 7. Frequency e Bandwidth cambiano la forma della deformazione (catena completa)
        {
            // larghezza: stretta = piu' dura / aggressiva, larga = piu' morbida
            ToneSetup narrow = base; narrow.sat = 100.0f; narrow.width = 0.2f;
            ToneSetup wide = base;   wide.sat = 100.0f;   wide.width = 4.0f;
            const double rn = highRatio (harmonicsOf (narrow)), rw = highRatio (harmonicsOf (wide));
            std::cout << "     armoniche alte / terza (H7,H9,H11 su H3): banda stretta " << juce::String (rn, 3)
                      << ", banda larga " << juce::String (rw, 3) << std::endl;
            check (rn > 1.5 * rw, "banda stretta: spettro piu' piatto (aggressivo) della banda larga (" + juce::String (rn, 2) + " contro " + juce::String (rw, 2) + ")");

            // frequenza (a 96 kHz, cosi' le armoniche non ripiegano): bassa = piu' dura, alta = piu' morbida
            ToneSetup low = base;  low.rate = 96000.0;  low.tone = 96000.0 * 69.0 / 32768.0;     low.bandFreq = (float) low.tone;  low.sat = 100.0f;
            ToneSetup high = base; high.rate = 96000.0; high.tone = 96000.0 * 1024.0 / 32768.0;  high.bandFreq = (float) high.tone; high.sat = 100.0f;
            const double rl = highRatio (harmonicsOf (low)), rh = highRatio (harmonicsOf (high));
            std::cout << "     armoniche alte / terza: banda a " << juce::String (low.tone, 0) << " Hz " << juce::String (rl, 3)
                      << ", banda a " << juce::String (high.tone, 0) << " Hz " << juce::String (rh, 3) << std::endl;
            check (rl > 1.5 * rh, "banda bassa: spettro piu' piatto della banda alta (" + juce::String (rl, 2) + " contro " + juce::String (rh, 2) + ")");
        }

        // --- 8. legge matematica Freq / BW / Gain -> forma della curva (monotona, senza salti)
        {
            const auto character = sat::makeCharacter (sat::defaultSeeds[0]);
            auto shapeOf = [&] (double f, double w, double g)
            {
                sat::Shaper sh;
                double scale = 1.0;
                sat::computeBandShape (character, f, w, g, sh, scale);
                return sh;
            };
            auto hardness = [] (const sat::Shaper& sh) { return 0.5 * (1.0 - sh.wS) + 0.5 * sh.wH; };       // 0 = SINE, 0.5 = TANH, 1 = HARD

            bool freqMono = true, widthMono = true, gainMono = true;
            double last = 2.0;
            for (double f = 20.0; f <= 20000.0; f *= 1.1)
            {
                const double q = hardness (shapeOf (f, 1.0, 0.0));
                freqMono = freqMono && q <= last + 1.0e-12;
                last = q;
            }
            last = 2.0;
            for (double w = 0.1; w <= 6.0; w *= 1.1)
            {
                const double q = hardness (shapeOf (1000.0, w, 0.0));
                widthMono = widthMono && q <= last + 1.0e-12;
                last = q;
            }
            double lastAsym = -1.0;
            for (double g = 0.0; g <= 18.0; g += 0.5)
            {
                const auto sh = shapeOf (1000.0, 1.0, g);
                const double a = std::abs (sh.kp - sh.kn) * 0.5;
                gainMono = gainMono && a >= lastAsym - 1.0e-12;
                lastAsym = a;
            }
            check (freqMono, "la durezza della curva non cresce mai con la frequenza");
            check (widthMono, "la durezza della curva non cresce mai con la larghezza di banda");
            check (gainMono, "l'asimmetria (armoniche pari) cresce con |gain|");
        }

        // --- 9. dove lavora il fader, lavora la saturazione
        {
            ToneSetup s = base; s.sat = 60.0f;
            s.ms = 100.0f; s.side = false;  const double midWhenSide = harmonicsOf (s).thdPercent;
            s.ms = 100.0f; s.side = true;   const double sideWhenSide = harmonicsOf (s).thdPercent;
            s.ms = -100.0f; s.side = false; const double midWhenMid = harmonicsOf (s).thdPercent;
            s.ms = -100.0f; s.side = true;  const double sideWhenMid = harmonicsOf (s).thdPercent;
            s.ms = 0.0f; s.side = false;    const double midCentre = harmonicsOf (s).thdPercent;
            s.ms = 0.0f; s.side = true;     const double sideCentre = harmonicsOf (s).thdPercent;

            check (midWhenSide < 1.0e-3 && sideWhenSide > 1.0, "fader +100 (Side): il Mid resta pulito (" + juce::String (midWhenSide, 5)
                                                                    + " %), il Side satura (" + juce::String (sideWhenSide, 2) + " %)");
            check (sideWhenMid < 1.0e-3 && midWhenMid > 1.0, "fader -100 (Mid): il Side resta pulito (" + juce::String (sideWhenMid, 5)
                                                                   + " %), il Mid satura (" + juce::String (midWhenMid, 2) + " %)");
            check (std::abs (midCentre - sideCentre) < 0.01 * midCentre && midCentre > 1.0,
                   "fader 0: Mid e Side saturano allo stesso modo (" + juce::String (midCentre, 3) + " % e " + juce::String (sideCentre, 3) + " %)");
        }

        // --- 10. nessun DC, anche con la saturazione al massimo
        {
            ToneSetup s = base; s.sat = 100.0f; s.amp = 0.3; s.gain = 6.0f;
            const double dc = meanOf (runTone (s), total - 32768, 32768);
            check (std::abs (dc) < 1.0e-4, "SAT 100 % a livello alto: nessun DC (media " + juce::String (dc, 7) + ")");
        }

        // --- 11. automazione di SAT: nessun click
        {
            ValstMsEqProcessor p;
            configureBand (p, 0, (float) f0, 1.0f, 0.0f, 0.0f, 0.0f);
            preparePlayback (p, fs, 256);
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            double last = 0.0, maxStep = 0.0;
            bool finite = true;

            for (int n = 0; n < (int) fs * 3; n += 256)
            {
                const double t = (double) n / fs;
                // rampa lenta nel 1o secondo, poi accensioni e spegnimenti improvvisi
                float sat = t < 1.0 ? (float) (100.0 * t) : ((int) (t * 4.0) % 2 == 0 ? 100.0f : 0.0f);
                setSat (p, 0, sat);

                for (int i = 0; i < 256; ++i)
                {
                    const float x = (float) (0.15 * std::sin (twoPi * f0 * (double) (n + i) / fs));
                    buffer.setSample (0, i, x);
                    buffer.setSample (1, i, x);
                }
                p.processBlock (buffer, midi);

                for (int i = 0; i < 256; ++i)
                {
                    const double y = buffer.getSample (0, i);
                    finite = finite && std::isfinite (y);
                    if (n > 0 || i > 0)
                        maxStep = juce::jmax (maxStep, std::abs (y - last));
                    last = y;
                }
            }
            const double naturalStep = 0.15 * twoPi * f0 / fs;
            check (finite, "automazione di SAT: uscita sempre finita");
            check (maxStep < 2.5 * naturalStep, "automazione di SAT: nessun click (salto massimo " + juce::String (maxStep, 4)
                                                    + ", una sinusoide pulita ha " + juce::String (naturalStep, 4) + ")");
        }

        // --- 12. risultato indipendente dalla dimensione del blocco, con la saturazione attiva
        {
            std::mt19937 rng (21);
            std::uniform_real_distribution<float> dist (-0.3f, 0.3f);
            Signal in (9600);
            for (auto& v : in) v = dist (rng);

            auto runBlock = [&] (int block)
            {
                ValstMsEqProcessor p;
                configureBand (p, 0, 800.0f, 0.8f, 6.0f, 25.0f, 70.0f);
                return renderMid (p, in, fs, block);
            };

            const auto ref = runBlock (512);
            for (int block : { 1, 7, 64, 480, 4096 })
            {
                const auto other = runBlock (block);
                double maxDiff = 0.0;
                for (size_t i = 0; i < ref.size(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (ref[i] - other[i]));
                check (maxDiff < 1.0e-6, "banda satura, blocchi da " + juce::String (block) + ": identico (diff max " + juce::String (maxDiff, 9) + ")");
            }
        }

        // --- 13. ripetibilita': stesso risultato in istanze diverse e dopo una nuova preparazione
        {
            ToneSetup s = base; s.sat = 80.0f; s.gain = 5.0f;
            const auto a = runTone (s), b = runTone (s);
            bool identical = a.size() == b.size();
            for (size_t i = 0; identical && i < a.size(); ++i)
                identical = a[i] == b[i];
            check (identical, "due istanze con gli stessi parametri producono lo stesso identico audio (bit per bit)");
        }
    }

    Signal makeNoise (int total, double rmsLinear, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> dist (-1.0, 1.0);
        const double a = rmsLinear * std::sqrt (3.0);
        Signal s ((size_t) total);
        for (auto& v : s)
            v = (float) (a * dist (rng));
        return s;
    }

    //==========================================================================
    //  FASE 3: le tre bande
    //==========================================================================
    void testPhase3AllBands()
    {
        section ("F3  Le tre bande: stesso comportamento, tre identita' diverse");

        const double fs = 48000.0;
        const double f0 = fs * 683.0 / 32768.0;                 // ~1000.4 Hz
        const int total = 65536;
        ToneSetup base { (float) f0 };
        base.tone = f0;
        auto harmonicsOf = [&] (const ToneSetup& s) { return analyseHarmonics (runTone (s, total), s.tone, s.rate, 12); };

        // --- 1. ciascuna banda, da sola, si comporta come la banda 1
        for (int b = 0; b < numBands; ++b)
        {
            const juce::String name = "banda " + juce::String (b + 1) + ": ";

            ToneSetup s = base;
            s.band = b; s.gain = 5.0f; s.amp = 0.1;

            {
                const auto y = runTone (s);
                const auto ref = referenceBell (makeSine (f0, s.amp, total, fs), fs, (float) f0, 1.0f, 5.0f, 0.0f);
                double maxDiff = 0.0;
                for (size_t i = 0; i < y.size(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (y[i] - ref[i]));
                check (maxDiff < 1.0e-6, name + "SAT 0 % = campana lineare (diff max " + juce::String (maxDiff, 9) + ")");
            }

            {
                ToneSetup low = s; low.sat = 100.0f; low.amp = linOf (-70.0);
                const auto y = runTone (low);
                const auto ref = referenceBell (makeSine (f0, low.amp, total, fs), fs, (float) f0, 1.0f, 5.0f, 0.0f);
                double maxDiff = 0.0;
                for (size_t i = 0; i < y.size(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (y[i] - ref[i]));
                check (maxDiff < 2.0e-7, name + "SAT 100 % a -70 dBFS: trasparente (diff max " + juce::String (maxDiff, 10) + ")");
            }

            {
                double previous = 0.0;
                bool monotonic = true;
                double thdMax = 0.0;
                for (float sat : { 10.0f, 25.0f, 50.0f, 75.0f, 100.0f })
                {
                    ToneSetup t = s; t.sat = sat; t.gain = 0.0f;
                    const auto h = harmonicsOf (t);
                    monotonic = monotonic && h.thdPercent > previous;
                    previous = h.thdPercent;
                    thdMax = h.thdPercent;
                }
                check (monotonic && thdMax > 5.0, name + "la saturazione cresce con SAT (a 100 %: " + juce::String (thdMax, 1) + " %)");
            }

            {
                ToneSetup t = s; t.sat = 60.0f; t.gain = 0.0f; t.ms = 100.0f;
                t.side = false; const double mid = harmonicsOf (t).thdPercent;
                t.side = true;  const double side = harmonicsOf (t).thdPercent;
                check (mid < 1.0e-3 && side > 1.0, name + "fader +100: lavora solo sul Side (Mid " + juce::String (mid, 5) + " %, Side " + juce::String (side, 2) + " %)");
            }
        }

        // --- 2. tre identita' diverse: stessi parametri, stesso livello, impronte armoniche diverse
        {
            struct Fingerprint { double h2h3Db, tailDb, thdDb; };
            Fingerprint fp[numBands];

            for (int b = 0; b < numBands; ++b)
            {
                ToneSetup s = base;
                s.band = b; s.sat = 60.0f; s.gain = 6.0f; s.amp = 0.08;
                const auto h = harmonicsOf (s);
                fp[b] = { dbOf (h.amp[1] / h.amp[2]), dbOf (highRatio (h)), dbOf (h.thdPercent / 100.0) };
                std::cout << "     banda " << (b + 1) << " (stessi parametri): H2/H3 " << juce::String (fp[b].h2h3Db, 1) << " dB, armoniche alte/H3 "
                          << juce::String (fp[b].tailDb, 1) << " dB, THD " << juce::String (fp[b].thdDb, 1) << " dB" << std::endl;
            }

            for (int a = 0; a < numBands; ++a)
                for (int b = a + 1; b < numBands; ++b)
                {
                    const double shape = std::sqrt (std::pow (fp[a].h2h3Db - fp[b].h2h3Db, 2.0) + std::pow (fp[a].tailDb - fp[b].tailDb, 2.0));
                    const double dose  = std::abs (fp[a].thdDb - fp[b].thdDb);
                    check (shape > 4.0, "banda " + juce::String (a + 1) + " e banda " + juce::String (b + 1) + ": forma armonica distinguibile (distanza "
                                            + juce::String (shape, 1) + " dB su pari/dispari e armoniche alte)");
                    check (dose < 6.0, "banda " + juce::String (a + 1) + " e banda " + juce::String (b + 1) + ": stessa 'dose' di saturazione con la stessa manopola (THD differisce di "
                                           + juce::String (dose, 1) + " dB)");
                }
        }

        // --- 3. indipendenza dei controlli: i parametri di una banda non cambiano il suono delle altre
        {
            auto runMix = [&] (bool alterOthers)
            {
                ValstMsEqProcessor p;
                configureBand (p, 0, 800.0f, 0.9f, 4.0f, 10.0f, 75.0f);
                if (alterOthers)
                {
                    configureBand (p, 1, 3000.0f, 0.2f, 0.0f, -60.0f, 0.0f);
                    configureBand (p, 2, 150.0f, 5.0f, 0.0f, 90.0f, 0.0f);
                    p.setSeed (1, 0x123456789ABCDEF0ULL);
                    p.setSeed (2, 0x0FEDCBA987654321ULL);
                }
                return renderMid (p, makeNoise (24000, 0.15, 3), fs);
            };

            const auto a = runMix (false), b = runMix (true);
            bool identical = true;
            for (size_t i = 0; identical && i < a.size(); ++i)
                identical = a[i] == b[i];
            check (identical, "cambiare freq, larghezza, fader e seed di due bande spente non altera in alcun modo la terza (bit per bit)");
        }

        // --- 4. il seed decide l'identita' di una banda
        {
            auto runSeed = [&] (uint64_t seed)
            {
                ValstMsEqProcessor p;
                p.setSeed (0, seed);
                configureBand (p, 0, (float) f0, 1.0f, 6.0f, 0.0f, 60.0f);
                return renderMid (p, makeSine (f0, 0.08, total, fs), fs);
            };

            const auto original = runSeed (sat::defaultSeeds[0]);
            const auto other    = runSeed (0x0123456789ABCDEFULL);
            const auto again    = runSeed (sat::defaultSeeds[0]);

            const auto ho = analyseHarmonics (original, f0, fs), hx = analyseHarmonics (other, f0, fs);
            const double d = std::abs (dbOf (ho.amp[1] / ho.amp[2]) - dbOf (hx.amp[1] / hx.amp[2])) + std::abs (dbOf (highRatio (ho)) - dbOf (highRatio (hx)));
            bool restored = true;
            for (size_t i = 0; restored && i < original.size(); ++i)
                restored = original[i] == again[i];

            check (d > 1.0, "un altro seed cambia il carattere della banda (differenza armonica " + juce::String (d, 1) + " dB)");
            check (restored, "rimettendo il seed originale il suono torna identico bit per bit");
        }

        // --- 5. le tre bande insieme su materiale forte: tutto finito, limitato, senza DC
        {
            ValstMsEqProcessor p;
            configureBand (p, 0, 90.0f, 1.0f, 8.0f, -40.0f, 90.0f);
            configureBand (p, 1, 1500.0f, 0.7f, -5.0f, 30.0f, 100.0f);
            configureBand (p, 2, 9000.0f, 1.5f, 10.0f, 0.0f, 100.0f);

            auto in = makeNoise (total, linOf (-12.0), 8);
            const auto bass = makeSine (60.0, 0.3, total, fs);
            for (size_t i = 0; i < in.size(); ++i)
                in[i] += bass[i];

            const auto y = renderMid (p, in, fs);
            bool finite = true;
            for (float v : y)
                finite = finite && std::isfinite (v);
            check (finite, "tre bande al massimo su rumore e basso forti: uscita sempre finita");
            check (peakOf (y, 0, total) < 3.0, "uscita limitata (picco " + juce::String (peakOf (y, 0, total), 2) + ")");
            check (std::abs (meanOf (y, total - 32768, 32768)) < 1.0e-3, "nessun DC (media " + juce::String (meanOf (y, total - 32768, 32768), 6) + ")");
        }
    }

    //==========================================================================
    //  FASE 4: interazione progressiva tra le saturazioni
    //==========================================================================
    void testPhase4Interaction()
    {
        section ("F4  Interazione progressiva tra le saturazioni delle bande");

        const double fs = 48000.0;
        const double f1 = fs * 137.0 / 32768.0;                 // ~200.7 Hz; 3*f1 = 602 Hz; 9*f1 = 1806 Hz
        const int total = 65536, start = 32768, len = 32768;

        // Due bande in serie, entrambe sul Mid. 'sameIdentity' dà alle due bande lo stesso seed: cosi' l'unica
        // differenza tra due configurazioni scambiate e' l'ORDINE in cui il segnale le attraversa.
        struct Cfg { float freq, width, gain, sat; };

        auto chain = [&] (int bandA, Cfg a, int bandB, Cfg b, bool sameIdentity, double amplitude = 0.12)
        {
            ValstMsEqProcessor p;
            if (sameIdentity)
            {
                p.setSeed (bandA, sat::defaultSeeds[0]);
                p.setSeed (bandB, sat::defaultSeeds[0]);
            }
            configureBand (p, bandA, a.freq, a.width, a.gain, 0.0f, a.sat);
            configureBand (p, bandB, b.freq, b.width, b.gain, 0.0f, b.sat);
            return renderMid (p, makeSine (f1, amplitude, total, fs), fs);
        };
        auto withSat = [] (Cfg c, float sat) { c.sat = sat; return c; };
        auto rmsDiff = [&] (const Signal& a, const Signal& b)           // RMS della differenza nella finestra di analisi
        {
            double sum = 0.0;
            for (int i = start; i < start + len; ++i)
            {
                const double d = (double) a[(size_t) i] - (double) b[(size_t) i];
                sum += d * d;
            }
            return std::sqrt (sum / (double) len);
        };
        auto level = [&] (const Signal& y, double f) { return toneAmplitude (y, start, len, f, fs); };

        const Cfg c1 { (float) f1,         1.0f, 0.0f, 100.0f };       // banda larga sul tono
        const Cfg c2 { (float) (3.0 * f1), 0.5f, 0.0f, 100.0f };       // banda stretta sulla 3a armonica

        // Termine di interazione = uscita(B1+B2) - uscita(solo B1) - uscita(solo B2) + uscita(nessuna).
        // Se le bande fossero elaborazioni isolate sommate in parallelo sarebbe esattamente zero:
        // e' diverso da zero solo se la banda a valle lavora sul segnale gia' deformato da quella a monte.
        auto interaction = [&] (int bandA, int bandB, const juce::String& name, double amplitude)
        {
            const auto y0 = chain (bandA, withSat (c1, 0.0f), bandB, withSat (c2, 0.0f), false, amplitude);
            const auto y1 = chain (bandA, c1,                 bandB, withSat (c2, 0.0f), false, amplitude);
            const auto y2 = chain (bandA, withSat (c1, 0.0f), bandB, c2,                 false, amplitude);
            const auto yb = chain (bandA, c1,                 bandB, c2,                 false, amplitude);

            Signal cross (yb.size());
            for (size_t i = 0; i < yb.size(); ++i)
                cross[i] = yb[i] - y1[i] - y2[i] + y0[i];

            const double rCross = rmsOf (cross, start, len), r1 = rmsDiff (y1, y0), r2 = rmsDiff (y2, y0);
            const double a9both = level (yb, 9.0 * f1), a9first = level (y1, 9.0 * f1), a9second = level (y2, 9.0 * f1);

            std::cout << "     " << name << ": residuo della prima banda " << juce::String (dbOf (r1), 1) << " dBFS, della seconda "
                      << juce::String (dbOf (r2), 1) << " dBFS, termine di interazione " << juce::String (dbOf (rCross), 1)
                      << " dBFS | 9a armonica: insieme " << juce::String (dbOf (a9both), 1) << ", solo prima " << juce::String (dbOf (a9first), 1)
                      << ", solo seconda " << juce::String (dbOf (a9second), 1) << std::endl;

            check (dbOf (rCross) > -100.0, name + ": il termine di interazione e' reale (" + juce::String (dbOf (rCross), 1) + " dBFS, una somma in parallelo darebbe -inf)");
            check (dbOf (rCross) > dbOf (juce::jmin (r1, r2)) - 30.0, name + ": l'interazione pesa quanto una frazione sensibile del residuo piu' piccolo ("
                                                                         + juce::String (dbOf (rCross) - dbOf (juce::jmin (r1, r2)), 1) + " dB)");
        };

        interaction (0, 1, "banda 1 -> banda 2", 0.12);
        interaction (0, 2, "banda 1 -> banda 3 (con la 2 neutra in mezzo)", 0.12);
        interaction (1, 2, "banda 2 -> banda 3", 0.12);
        interaction (0, 1, "banda 1 -> banda 2, segnale piu' forte (-8 dBFS)", 0.4);

        // l'ordine conta: due bande con la stessa identita' scambiate di posto danno un risultato diverso.
        // Qui entrambe lavorano sulla stessa frequenza, una larga con boost e una stretta con taglio: i due stadi
        // non lineari non commutano, quindi l'uscita dipende da quale viene prima.
        {
            const Cfg wide { (float) f1, 2.0f, 9.0f, 100.0f }, narrow { (float) f1, 0.4f, -6.0f, 100.0f };
            const auto none = chain (0, withSat (wide, 0.0f), 1, withSat (narrow, 0.0f), true, 0.3);
            const auto ab   = chain (0, wide, 1, narrow, true, 0.3);
            const auto ba   = chain (0, narrow, 1, wide, true, 0.3);

            const double residual = rmsDiff (ab, none), swapDiff = rmsDiff (ab, ba);
            std::cout << "     residuo di distorsione " << juce::String (dbOf (residual), 1) << " dBFS, differenza tra i due ordini "
                      << juce::String (dbOf (swapDiff), 1) << " dBFS" << std::endl;
            check (swapDiff > 0.05 * residual, "scambiando l'ordine delle bande il risultato cambia (" + juce::String (dbOf (swapDiff / residual), 1)
                                                   + " dB rispetto alla distorsione): e' una catena in serie, non una somma");

            // senza saturazione le campane sono filtri lineari e commutano: nessuna differenza
            const Cfg g1 { (float) f1, 1.0f, 6.0f, 0.0f }, g2 { (float) (3.0 * f1), 0.5f, -4.0f, 0.0f };
            const double linearDiff = rmsDiff (chain (0, g1, 1, g2, true), chain (0, g2, 1, g1, true));
            check (linearDiff < 1.0e-6, "con SAT a 0 l'ordine non conta (filtri lineari): differenza " + juce::String (linearDiff, 9));
        }

        // i parametri restano indipendenti: sono tre insiemi separati, anche se l'audio passa in serie
        {
            ValstMsEqProcessor p;
            configureBand (p, 0, 120.0f, 0.8f, 3.0f, -20.0f, 30.0f);
            configureBand (p, 1, 1200.0f, 1.2f, -4.0f, 40.0f, 60.0f);
            configureBand (p, 2, 6000.0f, 2.0f, 5.0f, 0.0f, 90.0f);
            configureBand (p, 1, 2500.0f, 0.4f, 9.0f, -70.0f, 10.0f);          // cambio solo la banda 2
            auto same = [] (float a, float b) { return std::abs (a - b) <= 1.0e-3f * juce::jmax (1.0f, std::abs (b)); };
            const bool untouched = same (p.getParam (0, kFreq), 120.0f) && same (p.getParam (0, kWidth), 0.8f) && same (p.getParam (0, kGain), 3.0f)
                                && same (p.getParam (0, kMs), -20.0f) && same (p.getParam (0, kSat), 30.0f)
                                && same (p.getParam (2, kFreq), 6000.0f) && same (p.getParam (2, kWidth), 2.0f) && same (p.getParam (2, kGain), 5.0f)
                                && same (p.getParam (2, kMs), 0.0f) && same (p.getParam (2, kSat), 90.0f);
            check (untouched, "regolare una banda non cambia i parametri delle altre");
        }

        // gain staging: SAT al massimo su tutte le bande non fa esplodere il volume
        {
            std::cout << "     variazione di livello con SAT 100 % su tutte le bande (rumore, EQ con boost):";
            double worstUp = -100.0, worstDown = 100.0;

            for (double rmsDb : { -40.0, -30.0, -24.0, -18.0, -12.0 })
            {
                const auto in = makeNoise (total, linOf (rmsDb), 11);
                auto configure = [&] (ValstMsEqProcessor& p, float sat)
                {
                    configureBand (p, 0, 100.0f, 1.0f, 6.0f, 0.0f, sat);
                    configureBand (p, 1, 1000.0f, 1.0f, 4.0f, 0.0f, sat);
                    configureBand (p, 2, 6000.0f, 1.0f, 6.0f, 0.0f, sat);
                };

                ValstMsEqProcessor flat, hot;
                configure (flat, 0.0f);
                configure (hot, 100.0f);
                const auto yFlat = renderMid (flat, in, fs), yHot = renderMid (hot, in, fs);
                const double delta = dbOf (rmsOf (yHot, 8192, total - 8192) / rmsOf (yFlat, 8192, total - 8192));
                std::cout << "  " << (int) rmsDb << " dB RMS: " << juce::String (delta, 1) << " dB";
                worstUp = juce::jmax (worstUp, delta);
                worstDown = juce::jmin (worstDown, delta);
            }
            std::cout << std::endl;
            check (worstUp < 3.0, "la saturazione non fa salire il livello di piu' di 3 dB (massimo " + juce::String (worstUp, 1) + " dB)");
            check (worstDown > -9.0, "ne' lo fa crollare (minimo " + juce::String (worstDown, 1) + " dB, su materiale molto forte)");
        }
    }

    double maxDiffOf (const Signal& a, const Signal& b, size_t from = 0)
    {
        double m = 0.0;
        for (size_t i = from; i < a.size() && i < b.size(); ++i)
            m = juce::jmax (m, (double) std::abs (a[i] - b[i]));
        return m;
    }

    //==========================================================================
    //  FASE 5: gain di uscita + saturazione a trasformatore
    //==========================================================================
    void testPhase5Transformer()
    {
        section ("F5  Uscita: gain + saturazione a trasformatore");

        const double fs = 48000.0;
        const double f0 = fs * 683.0 / 32768.0;                 // ~1000.4 Hz
        const int total = 65536;

        auto runAt = [&] (double amplitude, float outDb, double freq)
        {
            ValstMsEqProcessor p;
            setOutGain (p, outDb);
            return renderMid (p, makeSine (freq, amplitude, total, fs), fs);
        };
        auto run   = [&] (double amplitude, float outDb) { return runAt (amplitude, outDb, f0); };
        auto harmAt = [&] (const Signal& y, double freq) { return analyseHarmonics (y, freq, fs, 12); };
        auto harm   = [&] (const Signal& y) { return harmAt (y, f0); };

        // --- 1. fino a 0 dBFS lo stadio e' trasparente: l'uscita e' identica all'ingresso
        {
            // Il filtro di "flusso" ha una breve risposta transitoria (costante di tempo 1.6 ms) quando un tono parte
            // di colpo a fase zero: si confronta il regime (dopo 4096 campioni) e si riporta a parte l'avvio.
            double worst = 0.0, startup = 0.0;
            for (double a : { 0.05, 0.3, 0.7, 0.9, 0.98 })
            {
                const auto y = run (a, 0.0f);
                const auto x = makeSine (f0, a, total, fs);
                for (size_t i = 0; i < y.size(); ++i)
                {
                    const double d = std::abs ((double) y[i] - (double) x[i]);
                    (i < 4096 ? startup : worst) = juce::jmax (i < 4096 ? startup : worst, d);
                }
            }
            check (worst < 1.0e-6, "sotto 0 dBFS (picchi fino a -0.2 dBFS) il segnale esce identico a regime (diff max " + juce::String (worst, 9) + ")");
            std::cout << "     avvio brusco di un tono a -0.2 dBFS: scostamento massimo nei primi 85 ms " << juce::String (startup, 7)
                      << " (" << juce::String (dbOf (startup), 0) << " dBFS)" << std::endl;
        }

        // --- 2. l'Output Gain e' un gain puro finche' non si raggiunge la soglia
        {
            const auto in = makeSine (f0, 0.5, total, fs);
            Signal expected (in);
            for (auto& v : expected) v *= (float) linOf (-6.0);
            check (maxDiffOf (run (0.5, -6.0f), expected) < 1.0e-6, "Output Gain -6 dB: attenuazione esatta, nessuna distorsione aggiunta");

            Signal boosted (in);
            for (auto& v : boosted) v *= (float) linOf (3.0);
            check (maxDiffOf (run (0.5, 3.0f), boosted) < 1.0e-6, "Output Gain +3 dB su un segnale a -6 dBFS: gain puro (resta sotto 0 dBFS)");
        }

        // --- 3. sopra 0 dBFS: compressione progressiva, mai un muro
        {
            const double ref = harm (run (0.01, 0.0f)).amp[0] / 0.01;        // guadagno a livello basso = 1
            std::cout << "     riduzione di guadagno:";
            double previous = 0.0;
            bool monotonic = true;
            double at3 = 0.0, at6 = 0.0, at12 = 0.0, out9 = 0.0, out12 = 0.0, atZero = 0.0;

            for (double dbfs : { 0.0, 3.0, 6.0, 9.0, 12.0 })
            {
                const double a = linOf (dbfs);
                const double out = harm (run (a, 0.0f)).amp[0];
                const double reduction = -dbOf (out / a / ref);
                std::cout << "  " << (dbfs >= 0 ? "+" : "") << (int) dbfs << " dBFS: " << juce::String (reduction, 2) << " dB";
                monotonic = monotonic && reduction > previous - 1.0e-9;
                previous = reduction;
                if (dbfs == 0.0) atZero = reduction;
                if (dbfs == 3.0) at3 = reduction;
                if (dbfs == 6.0) at6 = reduction;
                if (dbfs == 9.0) out9 = out;
                if (dbfs == 12.0) { at12 = reduction; out12 = out; }
            }
            std::cout << std::endl;

            check (atZero < 0.2, "a 0 dBFS la saturazione e' appena all'inizio (" + juce::String (atZero, 2) + " dB)");
            check (monotonic, "la compressione aumenta in modo continuo con il livello");
            check (at3 > 0.2 && at3 < 1.5, "+3 dBFS: compressione dolce (" + juce::String (at3, 2) + " dB)");
            check (at6 > 1.5 && at6 < 4.0, "+6 dBFS: compressione marcata (" + juce::String (at6, 2) + " dB)");
            check (at12 > 5.0, "+12 dBFS: forte (" + juce::String (at12, 2) + " dB)");
            check (out12 > out9, "non e' un limitatore: l'uscita continua a salire, piano, anche oltre +9 dBFS (" + juce::String (dbOf (out9), 2)
                                     + " -> " + juce::String (dbOf (out12), 2) + " dBFS)");
        }

        // --- 4. armoniche: simmetrico = dispari, diverso dalla valvola
        {
            const auto t = harm (run (linOf (6.0), 0.0f));
            ValstMsEqProcessor valve;
            setInGain (valve, 12.0f);
            const auto v = harm (renderMid (valve, makeSine (f0, linOf (-18.0) * std::sqrt (2.0), total, fs), fs));

            std::cout << "     trasformatore a +6 dBFS: H2 " << juce::String (t.db (2), 1) << " dBc, H3 " << juce::String (t.db (3), 1)
                      << " dBc, H5 " << juce::String (t.db (5), 1) << " dBc | THD " << juce::String (t.thdPercent, 1) << " %" << std::endl;
            check (t.amp[1] < 0.01 * t.amp[2], "curva simmetrica: la 2a armonica e' a " + juce::String (t.db (2), 1) + " dBc, la 3a a " + juce::String (t.db (3), 1) + " dBc (solo dispari)");
            check (t.db (3) > -25.0, "sopra la soglia la generazione armonica e' netta (3a a " + juce::String (t.db (3), 1) + " dBc)");
            check (v.amp[1] / v.amp[2] > 0.3 && t.amp[1] / t.amp[2] < 0.01,
                   "carattere diverso dalla valvola: H2/H3 = " + juce::String (v.amp[1] / v.amp[2], 2) + " (valvola) contro " + juce::String (t.amp[1] / t.amp[2], 4) + " (trasformatore)");

            const double dc = meanOf (run (linOf (6.0), 0.0f), total - 32768, 32768);
            check (std::abs (dc) < 1.0e-6, "nessun DC (media " + juce::String (dc, 9) + ")");
        }

        // --- 5. il nucleo satura prima sulle basse frequenze
        {
            const double fLow = fs * 41.0 / 32768.0;             // ~60 Hz
            const auto low  = harmAt (runAt (0.84, 0.0f, fLow), fLow);
            const auto mid  = harm (run (0.84, 0.0f));
            std::cout << "     a -1.5 dBFS: THD a 60 Hz " << juce::String (low.thdPercent, 4) << " %, a 1 kHz " << juce::String (mid.thdPercent, 6) << " %" << std::endl;
            check (low.thdPercent > 0.05 && mid.thdPercent < 1.0e-4, "a -1.5 dBFS il basso (60 Hz) gia' satura, il medio (1 kHz) no: comportamento magnetico dipendente dalla frequenza");
        }

        // --- 6. l'Output Gain pilota la saturazione e permette di compensare il livello
        {
            const double hotThd   = harm (run (1.41, 0.0f)).thdPercent;                       // +3 dBFS
            const double compThd  = harm (run (1.41, -6.0f)).thdPercent;                      // +3 dBFS ma con -6 dB di Output Gain
            const double quietThd = harm (run (0.5, 0.0f)).thdPercent;                        // -6 dBFS
            const double drivenThd = harm (run (0.5, 9.0f)).thdPercent;                       // -6 dBFS + 9 dB
            check (hotThd > 0.5 && compThd < 1.0e-4, "segnale a +3 dBFS: satura (" + juce::String (hotThd, 2) + " %); con Output Gain -6 dB rientra e torna pulito ("
                                                          + juce::String (compThd, 6) + " %)");
            check (quietThd < 1.0e-4 && drivenThd > 0.5, "segnale a -6 dBFS: pulito (" + juce::String (quietThd, 6) + " %); con Output Gain +9 dB satura (" + juce::String (drivenThd, 2) + " %)");
        }

        // --- 7. la curva e' continua e liscia, esattamente lineare sotto la soglia, mai piatta
        {
            const auto shaper = sat::makeTransformerShaper();
            auto curve = [&] (double e) { return e + shaper.h (sat::trafoDrive * e) / sat::trafoDrive; };

            double maxLinearErr = 0.0, maxSlopeChange = 0.0, minSlopeAbove = 10.0;
            const double h = 1.0e-3;
            for (double e = -5.0; e < 5.0; e += h)
            {
                const double d0 = (curve (e + h) - curve (e)) / h;
                const double d1 = (curve (e + 2.0 * h) - curve (e + h)) / h;
                maxSlopeChange = juce::jmax (maxSlopeChange, std::abs (d1 - d0));
                if (std::abs (e) <= 1.0)
                    maxLinearErr = juce::jmax (maxLinearErr, std::abs (curve (e) - e));
                if (std::abs (e) >= 2.5)
                    minSlopeAbove = juce::jmin (minSlopeAbove, d0);
            }
            check (maxLinearErr == 0.0, "fino a |x| = 1 (0 dBFS) la curva e' esattamente la retta y = x");
            check (maxSlopeChange < 0.002, "curva continua e liscia: variazione massima della pendenza " + juce::String (maxSlopeChange, 5) + " per passo (clipping netto: 1.0)");
            check (minSlopeAbove > 0.03 && minSlopeAbove < 0.08, "oltre il ginocchio la pendenza non va a zero (" + juce::String (minSlopeAbove, 3) + "): nessun clipping digitale");
        }

        // --- 8. anti-aliasing ADAA anche qui
        {
            const double fsLow = 44100.0, f = 5010.7;
            const int n = 131072;
            const auto in = makeSine (f, 1.0, n, fsLow);
            Signal outOn (in), outOff (in), dummy (in);

            auto runEngine = [&] (float outDb, bool adaa, Signal& out)
            {
                MsEqEngine e;
                e.prepare (fsLow);
                e.setOutput (outDb);
                e.setAntiAliasing (adaa);
                e.snapToTargets();
                out = in;
                dummy = in;
                e.process (out.data(), dummy.data(), n);
            };

            // l'ADAA di primo ordine riduce l'aliasing (non lo elimina): di poco appena sopra 0 dBFS, di piu' con piu' pilotaggio
            for (float outDb : { 6.0f, 12.0f })
            {
                runEngine (outDb, true, outOn);
                runEngine (outDb, false, outOff);

                const int kmax = (int) (0.5 * fsLow / f);
                const double on = inharmonicDb (outOn, f, fsLow, kmax), off = inharmonicDb (outOff, f, fsLow, kmax);
                std::cout << "     aliasing a 5 kHz, +" << (int) outDb << " dBFS, 44.1 kHz: senza ADAA " << juce::String (off, 1) << " dB, con ADAA " << juce::String (on, 1) << " dB" << std::endl;
                check (off - on > 3.0, "+" + juce::String ((int) outDb) + " dBFS: l'ADAA riduce l'aliasing del trasformatore di " + juce::String (off - on, 1) + " dB");
            }
        }

        // --- 9. canali indipendenti, mono = Mid, automazione senza click
        {
            ValstMsEqProcessor p;
            setOutGain (p, 6.0f);
            const auto loud = makeSine (f0, 1.0, 16384, fs), quiet = makeSine (f0, 0.1, 16384, fs);
            Signal ol, orr;
            renderStereo (p, loud, quiet, ol, orr, fs);
            double rDiff = 0.0;
            for (size_t i = 0; i < quiet.size(); ++i)
                rDiff = juce::jmax (rDiff, (double) std::abs (orr[i] - quiet[i] * (float) linOf (6.0)));
            check (rDiff < 1.0e-6, "il canale sinistro che satura non sporca il destro (diff max " + juce::String (rDiff, 9) + ")");

            ValstMsEqProcessor stereo, mono;
            setOutGain (stereo, 6.0f);
            setOutGain (mono, 6.0f);
            const auto ref = renderMid (stereo, loud, fs);
            mono.setPlayConfigDetails (1, 1, fs, 512);
            mono.prepareToPlay (fs, 512);
            juce::AudioBuffer<float> buffer (1, 512);
            juce::MidiBuffer midi;
            double maxDiff = 0.0;
            for (int n = 0; n < (int) loud.size(); n += 512)
            {
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (0, i, loud[(size_t) (n + i)]);
                mono.processBlock (buffer, midi);
                for (int i = 0; i < 512; ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (buffer.getSample (0, i) - ref[(size_t) (n + i)]));
            }
            check (maxDiff < 1.0e-7, "mono con trasformatore attivo: identico al Mid stereo");
        }

        {
            ValstMsEqProcessor p;
            preparePlayback (p, fs, 256);
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            double last = 0.0, maxStep = 0.0;
            bool finite = true;

            for (int n = 0; n < (int) fs * 3; n += 256)
            {
                const double t = (double) n / fs;
                setOutGain (p, t < 1.0 ? (float) (-24.0 + 36.0 * t) : (t < 2.0 ? -24.0f : 12.0f));
                for (int i = 0; i < 256; ++i)
                {
                    const float x = (float) (0.5 * std::sin (twoPi * 440.0 * (double) (n + i) / fs));
                    buffer.setSample (0, i, x);
                    buffer.setSample (1, i, x);
                }
                p.processBlock (buffer, midi);
                for (int i = 0; i < 256; ++i)
                {
                    const double y = buffer.getSample (0, i);
                    finite = finite && std::isfinite (y);
                    if (n > 0 || i > 0)
                        maxStep = juce::jmax (maxStep, std::abs (y - last));
                    last = y;
                }
            }
            check (finite, "automazione dell'Output Gain: uscita sempre finita");
            check (maxStep < 0.25, "automazione dell'Output Gain: nessun click (salto massimo " + juce::String (maxStep, 4) + ")");
        }
    }

    //==========================================================================
    //  Saturazione profonda: nessuna "perdita" di segnale non limitato (difetto trovato in fase di verifica)
    //==========================================================================
    void testDeepSaturation()
    {
        section ("Saturazione profonda: anche l'alta frequenza viene limitata");

        const double fs = 48000.0;
        const int total = 65536;

        // valvola a +24 dB con un seno a 0 dBFS: la saturazione e' totale e il picco in uscita e' il tetto della curva
        // (compensato), uguale a tutte le frequenze. Senza la correzione di allineamento dell'ADAA a 10 kHz usciva 4 volte tanto.
        {
            std::cout << "     valvola +24 dB, seno a 0 dBFS, picco in uscita:";
            double worst = 0.0;
            for (double f : { 200.0, 1000.0, 3000.0, 6000.0, 10000.0, 15000.0 })
            {
                ValstMsEqProcessor p;
                setInGain (p, 24.0f);
                const double peak = peakOf (renderMid (p, makeSine (f, 1.0, total, fs), fs), 16384, total - 16384);
                std::cout << "  " << (int) f << " Hz " << juce::String (peak, 3);
                worst = juce::jmax (worst, peak);
            }
            std::cout << std::endl;
            check (worst < 0.45, "valvola in saturazione profonda: il picco resta al tetto della curva a tutte le frequenze (massimo " + juce::String (worst, 3) + ")");
        }

        {
            ValstMsEqProcessor p;
            setInGain (p, 24.0f);
            const auto y = renderMid (p, makeNoise (total, linOf (-6.0), 9), fs);
            const double peak = peakOf (y, 8192, total - 8192), rms = rmsOf (y, 8192, total - 8192);
            check (peak < 0.4, "valvola +24 dB con rumore a -6 dB RMS: picco " + juce::String (peak, 3) + ", RMS " + juce::String (rms, 3) + " (il rumore e' limitato, non amplificato)");
        }

        // banda ad alta frequenza con SAT al massimo e segnale forte: il segnale della banda viene limitato, non lasciato passare
        {
            ValstMsEqProcessor on, off;
            configureBand (on,  2, 9000.0f, 1.0f, 0.0f, 0.0f, 100.0f);
            configureBand (off, 2, 9000.0f, 1.0f, 0.0f, 0.0f, 0.0f);
            const auto in = makeSine (9000.0, 0.5, total, fs);
            const double a = toneAmplitude (renderMid (on, in, fs), 32768, 32768, 9000.0, fs);
            const double b = toneAmplitude (renderMid (off, in, fs), 32768, 32768, 9000.0, fs);
            check (a < 0.35 * b, "banda a 9 kHz con SAT 100 % su un tono a -6 dBFS: la fondamentale scende a " + juce::String (100.0 * a / b, 1)
                                     + " % del livello senza saturazione (la banda satura davvero)");
        }

        // trasformatore: seno a +12 dBFS a varie frequenze, il picco segue la curva (0.05 * x + 1.36), non cresce con la frequenza
        {
            std::cout << "     trasformatore, seno a +12 dBFS, picco in uscita:";
            double worst = 0.0;
            for (double f : { 1000.0, 5000.0, 10000.0, 15000.0 })
            {
                ValstMsEqProcessor p;
                const double peak = peakOf (renderMid (p, makeSine (f, 4.0, total, fs), fs), 16384, total - 16384);
                std::cout << "  " << (int) f << " Hz " << juce::String (peak, 3);
                worst = juce::jmax (worst, peak);
            }
            std::cout << std::endl;
            check (worst < 1.8, "trasformatore in saturazione profonda: picco massimo " + juce::String (worst, 3) + " (la curva dice 1.56)");
        }
    }

    //==========================================================================
    //  FASE 6: filtri passa-alto
    //==========================================================================
    void testPhase6HighPass()
    {
        section ("F6  Filtri passa-alto: banda 1 (6 dB/ott, morbido) e banda 3 (12 dB/ott, piu' ripido)");

        auto filterFor = [] (int which, double fs, double fc) { return which == 0 ? makeBand1HighPass (fs, fc) : makeBand3HighPass (fs, fc); };

        // --- 1. la risposta misurata sull'audio coincide con quella teorica, a piu' frequenze di campionamento
        for (int which : { 0, 1 })
        {
            double worst = 0.0;
            for (double fs : { 44100.0, 48000.0, 96000.0 })
                for (double f : { 25.0, 50.0, 100.0, 200.0, 400.0, 1000.0, 5000.0 })
                {
                    ValstMsEqProcessor p;
                    flatten (p);
                    setHpf (p, which, 200.0f);
                    worst = juce::jmax (worst, std::abs (measureGainDb (p, f, false, fs) - filterFor (which, fs, 200.0).magnitudeDb (f, fs)));
                }
            check (worst < 0.05, juce::String (which == 0 ? "banda 1" : "banda 3") + ": risposta misurata = teorica a 44.1 / 48 / 96 kHz su 7 frequenze (scarto max "
                                     + juce::String (worst, 4) + " dB)");
        }

        // --- 2. frequenza di taglio, pendenza, differenza tra i due
        {
            auto gainAt = [&] (int which, double cutoff, double f)
            {
                ValstMsEqProcessor p;
                flatten (p);
                setHpf (p, which, (float) cutoff);
                return measureGainDb (p, f, false);
            };

            checkNear (gainAt (0, 200.0, 200.0), -3.01, 0.1, "banda 1: alla frequenza di taglio -3 dB");
            checkNear (gainAt (1, 200.0, 200.0), -4.44, 0.1, "banda 3: alla frequenza di taglio -4.4 dB (Q 0.6, nessun picco di risonanza)");

            const double slope1 = gainAt (0, 400.0, 50.0) - gainAt (0, 400.0, 25.0);
            const double slope3 = gainAt (1, 1000.0, 125.0) - gainAt (1, 1000.0, 62.5);
            checkNear (slope1, 6.0, 0.4, "banda 1: pendenza di " + juce::String (slope1, 2) + " dB/ottava (graduale)");
            checkNear (slope3, 12.0, 0.6, "banda 3: pendenza di " + juce::String (slope3, 2) + " dB/ottava (piu' ripida)");

            const double one = gainAt (0, 200.0, 50.0), three = gainAt (1, 200.0, 50.0);
            check (three < one - 10.0, "a due ottave sotto il taglio la banda 3 attenua " + juce::String (three, 1) + " dB, la banda 1 solo " + juce::String (one, 1) + " dB");

            ValstMsEqProcessor p;
            flatten (p);
            setHpf (p, 0, 400.0f);
            setHpf (p, 1, 1000.0f);
            double peak = -100.0;
            for (double f : { 400.0, 600.0, 1000.0, 1300.0, 2000.0, 4000.0, 10000.0 })
                peak = juce::jmax (peak, measureGainDb (p, f, false));
            check (peak < 0.05, "nessun picco di risonanza sopra il taglio, nemmeno con i due filtri insieme (massimo " + juce::String (peak, 3) + " dB)");
        }

        // --- 3. spento = esattamente trasparente, anche dopo essere stato usato
        {
            const double fs = 48000.0;
            ValstMsEqProcessor p;
            const auto in = makeNoise (24000, 0.2, 21);
            Signal l, r;

            renderStereo (p, in, in, l, r, fs, 512, true);
            const double offDiff = maxDiffOf (l, in);

            setHpf (p, 0, 150.0f);
            setHpf (p, 1, 300.0f);
            renderStereo (p, in, in, l, r, fs, 512, false);
            const double onDiff = maxDiffOf (l, in);

            setHpf (p, 0, hpfMinHz);
            setHpf (p, 1, hpfMinHz);
            renderStereo (p, in, in, l, r, fs, 512, false);               // dissolvenza di 30 ms
            renderStereo (p, in, in, l, r, fs, 512, false);               // a regime
            const double afterDiff = maxDiffOf (l, in);

            check (offDiff < 1.0e-7, "passa-alto al minimo (OFF): il segnale esce identico (diff max " + juce::String (offDiff, 9) + ")");
            check (onDiff > 0.05, "con i passa-alto accesi il segnale cambia (diff max " + juce::String (onDiff, 3) + ")");
            check (afterDiff < 1.0e-7, "dopo averli spenti torna identico, senza residui (diff max " + juce::String (afterDiff, 9) + ")");
        }

        // --- 4. Mid e Side ricevono lo stesso filtro
        {
            ValstMsEqProcessor p;
            flatten (p);
            setHpf (p, 0, 250.0f);
            setHpf (p, 1, 250.0f);
            const double mid = measureGainDb (p, 120.0, false), side = measureGainDb (p, 120.0, true);
            checkNear (side, mid, 0.01, "i passa-alto lavorano uguale su Mid e Side (" + juce::String (mid, 2) + " dB)");
        }

        // --- 5. il grafico (LinearResponse) mostra la stessa risposta dell'audio: campane + passa-alto insieme
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            ValstMsEqProcessor p;
            flatten (p);
            setBand (p, 0,   120.0f, 1.8f, -6.0f, -40.0f);
            setBand (p, 1,  1800.0f, 0.7f,  9.0f,  60.0f);
            setBand (p, 2,  9000.0f, 1.2f,  7.0f,   0.0f);
            setHpf (p, 0, 90.0f);
            setHpf (p, 1, 250.0f);

            std::array<LinearResponse::BandSetting, (size_t) numBands> settings;
            for (int b = 0; b < numBands; ++b)
                settings[(size_t) b] = { p.getParam (b, kFreq), p.getParam (b, kWidth), p.getParam (b, kGain), p.getParam (b, kMs) };

            LinearResponse model;
            model.setup (fs, settings, p.getGlobal (kHpf1), p.getGlobal (kHpf3));

            double worst = 0.0;
            for (double f : { 30.0, 60.0, 120.0, 250.0, 400.0, 1800.0, 5000.0, 9000.0, 15000.0 })
            {
                worst = juce::jmax (worst, std::abs (measureGainDb (p, f, false, fs) - model.totalDb (false, f)));
                worst = juce::jmax (worst, std::abs (measureGainDb (p, f, true,  fs) - model.totalDb (true,  f)));
            }
            check (worst < 0.05, "fs " + juce::String (fs, 0) + " Hz: grafico = audio con campane e passa-alto (scarto max " + juce::String (worst, 4) + " dB su 9 frequenze x Mid/Side)");
        }

        // --- 6. posizione nella catena
        {
            const double fs = 48000.0;
            const int total = 65536;

            // il passa-alto della banda 3 sta DOPO la banda 3: il filtro cambia il bilancio fondamentale/armoniche
            // della distorsione prodotta dalla banda, esattamente come la sua risposta in frequenza
            const double tone3 = fs * 205.0 / 32768.0;                      // ~300 Hz
            auto harmonics3 = [&] (float cutoff)
            {
                ValstMsEqProcessor p;
                configureBand (p, 2, (float) tone3, 1.0f, 0.0f, 0.0f, 100.0f);
                setHpf (p, 1, cutoff);
                return analyseHarmonics (renderMid (p, makeSine (tone3, 0.5, total, fs), fs), tone3, fs, 12);
            };

            const auto off = harmonics3 (hpfMinHz), on = harmonics3 (1000.0f);
            const auto filter = makeBand3HighPass (fs, 1000.0);
            const double expected = filter.magnitudeDb (3.0 * tone3, fs) - filter.magnitudeDb (tone3, fs);
            const double measured = on.db (3) - off.db (3);
            checkNear (measured, expected, 0.3, "banda 3: il passa-alto agisce dopo la saturazione (la 3a armonica sale di " + juce::String (measured, 1)
                                                    + " dB rispetto alla fondamentale, come prevede la risposta del filtro)");

            // il passa-alto della banda 1 sta PRIMA della banda 1: la banda riceve meno basso, quindi distorce meno
            const double tone1 = fs * 41.0 / 32768.0;                       // ~60 Hz
            auto harmonics1 = [&] (float cutoff)
            {
                ValstMsEqProcessor p;
                configureBand (p, 0, (float) tone1, 1.0f, 0.0f, 0.0f, 100.0f);
                setHpf (p, 0, cutoff);
                return analyseHarmonics (renderMid (p, makeSine (tone1, 0.5, total, fs), fs), tone1, fs, 12);
            };

            const auto thdOff = harmonics1 (hpfMinHz), thdOn = harmonics1 (400.0f);
            std::cout << "     banda 1 su un basso a 60 Hz: distorsione senza passa-alto " << juce::String (thdOff.thdPercent, 2) << " %, con passa-alto a 400 Hz "
                      << juce::String (thdOn.thdPercent, 3) << " %" << std::endl;
            check (thdOn.thdPercent < 0.5 * thdOff.thdPercent, "banda 1: il passa-alto agisce prima della saturazione (la banda distorce molto meno il basso filtrato)");
        }

        // --- 7. nessun click: accensione, spostamento del taglio, spegnimento
        {
            ValstMsEqProcessor p;
            preparePlayback (p, 48000.0, 256);
            const int block = 256, total = 48000 * 4;
            juce::AudioBuffer<float> buffer (2, block);
            juce::MidiBuffer midi;
            double last = 0.0, maxStep = 0.0;
            bool finite = true;

            for (int n = 0; n < total; n += block)
            {
                const int blockIndex = n / block;
                if (blockIndex == 188) setHpf (p, 0, 300.0f);                                         // accende il filtro 1
                if (blockIndex == 281) setHpf (p, 1, 800.0f);                                         // accende il filtro 3
                if (blockIndex == 375) { setHpf (p, 0, 60.0f); setHpf (p, 1, 120.0f); }               // salto del taglio
                if (blockIndex == 563) { setHpf (p, 0, hpfMinHz); setHpf (p, 1, hpfMinHz); }          // spegne entrambi

                for (int i = 0; i < block; ++i)
                {
                    const float x = (float) (0.25 * std::sin (twoPi * 180.0 * (double) (n + i) / 48000.0));
                    buffer.setSample (0, i, x);
                    buffer.setSample (1, i, x);
                }

                p.processBlock (buffer, midi);

                for (int i = 0; i < block; ++i)
                {
                    const double y = buffer.getSample (0, i);
                    finite = finite && std::isfinite (y);
                    if (n > 0 || i > 0)
                        maxStep = juce::jmax (maxStep, std::abs (y - last));
                    last = y;
                }
            }

            check (finite, "accendendo, spostando e spegnendo i passa-alto l'uscita e' sempre finita");
            check (maxStep < 0.03, "nessun click: salto massimo " + juce::String (maxStep, 4) + " (la sinusoide da sola ha " + juce::String (0.25 * twoPi * 180.0 / 48000.0, 4) + ")");
        }
    }

    //==========================================================================
    //  FASE 7: verifica complessiva, tutti gli stadi insieme
    //==========================================================================
    void configureEverything (juce::AudioProcessor& p, float inDb, float outDb, float sat, bool extreme)
    {
        setInGain (p, inDb);
        setOutGain (p, outDb);
        configureBand (p, 0,   90.0f, 1.2f, extreme ?  18.0f :  4.0f, extreme ? -100.0f : -30.0f, sat);
        configureBand (p, 1, 1800.0f, 0.6f, extreme ? -18.0f : -3.0f, extreme ?   60.0f :  20.0f, sat);
        configureBand (p, 2, 9000.0f, 2.5f, extreme ?  18.0f :  5.0f, extreme ?  100.0f :   0.0f, sat);
        setHpf (p, 0, extreme ? 400.0f : 60.0f);
        setHpf (p, 1, extreme ? 1000.0f : 120.0f);
    }

    juce::MemoryBlock makeV10State (ValstMsEqProcessor& source)
    {
        juce::MemoryBlock block;
        source.getStateInformation (block);

        auto xml = XmlBlob::getXmlFromBinary (block.getData(), (int) block.getSize());
        jassert (xml != nullptr);

        // come la 1.0: solo i 12 parametri originali, nessun seed
        const juce::StringArray added { "in_gain", "out_gain", "hpf1_freq", "hpf3_freq", "b1_sat", "b2_sat", "b3_sat" };
        for (auto* child = xml->getFirstChildElement(); child != nullptr;)
        {
            auto* next = child->getNextElement();
            if (child->hasTagName ("PARAM") && added.contains (child->getStringAttribute ("id")))
                xml->removeChildElement (child, true);
            child = next;
        }
        for (const char* attribute : { "satAlgo", "satSeed1", "satSeed2", "satSeed3" })
            xml->removeAttribute (attribute);

        juce::MemoryBlock out;
        XmlBlob::copyXmlToBinary (*xml, out);
        return out;
    }

    void testPhase7Integration()
    {
        section ("F7  Verifica complessiva: tutti gli stadi attivi insieme");

        const double fs = 48000.0;
        const double f0 = fs * 683.0 / 32768.0;                 // ~1000.4 Hz

        // --- 1. relazione prevedibile tra manopole e livello (zona lineare, -60 dBFS)
        {
            std::cout << "     guadagno netto a -60 dBFS (Input Gain -> dB):";
            bool allGood = true;
            for (float inDb : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 24.0f })
            {
                ValstMsEqProcessor p;
                setInGain (p, inDb);
                configureBand (p, 1, (float) f0, 1.0f, 6.0f, 0.0f, 100.0f);          // +6 dB al centro, con saturazione al massimo
                setOutGain (p, -3.0f);
                const auto y = renderMid (p, makeSine (f0, 0.001, 65536, fs), fs);
                const double gain = dbOf (toneAmplitude (y, 32768, 32768, f0, fs) / 0.001);
                const double expected = (inDb > 0.0f ? 0.5 * inDb : (double) inDb) + 6.0 - 3.0;      // la valvola compensa la meta' dei dB aggiunti
                std::cout << "  " << (int) inDb << " -> " << juce::String (gain, 2) << " (atteso " << juce::String (expected, 2) << ")";
                allGood = allGood && std::abs (gain - expected) < 0.05;
            }
            std::cout << std::endl;
            check (allGood, "a livello basso: gain netto = ingresso (meta' dei dB in salita) + gain della banda + uscita, senza sorprese");
        }

        // --- 2. Mid puro in ingresso: nessun Side inventato dagli stadi (L = R esatti), con tutto attivo
        {
            ValstMsEqProcessor p;
            configureEverything (p, 9.0f, 3.0f, 80.0f, false);
            const auto in = makeNoise (48000, 0.15, 5);
            Signal l, r;
            renderStereo (p, in, in, l, r, fs);
            check (maxDiffOf (l, r) == 0.0, "con tutti gli stadi attivi un segnale mono resta mono: L e R identici bit per bit");
        }

        // --- 3. il layout mono coincide con il canale Mid dello stereo
        {
            ValstMsEqProcessor stereo, mono;
            configureEverything (stereo, 9.0f, 3.0f, 80.0f, false);
            configureEverything (mono,   9.0f, 3.0f, 80.0f, false);
            const auto in = makeNoise (48000, 0.15, 6);
            const auto a = renderMid (stereo, in, fs);
            const auto b = renderMono (mono, in, fs);
            check (maxDiffOf (a, b) < 1.0e-6, "layout mono = Mid dello stereo con tutti gli stadi attivi (diff max " + juce::String (maxDiffOf (a, b), 9) + ")");
        }

        // --- 4. estremi: tutto al massimo, segnali al limite
        {
            struct Stimulus { const char* name; Signal signal; };
            const int total = 96000;
            Signal square (total), impulses (total, 0.0f);
            for (int i = 0; i < total; ++i)
            {
                square[(size_t) i] = ((i / 120) % 2 == 0) ? 1.0f : -1.0f;               // 200 Hz, fondo scala
                if (i % 1000 == 0) impulses[(size_t) i] = 1.0f;
            }

            const Stimulus stimuli[] = {
                { "seno 1 kHz a 0 dBFS", makeSine (f0, 1.0, total, fs) },
                { "seno 40 Hz a 0 dBFS", makeSine (40.0, 1.0, total, fs) },
                { "rumore a -6 dB RMS",  makeNoise (total, linOf (-6.0), 9) },
                { "onda quadra 200 Hz",  square },
                { "impulsi a fondo scala", impulses },
            };

            bool finite = true;
            double peak = 0.0, dc = 0.0;
            std::cout << "     estremi (Input +24, Output +12, SAT 100 %, bande +-18 dB, passa-alto 400 / 1000 Hz), picco per segnale:";
            for (const auto& s : stimuli)
            {
                ValstMsEqProcessor p;
                configureEverything (p, 24.0f, 12.0f, 100.0f, true);
                Signal l, r;
                renderStereo (p, s.signal, s.signal, l, r, fs);

                for (float v : l)
                    finite = finite && std::isfinite (v);
                const double thisPeak = peakOf (l, 0, total);
                std::cout << "  " << s.name << " " << juce::String (thisPeak, 2) << ";";
                peak = juce::jmax (peak, thisPeak);
                dc = juce::jmax (dc, std::abs (meanOf (l, total / 2, total / 2)));
            }

            std::cout << " DC massimo " << juce::String (dc, 5) << std::endl;
            check (finite, "tutto al massimo, 5 segnali al limite: uscita sempre finita");
            check (peak < 2.5, "uscita limitata: la saturazione non esplode (picco " + juce::String (peak, 2) + ")");
            check (dc < 0.02, "nessun offset DC apprezzabile (" + juce::String (dc, 5) + ")");
        }

        // --- 5. dimensione del blocco: stesso audio con tutti gli stadi attivi
        {
            const auto in = makeNoise (9600, 0.2, 77);
            auto run = [&] (int block)
            {
                ValstMsEqProcessor p;
                configureEverything (p, 10.0f, 4.0f, 70.0f, false);
                return renderMid (p, in, fs, block);
            };

            const auto reference = run (512);
            for (int block : { 1, 7, 64, 480, 4096 })
            {
                const auto other = run (block);
                check (maxDiffOf (reference, other) < 1.0e-6, "tutti gli stadi, blocchi da " + juce::String (block) + ": identico (diff max "
                                                                  + juce::String (maxDiffOf (reference, other), 9) + ")");
            }
        }

        // --- 6. stato completo: 19 parametri + seed, riaprendo il "progetto" in un'istanza nuova
        {
            ValstMsEqProcessor a;
            configureEverything (a, 7.5f, -4.5f, 45.0f, false);
            configureBand (a, 1, 777.0f, 0.55f, 6.5f, 35.0f, 62.0f);
            a.setSeed (1, 0x0123456789ABCDEFULL);

            juce::MemoryBlock state;
            a.getStateInformation (state);

            ValstMsEqProcessor b;
            b.setStateInformation (state.getData(), (int) state.getSize());

            auto close = [] (float x, float y) { return std::abs (x - y) <= 1.0e-5f * juce::jmax (1.0f, std::abs (x)); };

            bool same = true;
            for (int band = 0; band < numBands; ++band)
                for (int k = 0; k < numBandParams; ++k)
                    same = same && close (a.getParam (band, (ParamKind) k), b.getParam (band, (ParamKind) k));
            for (int g = 0; g < numGlobalParams; ++g)
                same = same && close (a.getGlobal ((GlobalParam) g), b.getGlobal ((GlobalParam) g));

            check (same, "ripristino: i 19 parametri tornano identici (entro 1e-5 relativo: il valore passa da testo e dalla scala normalizzata 0..1)");
            check (b.getSeed (0) == a.getSeed (0) && b.getSeed (1) == 0x0123456789ABCDEFULL && b.getSeed (2) == a.getSeed (2),
                   "ripristino: i seed delle tre bande tornano identici (compreso uno personalizzato)");

            const auto in = makeNoise (48000, 0.2, 31);
            const auto ya = renderMid (a, in, fs), yb = renderMid (b, in, fs);
            check (maxDiffOf (ya, yb) < 1.0e-5, "progetto riaperto in un'istanza nuova: stesso audio (diff max " + juce::String (maxDiffOf (ya, yb), 9) + ", sotto -100 dBFS)");

            // lo stesso stato caricato due volte (due aperture del progetto) da' sempre lo stesso identico audio, bit per bit
            ValstMsEqProcessor b2;
            b2.setStateInformation (state.getData(), (int) state.getSize());
            const auto yb2 = renderMid (b2, in, fs);
            check (maxDiffOf (yb, yb2) == 0.0, "lo stesso progetto riaperto due volte: audio identico bit per bit");

            // le tre identita' timbriche non cambiano mai: stesso seed = stesso carattere
            juce::MemoryBlock again;
            b.getStateInformation (again);
            ValstMsEqProcessor c;
            c.setStateInformation (again.getData(), (int) again.getSize());
            check (c.getSeed (0) == a.getSeed (0) && c.getSeed (1) == 0x0123456789ABCDEFULL && c.getSeed (2) == a.getSeed (2),
                   "salvataggio e ripristino ripetuti: i seed restano identici");

            ValstMsEqProcessor fresh;
            bool neutral = fresh.getGlobal (kInGain) == 0.0f && fresh.getGlobal (kOutGain) == 0.0f
                        && fresh.getGlobal (kHpf1) == hpfMinHz && fresh.getGlobal (kHpf3) == hpfMinHz;
            for (int band = 0; band < numBands; ++band)
                neutral = neutral && fresh.getParam (band, kSat) == 0.0f && fresh.getSeed (band) == sat::defaultSeeds[band];
            check (neutral, "valori predefiniti neutri: gain 0 dB, SAT 0 %, passa-alto OFF, seed di fabbrica");
        }

        // --- 7. un preset della 1.0 (senza i parametri nuovi) si carica e suona come prima
        {
            ValstMsEqProcessor source;
            setBand (source, 0, 77.0f, 0.4f, -3.5f, -25.0f);
            setBand (source, 1, 1234.0f, 2.2f, 6.0f, 80.0f);
            setBand (source, 2, 15000.0f, 5.0f, 12.0f, -100.0f);
            const auto v10 = makeV10State (source);

            // un'istanza con TUTTI i parametri nuovi spostati: caricando il vecchio stato devono tornare neutri
            ValstMsEqProcessor target;
            configureEverything (target, 10.0f, 5.0f, 80.0f, true);
            target.setSeed (0, 0x1234ULL);
            target.setStateInformation (v10.getData(), (int) v10.getSize());

            bool oldOk = true;
            for (int band = 0; band < numBands; ++band)
                for (int k = 0; k < 4; ++k)
                    oldOk = oldOk && std::abs (target.getParam (band, (ParamKind) k) - source.getParam (band, (ParamKind) k)) < 1.0e-4f;

            bool newNeutral = target.getGlobal (kInGain) == 0.0f && target.getGlobal (kOutGain) == 0.0f
                           && target.getGlobal (kHpf1) == hpfMinHz && target.getGlobal (kHpf3) == hpfMinHz;
            for (int band = 0; band < numBands; ++band)
                newNeutral = newNeutral && target.getParam (band, kSat) == 0.0f && target.getSeed (band) == sat::defaultSeeds[band];

            check (oldOk, "preset 1.0: i 12 parametri originali vengono ripristinati");
            check (newNeutral, "preset 1.0: i parametri nuovi tornano ai valori neutri (anche se prima erano spostati) e i seed a quelli di fabbrica");

            ValstMsEqProcessor reference;
            for (int band = 0; band < numBands; ++band)
                setBand (reference, band, source.getParam (band, kFreq), source.getParam (band, kWidth), source.getParam (band, kGain), source.getParam (band, kMs));

            const auto in = makeNoise (48000, 0.2, 41);
            check (maxDiffOf (renderMid (target, in, fs), renderMid (reference, in, fs)) == 0.0, "preset 1.0: suona identico a un EQ con i soli 12 parametri originali");
        }

        // --- 8. nessuna allocazione e nessun blocco nel thread audio (stereo e mono, con automazioni)
        {
            auto countAllocations = [&] (bool mono)
            {
                ValstMsEqProcessor p;
                configureEverything (p, 12.0f, 3.0f, 70.0f, false);
                const int block = 256, channels = mono ? 1 : 2;
                p.setPlayConfigDetails (channels, channels, fs, block);
                p.prepareToPlay (fs, block);

                juce::AudioBuffer<float> buffer (channels, block);
                juce::MidiBuffer midi;
                std::mt19937 rng (5);
                std::uniform_real_distribution<float> noise (-0.5f, 0.5f);
                long total = 0;
                int firstBlock = -1;
                std::size_t firstSize = 0;

                for (int n = 0; n < 400; ++n)
                {
                    // automazione (fuori dal conteggio: avviene nel thread dei messaggi, non in quello audio)
                    if (n % 10 == 0)
                    {
                        setInGain (p, (float) (-12 + (n * 7) % 36));
                        setOutGain (p, (float) (-24 + (n * 5) % 36));
                        setSat (p, n % 3, (float) ((n * 13) % 101));
                        setHpf (p, n % 2, (float) (20 + (n * 11) % 300));
                        setBand (p, n % 3, (float) (50 + (n * 97) % 15000), 1.0f + (float) (n % 5) * 0.5f, (float) ((n % 37) - 18), (float) ((n % 201) - 100));
                    }

                    for (int c = 0; c < channels; ++c)
                        for (int i = 0; i < block; ++i)
                            buffer.setSample (c, i, noise (rng));

                    allocationProbe::count = 0;
                    allocationProbe::armed = true;
                    p.processBlock (buffer, midi);
                    allocationProbe::armed = false;

                    const long c = allocationProbe::count.load();
                    if (c > 0 && firstBlock < 0)
                    {
                        firstBlock = n;
                        firstSize = allocationProbe::lastSize.load();
                    }
                    total += c;
                }

                if (total > 0)
                    std::cout << "     (prima allocazione al blocco " << firstBlock << ", " << firstSize << " byte)" << std::endl;
                return total;
            };

            const long stereoAllocs = countAllocations (false), monoAllocs = countAllocations (true);
            check (stereoAllocs == 0, "processBlock stereo: " + juce::String ((int) stereoAllocs) + " allocazioni in 400 blocchi con automazione di tutti gli stadi");
            check (monoAllocs == 0, "processBlock mono: " + juce::String ((int) monoAllocs) + " allocazioni in 400 blocchi con automazione di tutti gli stadi");
        }

        // --- 9. automazione casuale di tutti i parametri: nessun NaN, nessuna esplosione
        {
            std::mt19937 rng (2024);
            auto uni = [&] (double lo, double hi) { return std::uniform_real_distribution<double> (lo, hi) (rng); };
            auto logUni = [&] (double lo, double hi) { return lo * std::pow (hi / lo, uni (0.0, 1.0)); };

            bool finite = true;
            double peak = 0.0;
            long blocks = 0;

            for (double rate : { 44100.0, 96000.0, 192000.0 })
            {
                ValstMsEqProcessor p;
                preparePlayback (p, rate, 512);
                juce::AudioBuffer<float> buffer (2, 512);
                juce::MidiBuffer midi;

                for (int n = 0; n < 500; ++n, ++blocks)
                {
                    setInGain (p, (float) uni (-12.0, 24.0));
                    setOutGain (p, (float) uni (-24.0, 12.0));
                    for (int b = 0; b < numBands; ++b)
                    {
                        setBand (p, b, (float) logUni (20.0, 20000.0), (float) logUni (0.1, 6.0), (float) uni (-18.0, 18.0), (float) uni (-100.0, 100.0));
                        setSat (p, b, (float) uni (0.0, 100.0));
                    }
                    setHpf (p, 0, (float) logUni (20.0, 400.0));
                    setHpf (p, 1, (float) logUni (20.0, 1000.0));

                    const int kind = n % 4;
                    const double level = uni (0.0, 1.5), freq = logUni (30.0, 15000.0);
                    for (int i = 0; i < 512; ++i)
                    {
                        const double t = (double) (n * 512 + i) / rate;
                        const float x = kind == 0 ? (float) (level * uni (-1.0, 1.0))
                                      : kind == 1 ? (float) (level * std::sin (twoPi * freq * t))
                                      : kind == 2 ? (float) (level * ((i / 64) % 2 == 0 ? 1.0 : -1.0))
                                                  : 0.0f;
                        buffer.setSample (0, i, x);
                        buffer.setSample (1, i, kind == 1 ? -x : x);
                    }

                    p.processBlock (buffer, midi);

                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < 512; ++i)
                        {
                            const float y = buffer.getSample (c, i);
                            finite = finite && std::isfinite (y);
                            peak = juce::jmax (peak, (double) std::abs (y));
                        }
                }
            }

            std::cout << "     automazione casuale: " << blocks << " blocchi, tutti i 19 parametri cambiati a ogni blocco, picco " << juce::String (peak, 2) << std::endl;
            check (finite, "automazione casuale di tutti i parametri (44.1 / 96 / 192 kHz): uscita sempre finita");
            check (peak < 40.0, "automazione casuale: l'uscita resta limitata (picco " + juce::String (peak, 2) + ")");
        }

        // --- 10. diverse frequenze di campionamento
        {
            double referenceThd = 0.0;
            for (double rate : { 48000.0, 44100.0, 88200.0, 96000.0, 176400.0, 192000.0 })
            {
                const juce::String label = "fs " + juce::String (rate, 0) + " Hz: ";

                // a) neutro = trasparente
                {
                    ValstMsEqProcessor p;
                    const auto in = makeNoise (24000, 0.3, 3);
                    check (maxDiffOf (renderMid (p, in, rate), in) < 1.0e-6, label + "con tutto neutro il segnale esce identico");
                }

                // b) campane + passa-alto = grafico
                {
                    ValstMsEqProcessor p;
                    flatten (p);
                    setBand (p, 0, 120.0f, 1.8f, -6.0f, -40.0f);
                    setBand (p, 1, 1800.0f, 0.7f, 9.0f, 60.0f);
                    setBand (p, 2, 9000.0f, 1.2f, 7.0f, 0.0f);
                    setHpf (p, 0, 90.0f);
                    setHpf (p, 1, 250.0f);

                    std::array<LinearResponse::BandSetting, (size_t) numBands> settings;
                    for (int b = 0; b < numBands; ++b)
                        settings[(size_t) b] = { p.getParam (b, kFreq), p.getParam (b, kWidth), p.getParam (b, kGain), p.getParam (b, kMs) };
                    LinearResponse model;
                    model.setup (rate, settings, p.getGlobal (kHpf1), p.getGlobal (kHpf3));

                    double worst = 0.0;
                    for (double f : { 60.0, 250.0, 1800.0, 9000.0 })
                        worst = juce::jmax (worst, std::abs (measureGainDb (p, f, false, rate) - model.totalDb (false, f)));
                    check (worst < 0.06, label + "grafico = audio (scarto max " + juce::String (worst, 4) + " dB)");
                }

                // c) il trasformatore e' trasparente sotto 0 dBFS
                {
                    ValstMsEqProcessor p;
                    const auto in = makeSine (1000.0, 0.95, 32768, rate);
                    const auto y = renderMid (p, in, rate);
                    double worst = 0.0;
                    for (size_t i = 4096; i < y.size(); ++i)
                        worst = juce::jmax (worst, (double) std::abs (y[i] - in[i]));
                    check (worst < 1.0e-6, label + "trasformatore trasparente sotto 0 dBFS (diff max " + juce::String (worst, 9) + ")");
                }

                // d) la saturazione di banda ha lo stesso carattere a ogni frequenza di campionamento
                {
                    ValstMsEqProcessor p;
                    configureBand (p, 1, 1000.0f, 1.0f, 0.0f, 0.0f, 100.0f);
                    const auto y = renderMid (p, makeSine (1000.0, 0.1, 65536, rate), rate);
                    const double thd = analyseHarmonics (y, 1000.0, rate, 12).thdPercent;

                    if (rate == 48000.0)
                        referenceThd = thd;

                    const double diff = dbOf (thd / referenceThd);
                    check (std::abs (diff) < 1.5, label + "distorsione di banda " + juce::String (thd, 2) + " % (" + juce::String (diff, 2) + " dB rispetto a 48 kHz)");
                }
            }
        }

        // --- 11. costo di calcolo
        {
            ValstMsEqProcessor p;
            configureEverything (p, 12.0f, 3.0f, 100.0f, false);
            preparePlayback (p, fs, 512);

            const auto in = makeNoise (512, 0.2, 8);
            juce::AudioBuffer<float> buffer (2, 512);
            juce::MidiBuffer midi;
            const int blocks = (int) (fs * 10.0 / 512.0);              // 10 secondi di audio stereo

            auto timeIt = [&] (juce::AudioProcessor& proc)
            {
                const auto t0 = std::chrono::steady_clock::now();
                for (int n = 0; n < blocks; ++n)
                {
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < 512; ++i)
                            buffer.setSample (c, i, in[(size_t) i]);
                    proc.processBlock (buffer, midi);
                }
                return std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            };

            ValstMsEqProcessor neutral;
            preparePlayback (neutral, fs, 512);
            const double tNeutral = timeIt (neutral), tFull = timeIt (p);

            std::cout << "     10 s di audio stereo a 48 kHz: EQ come la 1.0 " << juce::String (tNeutral * 1000.0, 1) << " ms, tutti gli stadi attivi "
                      << juce::String (tFull * 1000.0, 1) << " ms  (" << juce::String (10.0 / tFull, 0) << "x il tempo reale, "
                      << juce::String (100.0 * tFull / 10.0, 2) << " % di un core)" << std::endl;
            check (tFull < 10.0 / 15.0, "con tutti gli stadi attivi il calcolo e' almeno 15 volte piu' veloce del tempo reale");
        }
    }

    //==========================================================================
    void testMapping()
    {
        section ("Mapping Mid/Side e fader logaritmico");

        auto g = computeBandGains (12.0, 0.0);
        checkNear (g.midDb, 12.0, 1e-9, "fader 0   -> Mid  = +12 dB (stereo, neutro)");
        checkNear (g.sideDb, 12.0, 1e-9, "fader 0   -> Side = +12 dB (stereo, neutro)");

        g = computeBandGains (12.0, 100.0);
        checkNear (g.midDb, 0.0, 1e-9, "fader +100 -> Mid  = 0 dB (invariato)");
        checkNear (g.sideDb, 12.0, 1e-9, "fader +100 -> Side = +12 dB");

        g = computeBandGains (12.0, -100.0);
        checkNear (g.midDb, 12.0, 1e-9, "fader -100 -> Mid  = +12 dB");
        checkNear (g.sideDb, 0.0, 1e-9, "fader -100 -> Side = 0 dB (invariato)");

        g = computeBandGains (12.0, 50.0);
        checkNear (g.midDb, 6.0, 1e-9, "fader +50  -> Mid  = +6 dB");
        checkNear (g.sideDb, 12.0, 1e-9, "fader +50  -> Side = +12 dB");

        g = computeBandGains (-6.0, -50.0);
        checkNear (g.midDb, -6.0, 1e-9, "gain -6, fader -50 -> Mid  = -6 dB");
        checkNear (g.sideDb, -3.0, 1e-9, "gain -6, fader -50 -> Side = -3 dB");

        const auto ms = msRange();
        checkNear (ms.convertFrom0to1 (0.0f), -100.0, 1e-3, "posizione 0   -> -100");
        checkNear (ms.convertFrom0to1 (0.5f), 0.0, 1e-3,    "posizione 0.5 -> 0 (centro)");
        checkNear (ms.convertFrom0to1 (1.0f), 100.0, 1e-3,  "posizione 1   -> +100");
        checkNear (ms.convertFrom0to1 (0.75f), 24.03, 0.05, "posizione 0.75 -> circa 24 (legge logaritmica: la meta' superiore della corsa copre 0..100 non linearmente)");
        checkNear (ms.convertFrom0to1 (0.25f), -24.03, 0.05, "simmetrico sotto lo zero");

        bool monotonic = true, roundTrip = true;
        float previous = -1000.0f;
        for (int i = 0; i <= 1000; ++i)
        {
            const float p = (float) i / 1000.0f;
            const float v = ms.convertFrom0to1 (p);
            monotonic = monotonic && v > previous;
            roundTrip = roundTrip && std::abs (ms.convertTo0to1 (v) - p) < 1e-4f;
            previous = v;
        }
        check (monotonic, "il fader e' strettamente monotono lungo tutta la corsa");
        check (roundTrip, "posizione -> valore -> posizione ritorna al punto di partenza");

        checkNear (ms.snapToLegalValue (0.4f), 0.0, 1e-9, "0.4 si aggancia a 0");
        checkNear (ms.snapToLegalValue (-0.4f), 0.0, 1e-9, "-0.4 si aggancia a 0");
        checkNear (ms.snapToLegalValue (37.6f), 38.0, 1e-9, "37.6 -> 38 (valori interi)");

        const auto fr = logRange (freqMin, freqMax);
        checkNear (fr.convertFrom0to1 (0.5f), std::sqrt (20.0 * 20000.0), 0.01, "FREQ: il centro della manopola e' 632 Hz (scala log)");
        checkNear (gainRange().convertFrom0to1 (0.5f), 0.0, 1e-4, "GAIN: il centro della manopola e' 0 dB");

        check (msToText (0.0f) == "0" && msToText (37.0f) == "+37 S" && msToText (-100.0f) == "-100 M", "testo del fader: 0, +37 S, -100 M");
        check (freqToText (999.0f) == "999 Hz" && freqToText (1500.0f) == "1.50 kHz" && freqToText (12000.0f) == "12.0 kHz", "testo FREQ");
        checkNear (textToFreq ("1.5k"), 1500.0, 1e-3, "FREQ si puo' digitare come '1.5k'");
    }

    //==========================================================================
    void testFlatPassThrough()
    {
        section ("Trasparenza (EQ piatto)");

        ValstMsEqProcessor p;
        p.setRateAndBufferSizeDetails (48000.0, 512);
        p.prepareToPlay (48000.0, 512);

        std::mt19937 rng (1234);
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);

        juce::AudioBuffer<float> buffer (2, 512), reference (2, 512);
        juce::MidiBuffer midi;
        float maxDiff = 0.0f;

        for (int block = 0; block < 10; ++block)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (c, i, dist (rng));
            reference.makeCopyOf (buffer);

            p.processBlock (buffer, midi);

            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    maxDiff = juce::jmax (maxDiff, std::abs (buffer.getSample (c, i) - reference.getSample (c, i)));
        }

        check (maxDiff < 1.0e-6f, "con tutti i gain a 0 il segnale esce identico (max diff " + juce::String (maxDiff, 9) + ")");
    }

    //==========================================================================
    void testMidSideGains()
    {
        section ("DSP: il GAIN lavora su Mid, Side o entrambi a seconda del fader");

        struct Case { float gain, ms, expectMid, expectSide; const char* name; };
        const Case cases[] = {
            { 12.0f,    0.0f, 12.0f, 12.0f, "fader 0:    +12 dB su Mid e Side (stereo)" },
            { 12.0f,  100.0f,  0.0f, 12.0f, "fader +100: +12 dB solo sul Side" },
            { 12.0f, -100.0f, 12.0f,  0.0f, "fader -100: +12 dB solo sul Mid" },
            { 12.0f,   50.0f,  6.0f, 12.0f, "fader +50:  Side +12, Mid +6" },
            { -9.0f,  -50.0f, -9.0f, -4.5f, "fader -50:  gain -9 -> Mid -9, Side -4.5" },
            { -9.0f,   20.0f, -7.2f, -9.0f, "fader +20:  gain -9 -> Side -9, Mid -7.2" },
        };

        for (const auto& c : cases)
        {
            ValstMsEqProcessor p;
            flatten (p);
            setBand (p, 1, 1000.0f, 1.0f, c.gain, c.ms);

            checkNear (measureGainDb (p, 1000.0, false), c.expectMid,  0.05, juce::String (c.name) + " [Mid]");
            checkNear (measureGainDb (p, 1000.0, true),  c.expectSide, 0.05, juce::String (c.name) + " [Side]");
        }

        // una banda sul Mid non deve toccare il Side nemmeno lontano dalla frequenza centrale
        ValstMsEqProcessor p;
        flatten (p);
        setBand (p, 1, 1000.0f, 1.0f, 12.0f, -100.0f);
        checkNear (measureGainDb (p, 250.0, true), 0.0, 0.01, "banda solo-Mid: il Side a 250 Hz resta a 0 dB");
        checkNear (measureGainDb (p, 4000.0, true), 0.0, 0.01, "banda solo-Mid: il Side a 4 kHz resta a 0 dB");
    }

    //==========================================================================
    void testGraphMatchesAudio()
    {
        section ("DSP: il grafico mostra esattamente la risposta dell'audio");

        for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
        {
            ValstMsEqProcessor p;
            flatten (p);
            setBand (p, 0,   120.0f, 1.8f,  -6.0f, -40.0f);
            setBand (p, 1,  1800.0f, 0.7f,   9.0f,  60.0f);
            setBand (p, 2,  9000.0f, 1.2f,   7.0f,   0.0f);

            Biquad mid[3], side[3];
            for (int b = 0; b < 3; ++b)
            {
                const auto g = computeBandGains (p.getParam (b, kGain), p.getParam (b, kMs));
                mid[b].setPeaking  (sampleRate, p.getParam (b, kFreq), p.getParam (b, kWidth), g.midDb);
                side[b].setPeaking (sampleRate, p.getParam (b, kFreq), p.getParam (b, kWidth), g.sideDb);
            }

            double worst = 0.0;
            for (double f : { 40.0, 120.0, 400.0, 1800.0, 5000.0, 9000.0, 15000.0 })
            {
                double expectedMid = 0.0, expectedSide = 0.0;
                for (int b = 0; b < 3; ++b)
                {
                    expectedMid  += mid[b].magnitudeDb (f, sampleRate);
                    expectedSide += side[b].magnitudeDb (f, sampleRate);
                }

                worst = juce::jmax (worst, std::abs (measureGainDb (p, f, false, sampleRate) - expectedMid));
                worst = juce::jmax (worst, std::abs (measureGainDb (p, f, true,  sampleRate) - expectedSide));
            }

            check (worst < 0.05, "fs " + juce::String (sampleRate, 0) + " Hz: scarto massimo grafico/audio "
                                     + juce::String (worst, 4) + " dB su 7 frequenze x (Mid, Side)");
        }
    }

    void testBellShape()
    {
        section ("DSP: forma e larghezza della campana");

        Biquad bell;
        bell.setPeaking (48000.0, 1000.0, 1.0, 12.0);

        checkNear (bell.magnitudeDb (1000.0, 48000.0), 12.0, 0.01, "centro della campana = +12 dB");
        checkNear (bell.magnitudeDb (1000.0 * std::sqrt (2.0), 48000.0), 6.0, 0.3, "larghezza 1 ottava: a mezzo guadagno (+6 dB) a f0 * 2^0.5");
        checkNear (bell.magnitudeDb (1000.0 / std::sqrt (2.0), 48000.0), 6.0, 0.3, "larghezza 1 ottava: a mezzo guadagno (+6 dB) a f0 / 2^0.5");
        checkNear (bell.magnitudeDb (20.0, 48000.0), 0.0, 0.2, "lontano dalla campana (20 Hz) il guadagno e' ~0 dB");

        Biquad narrow, wide;
        narrow.setPeaking (48000.0, 1000.0, 0.2, 12.0);
        wide.setPeaking   (48000.0, 1000.0, 4.0, 12.0);
        check (narrow.magnitudeDb (2000.0, 48000.0) < 1.0, "WIDTH 0.2 oct: a un'ottava di distanza quasi nessun effetto");
        check (wide.magnitudeDb (2000.0, 48000.0) > 8.0, "WIDTH 4 oct: a un'ottava di distanza ancora forte");

        Biquad cut;
        cut.setPeaking (48000.0, 1000.0, 1.0, -12.0);
        checkNear (bell.magnitudeDb (1700.0, 48000.0) + cut.magnitudeDb (1700.0, 48000.0), 0.0, 1e-9,
                   "taglio e boost con lo stesso valore si annullano esattamente (campana simmetrica)");

        Biquad extreme;
        extreme.setPeaking (44100.0, 20000.0, 6.0, 18.0);
        check (std::isfinite (extreme.magnitudeDb (20000.0, 44100.0)) && std::isfinite (extreme.b0) && std::isfinite (extreme.a2),
               "caso estremo (20 kHz, 6 ottave, +18 dB a 44.1 kHz): coefficienti finiti");
    }

    //==========================================================================
    void testBlockSizes()
    {
        section ("DSP: risultato indipendente dalla dimensione del blocco");

        std::mt19937 rng (99);
        std::uniform_real_distribution<float> dist (-0.4f, 0.4f);
        const int total = 9600;
        std::vector<float> inL ((size_t) total), inR ((size_t) total);
        for (int i = 0; i < total; ++i) { inL[(size_t) i] = dist (rng); inR[(size_t) i] = dist (rng); }

        auto run = [&] (int block)
        {
            ValstMsEqProcessor p;
            setBand (p, 0, 90.0f, 1.5f, 8.0f, -70.0f);
            setBand (p, 1, 2500.0f, 0.5f, -10.0f, 35.0f);
            setBand (p, 2, 11000.0f, 2.0f, 5.0f, 0.0f);
            p.setRateAndBufferSizeDetails (48000.0, block);
            p.prepareToPlay (48000.0, block);

            std::vector<float> out;
            juce::AudioBuffer<float> buffer (2, block);
            juce::MidiBuffer midi;
            for (int n = 0; n < total; n += block)
            {
                const int num = juce::jmin (block, total - n);
                for (int i = 0; i < num; ++i)
                {
                    buffer.setSample (0, i, inL[(size_t) (n + i)]);
                    buffer.setSample (1, i, inR[(size_t) (n + i)]);
                }
                juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, num);
                p.processBlock (view, midi);
                for (int i = 0; i < num; ++i) { out.push_back (buffer.getSample (0, i)); out.push_back (buffer.getSample (1, i)); }
            }
            return out;
        };

        const auto reference = run (512);
        for (int block : { 1, 7, 64, 480, 4096 })
        {
            const auto other = run (block);
            float maxDiff = 0.0f;
            for (size_t i = 0; i < reference.size(); ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (reference[i] - other[i]));
            check (other.size() == reference.size() && maxDiff < 1.0e-6f,
                   "blocchi da " + juce::String (block) + " campioni: identico al riferimento (max diff " + juce::String (maxDiff, 9) + ")");
        }
    }

    void testNoClicks()
    {
        section ("DSP: nessun click quando i parametri cambiano di colpo");

        ValstMsEqProcessor p;
        flatten (p);
        p.setRateAndBufferSizeDetails (48000.0, 256);
        p.prepareToPlay (48000.0, 256);

        const int block = 256, total = 48000 * 3;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        float last = 0.0f, maxStep = 0.0f;
        bool finite = true;

        for (int n = 0; n < total; n += block)
        {
            if (n == 48000)  setBand (p, 1, 1000.0f, 1.0f, 18.0f, 100.0f);      // salto di 18 dB, tutto sul Side
            if (n == 96000)  setBand (p, 1, 4000.0f, 0.3f, -18.0f, -100.0f);    // salto di frequenza, larghezza, gain e canale

            for (int i = 0; i < block; ++i)
            {
                const float x = (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) (n + i) / 48000.0));
                buffer.setSample (0, i, x);
                buffer.setSample (1, i, x);        // segnale mono: Side = 0, il Mid cambia solo dopo il 2o salto
            }

            p.processBlock (buffer, midi);

            for (int i = 0; i < block; ++i)
            {
                const float y = buffer.getSample (0, i);
                finite = finite && std::isfinite (y);
                if (n > 0 || i > 0)
                    maxStep = juce::jmax (maxStep, std::abs (y - last));
                last = y;
            }
        }

        check (finite, "uscita sempre finita");
        check (maxStep < 0.2f, "salto massimo tra due campioni consecutivi = " + juce::String (maxStep, 4)
                                   + " (una sinusoide a 1 kHz di ampiezza 0.25 ha al massimo 0.033)");
    }

    void testState()
    {
        section ("Salvataggio e ripristino dello stato");

        ValstMsEqProcessor a, b;
        setBand (a, 0, 77.0f, 0.4f, -3.5f, -25.0f);
        setBand (a, 1, 1234.0f, 2.2f, 6.0f, 80.0f);
        setBand (a, 2, 15000.0f, 5.0f, 18.0f, -100.0f);

        juce::MemoryBlock state;
        a.getStateInformation (state);
        b.setStateInformation (state.getData(), (int) state.getSize());

        bool same = true;
        for (int band = 0; band < numBands; ++band)
            for (int k = 0; k < 4; ++k)
                same = same && std::abs (a.getParam (band, (ParamKind) k) - b.getParam (band, (ParamKind) k)) < 1.0e-4f;

        check (state.getSize() > 0, "lo stato non e' vuoto (" + juce::String ((int) state.getSize()) + " byte)");
        check (same, "i 12 parametri della versione 1.0 tornano identici dopo il ripristino");

        ValstMsEqProcessor c;
        c.setStateInformation ("garbage", 7);
        check (true, "uno stato corrotto viene ignorato senza crash");
    }

    //==========================================================================
    void testHostedVst3 (const juce::File& vst3)
    {
        section ("Il file VST3 compilato, caricato come farebbe un DAW");

        check (vst3.exists(), "il file esiste: " + vst3.getFullPathName());
        if (! vst3.exists())
            return;

        juce::AudioPluginFormatManager manager;
        manager.addFormat (std::make_unique<juce::VST3PluginFormat>());

        juce::OwnedArray<juce::PluginDescription> found;
        manager.getFormat (0)->findAllTypesForFile (found, vst3.getFullPathName());
        check (found.size() == 1, "la scansione trova esattamente 1 plugin (trovati " + juce::String (found.size()) + ")");
        if (found.isEmpty())
            return;

        const auto& d = *found[0];
        std::cout << "     nome: " << d.name << " | produttore: " << d.manufacturerName << " | categoria: " << d.category
                  << " | ingressi/uscite: " << d.numInputChannels << "/" << d.numOutputChannels << std::endl;
        check (d.name == "ValST MS EQ", "nome del plugin = 'ValST MS EQ'");
        check (! d.isInstrument, "e' un effetto, non uno strumento");
        check (d.category.containsIgnoreCase ("EQ"), "categoria VST3 contiene 'EQ' (" + d.category + ")");

        juce::String error;
        auto instance = manager.createPluginInstance (d, 48000.0, 512, error);
        check (instance != nullptr, "istanza creata" + (error.isEmpty() ? juce::String() : " - errore: " + error));
        if (instance == nullptr)
            return;

        instance->setPlayConfigDetails (2, 2, 48000.0, 512);
        instance->prepareToPlay (48000.0, 512);

        check (instance->getTotalNumInputChannels() == 2 && instance->getTotalNumOutputChannels() == 2, "e' un effetto stereo 2 in / 2 out");

        // 19 parametri del plugin (12 della 1.0 + 7 della 1.1) + il parametro "Bypass" che il wrapper VST3 di JUCE aggiunge per i DAW
        int pluginParams = 0, bypassParams = 0;
        for (auto* param : instance->getParameters())
        {
            std::cout << "     - " << param->getName (64) << "  [" << param->getText (param->getDefaultValue(), 64) << "]" << std::endl;
            if (param->getName (64) == "Bypass") ++bypassParams; else ++pluginParams;
        }
        check (pluginParams == 19 && bypassParams == 1, "espone i 19 parametri (" + juce::String (pluginParams) + ") piu' il Bypass dell'host (" + juce::String (bypassParams) + ")");

        // imposta come farebbe l'host (valori normalizzati 0..1)
        auto setNormalised = [&] (const juce::String& name, float v)
        {
            for (auto* param : instance->getParameters())
                if (param->getName (64) == name)
                {
                    param->setValue (v);
                    return true;
                }
            return false;
        };

        check (setNormalised ("Band 2 Freq", (float) (std::log (1000.0 / 20.0) / std::log (1000.0))), "parametro 'Band 2 Freq' trovato e impostato a 1 kHz");
        check (setNormalised ("Band 2 Width", (float) (std::log (1.0 / 0.1) / std::log (60.0))), "parametro 'Band 2 Width' impostato a 1 ottava");
        check (setNormalised ("Band 2 Gain", (12.0f + 18.0f) / 36.0f), "parametro 'Band 2 Gain' impostato a +12 dB");
        check (setNormalised ("Band 2 Mid/Side", 0.0f), "parametro 'Band 2 Mid/Side' impostato a -100 (solo Mid)");

        const double mid  = measureGainDb (*instance, 1000.0, false, 48000.0, 512, false);
        const double side = measureGainDb (*instance, 1000.0, true,  48000.0, 512, false);
        checkNear (mid,  12.0, 0.1, "VST3 caricato: Mid  a 1 kHz = +12 dB");
        checkNear (side,  0.0, 0.1, "VST3 caricato: Side a 1 kHz = 0 dB");

        // gli stadi nuovi (valvola, saturazione di banda, passa-alto, trasformatore) nel file VST3 vero:
        // stesso audio del codice in-process con gli stessi valori impostati dall'host
        {
            // Sul processore in-process il valore va impostato con setValueNotifyingHost: e' cosi' che l'APVTS (da cui
            // legge il motore) viene aggiornato, come fa il wrapper VST3 quando riceve un cambio dall'host. Con il solo
            // setValue() il motore continuerebbe a vedere i valori vecchi. Il plugin caricato come host usa invece
            // l'API dell'host (setValue sul parametro dell'istanza).
            auto setByName = [] (juce::AudioProcessor& proc, const juce::String& name, float normalised, bool notify)
            {
                for (auto* param : proc.getParameters())
                    if (param->getName (64) == name)
                    {
                        if (notify) param->setValueNotifyingHost (normalised);
                        else        param->setValue (normalised);
                        return true;
                    }
                return false;
            };

            ValstMsEqProcessor local;
            bool allFound = true;
            auto setBoth = [&] (const juce::String& name, float v)
            {
                const bool a = setByName (*instance, name, v, false), b = setByName (local, name, v, true);
                allFound = allFound && a && b;
            };

            // gli stadi vengono attivati uno alla volta (cumulativi) e a ogni passo l'audio del file VST3 caricato
            // deve coincidere con quello del codice in-process: se uno stadio si comportasse in modo diverso nel
            // binario vero (ottimizzazioni, ordine di inizializzazione, gestione dei parametri) lo si vedrebbe qui
            {
                const auto probe = makeNoise (96000, 0.2, 17);
                auto step = [&] (const char* label)
                {
                    const auto h = renderMid (*instance, probe, 48000.0, 512);
                    const auto l = renderMid (local, probe, 48000.0, 512);
                    const double d = maxDiffOf (h, l, 48000);
                    std::cout << "     " << label << ": RMS VST3 " << juce::String (dbOf (rmsOf (h, 48000, 48000)), 2)
                              << " dBFS, in-process " << juce::String (dbOf (rmsOf (l, 48000, 48000)), 2)
                              << " dBFS, scarto massimo " << juce::String (d, 9) << std::endl;
                    check (d < 1.0e-6, juce::String ("VST3 caricato = codice in-process: ") + label);
                };

                setBoth ("Band 2 Freq", (float) (std::log (1000.0 / 20.0) / std::log (1000.0)));
                setBoth ("Band 2 Width", (float) (std::log (1.0 / 0.1) / std::log (60.0)));
                setBoth ("Band 2 Gain", (12.0f + 18.0f) / 36.0f);
                setBoth ("Band 2 Mid/Side", 0.0f);
                step ("banda 2 +12 dB Mid");
                setBoth ("Input Gain", (9.0f + 12.0f) / 36.0f);                  step ("+ Input Gain +9 dB");
                setBoth ("Output Gain", (3.0f + 24.0f) / 36.0f);                 step ("+ Output Gain +3 dB");
                setBoth ("Band 1 Saturation", 0.6f);                             step ("+ SAT banda 1 60 %");
                setBoth ("Band 2 Saturation", 0.4f);                             step ("+ SAT banda 2 40 %");
                setBoth ("Band 3 Saturation", 0.9f);                             step ("+ SAT banda 3 90 %");
                setBoth ("Band 1 Low Cut", (float) (std::log (80.0 / 20.0) / std::log (400.0 / 20.0)));    step ("+ LOW CUT 1 80 Hz");
                setBoth ("Band 3 Low Cut", (float) (std::log (250.0 / 20.0) / std::log (1000.0 / 20.0)));  step ("+ LOW CUT 3 250 Hz");
            }

            setBoth ("Band 2 Freq", (float) (std::log (1000.0 / 20.0) / std::log (1000.0)));
            setBoth ("Band 2 Width", (float) (std::log (1.0 / 0.1) / std::log (60.0)));
            setBoth ("Band 2 Gain", (12.0f + 18.0f) / 36.0f);
            setBoth ("Band 2 Mid/Side", 0.0f);
            setBoth ("Input Gain", (9.0f + 12.0f) / 36.0f);                       // +9 dB
            setBoth ("Output Gain", (3.0f + 24.0f) / 36.0f);                      // +3 dB
            setBoth ("Band 1 Saturation", 0.6f);
            setBoth ("Band 2 Saturation", 0.4f);
            setBoth ("Band 3 Saturation", 0.9f);
            setBoth ("Band 1 Low Cut", (float) (std::log (80.0 / 20.0) / std::log (400.0 / 20.0)));
            setBoth ("Band 3 Low Cut", (float) (std::log (250.0 / 20.0) / std::log (1000.0 / 20.0)));
            check (allFound, "tutti i parametri nuovi sono raggiungibili per nome dall'host");

            const auto in = makeNoise (96000, 0.2, 17);
            const auto hosted    = renderMid (*instance, in, 48000.0, 512);
            const auto inProcess = renderMid (local, in, 48000.0, 512);

            const double diff = maxDiffOf (hosted, inProcess, 48000);            // dopo 1 s: le rampe di 30 ms sono finite
            std::cout << "     VST3 caricato con IN +9, OUT +3, SAT 60/40/90 %, LOW CUT 80/250 Hz: differenza massima dal codice in-process " << juce::String (diff, 9)
                      << ", RMS uscita " << juce::String (dbOf (rmsOf (hosted, 48000, 48000)), 1) << " dBFS" << std::endl;
            check (diff < 1.0e-6, "VST3 caricato: stesso audio del codice in-process con tutti gli stadi attivi");
            check (maxDiffOf (hosted, in, 48000) > 0.01, "e il VST3 elabora davvero il segnale (non e' un passaggio diretto)");
        }

        // stato
        juce::MemoryBlock state;
        instance->getStateInformation (state);
        check (state.getSize() > 0, "il VST3 caricato salva lo stato");

        // editor
        if (instance->hasEditor())
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (instance->createEditorAndMakeActive());
            check (editor != nullptr && editor->getWidth() > 0, "l'editor del VST3 si apre (" + juce::String (editor ? editor->getWidth() : 0)
                                                                    + " x " + juce::String (editor ? editor->getHeight() : 0) + ")");
        }
    }

    //==========================================================================
    juce::MouseEvent makeEvent (juce::Component& c, juce::Point<float> pos, int clicks = 1, bool dragged = false,
                                juce::Point<float> downPos = {})
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(),
                                 downPos, juce::Time::getCurrentTime(), clicks, dragged);
    }

    void savePng (const juce::Image& image, const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        if (out.openedOk())
            png.writeImageToStream (image, out);
    }

    void testEditor (const juce::File& outDir)
    {
        section ("GUI: editor, interazione sul grafico, snapshot");

        ValstMsEqProcessor p;
        setBand (p, 0,   90.0f, 1.4f,  5.0f, -60.0f);
        setBand (p, 1, 1800.0f, 0.8f, -4.5f,  45.0f);
        setBand (p, 2, 9000.0f, 1.2f,  7.5f, 100.0f);
        setSat (p, 0, 35.0f);
        setSat (p, 1, 60.0f);
        setSat (p, 2, 20.0f);
        setHpf (p, 0, 70.0f);
        setHpf (p, 1, 180.0f);
        setInGain (p, 9.0f);
        setOutGain (p, 3.0f);
        p.setRateAndBufferSizeDetails (48000.0, 512);

        const int editorW = ValstMsEqEditor::baseWidth, editorH = ValstMsEqEditor::baseHeight;

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            check (editor != nullptr, "l'editor viene creato");

            auto& ed = static_cast<ValstMsEqEditor&> (*editor);
            check (ed.getWidth() == 960 && ed.getHeight() == 664, "dimensione predefinita 960 x 664");

            // i nuovi controlli ci sono e sono collegati ai parametri veri
            {
                std::vector<juce::Slider*> rotaries, sats, lowCuts, ins, outs;
                std::function<void (juce::Component&)> scan = [&] (juce::Component& c)
                {
                    if (auto* s = dynamic_cast<juce::Slider*> (&c))
                    {
                        if (s->isRotary())
                            rotaries.push_back (s);
                        if (s->getComponentID() == "sat")    sats.push_back (s);
                        if (s->getComponentID() == "lowcut") lowCuts.push_back (s);
                        if (s->getComponentID() == "in")     ins.push_back (s);
                        if (s->getComponentID() == "out")    outs.push_back (s);
                    }
                    for (auto* child : c.getChildren())
                        scan (*child);
                };
                scan (ed);

                check (rotaries.size() == 16, "16 pomelli: 9 delle bande + 3 SAT + 2 LOW CUT + IN + OUT (trovati " + juce::String ((int) rotaries.size()) + ")");
                check (sats.size() == 3 && lowCuts.size() == 2 && ins.size() == 1 && outs.size() == 1,
                       "3 SAT (uno per banda), 2 LOW CUT (bande 1 e 3), 1 IN, 1 OUT");

                if (sats.size() == 3 && lowCuts.size() == 2 && ins.size() == 1 && outs.size() == 1)
                {
                    checkNear (sats[0]->getValue(), 35.0, 1e-3, "il pomello SAT della banda 1 mostra il parametro (35 %)");
                    check (sats[1]->getTextFromValue (sats[1]->getValue()) == "60 %", "testo del pomello SAT: '60 %'");
                    check (lowCuts[0]->getTextFromValue (lowCuts[0]->getValue()).startsWith ("70"), "testo del pomello LOW CUT 1: '" + lowCuts[0]->getTextFromValue (lowCuts[0]->getValue()) + "'");
                    check (lowCuts[0]->getTextFromValue (hpfMinHz) == "OFF", "al minimo il LOW CUT mostra 'OFF'");
                    check (ins[0]->getTextFromValue (ins[0]->getValue()) == "+9.0 dB", "testo del pomello IN: '+9.0 dB'");

                    // muovere i pomelli muove i parametri (e viceversa)
                    sats[2]->setValue (75.0, juce::sendNotificationSync);
                    checkNear (p.getParam (2, kSat), 75.0, 1e-3, "muovendo SAT della banda 3 cambia il parametro");
                    lowCuts[1]->setValue (220.0, juce::sendNotificationSync);
                    checkNear (p.getGlobal (kHpf3), 220.0, 0.5, "muovendo LOW CUT 3 cambia il passa-alto della banda 3");
                    ins[0]->setValue (-6.0, juce::sendNotificationSync);
                    checkNear (p.getGlobal (kInGain), -6.0, 1e-3, "muovendo IN cambia il gain di ingresso");
                    outs[0]->setValue (-9.0, juce::sendNotificationSync);
                    checkNear (p.getGlobal (kOutGain), -9.0, 1e-3, "muovendo OUT cambia il gain di uscita");

                    setSat (p, 2, 20.0f);
                    setHpf (p, 1, 180.0f);
                    setInGain (p, 9.0f);
                    setOutGain (p, 3.0f);
                }
            }

            // trascinamento del nodo della banda 2
            auto& graph = ed.getGraph();
            const auto start = graph.getNodeCentre (1);
            const float f0 = p.getParam (1, kFreq), g0 = p.getParam (1, kGain);

            graph.mouseDown (makeEvent (graph, start, 1, false, start));
            graph.mouseDrag (makeEvent (graph, start + juce::Point<float> (120.0f, -40.0f), 1, true, start));
            graph.mouseUp (makeEvent (graph, start + juce::Point<float> (120.0f, -40.0f), 1, true, start));

            check (p.getParam (1, kFreq) > f0 * 1.3f, "trascinare il nodo a destra alza la frequenza (" + juce::String (f0, 0) + " -> " + juce::String (p.getParam (1, kFreq), 0) + " Hz)");
            check (p.getParam (1, kGain) > g0 + 3.0f, "trascinare il nodo in alto alza il guadagno (" + juce::String (g0, 1) + " -> " + juce::String (p.getParam (1, kGain), 1) + " dB)");
            check (p.getParam (1, kMs) == 45.0f, "il fader Mid/Side non viene toccato dal trascinamento");

            // rotella = larghezza
            const float w0 = p.getParam (1, kWidth);
            juce::MouseWheelDetails wheel;
            wheel.deltaX = 0.0f; wheel.deltaY = 0.47f; wheel.isReversed = false; wheel.isSmooth = false; wheel.isInertial = false;
            graph.mouseWheelMove (makeEvent (graph, graph.getNodeCentre (1)), wheel);
            check (p.getParam (1, kWidth) > w0 * 1.1f, "la rotella sul nodo allarga la campana (" + juce::String (w0, 2) + " -> " + juce::String (p.getParam (1, kWidth), 2) + " oct)");

            // doppio clic = azzera il guadagno
            graph.mouseDoubleClick (makeEvent (graph, graph.getNodeCentre (1), 2));
            checkNear (p.getParam (1, kGain), 0.0, 1e-6, "doppio clic sul nodo azzera il guadagno");

            // fader Mid/Side di una banda, mosso con eventi mouse veri: scala logaritmica e agganci
            std::vector<juce::Slider*> faders;
            std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
            {
                if (auto* s = dynamic_cast<juce::Slider*> (&c))
                    if (s->getSliderStyle() == juce::Slider::LinearVertical)
                        faders.push_back (s);
                for (auto* child : c.getChildren())
                    collect (*child);
            };
            collect (ed);
            check (faders.size() == 3, "ci sono 3 fader Mid/Side, uno per banda");

            if (faders.size() == 3)
            {
                setBand (p, 0, 100.0f, 1.0f, 6.0f, 0.0f);          // fader della banda 1 al centro
                auto& fader = *faders[0];
                const auto  centre = fader.getLocalBounds().toFloat().getCentre();
                const float travel = (float) fader.getHeight();

                auto drag = [&] (float dy)
                {
                    auto& base = static_cast<juce::Component&> (fader);
                    const auto from = fader.getLocalBounds().toFloat().getCentre();
                    base.mouseDown (makeEvent (fader, from, 1, false, from));
                    base.mouseDrag (makeEvent (fader, from + juce::Point<float> (0.0f, dy), 1, true, from));
                    base.mouseUp   (makeEvent (fader, from + juce::Point<float> (0.0f, dy), 1, true, from));
                };

                juce::ignoreUnused (centre);
                checkNear (p.getParam (0, kMs), 0.0, 1e-6, "il fader parte da 0 (neutro)");

                drag (-0.1f * travel);
                const float small = p.getParam (0, kMs);
                check (small > 0.0f && small < 15.0f, "10% di corsa verso l'alto = valore piccolo (" + juce::String (small, 0) + "): regolazione fine vicino allo 0");

                drag (-0.5f * travel);
                const float big = p.getParam (0, kMs);
                check (big > 40.0f, "trascinando in alto il valore sale (" + juce::String (big, 0) + ", verso il Side)");

                setReal (p, paramId (0, kMs), 0.0f);
                drag (-2.0f * travel);
                checkNear (p.getParam (0, kMs), 100.0, 1e-6, "tutto in alto = +100 (solo Side)");
                drag (+4.0f * travel);
                checkNear (p.getParam (0, kMs), -100.0, 1e-6, "tutto in basso = -100 (solo Mid)");

                // doppio clic = torna a 0
                static_cast<juce::Component&> (fader).mouseDoubleClick (makeEvent (fader, centre, 2));
                checkNear (p.getParam (0, kMs), 0.0, 1e-6, "doppio clic sul fader = 0");

                // il testo mostrato e' quello del fader
                setReal (p, paramId (0, kMs), 37.0f);
                check (fader.getTextFromValue (fader.getValue()) == "+37 S", "testo del fader a +37 = '+37 S'");
            }

            // ripristino i valori per gli snapshot
            setBand (p, 0,   90.0f, 1.4f,  5.0f, -60.0f);
            setBand (p, 1, 1800.0f, 0.8f, -4.5f,  45.0f);
            ed.setSize (editorW, editorH);

            if (outDir != juce::File())
            {
                outDir.createDirectory();

                graph.setExternalBand (-1);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_960x664.png"));
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f), outDir.getChildFile ("editor_960x664_2x.png"));

                graph.setExternalBand (1);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f), outDir.getChildFile ("editor_band2_highlight_2x.png"));

                graph.setExternalBand (-1);
                ed.setSize (1440, 996);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_1440x996.png"));

                ed.setSize (720, 498);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_720x498.png"));

                // stato predefinito (tutto a zero), come la prima volta che il plugin viene caricato
                ValstMsEqProcessor fresh;
                std::unique_ptr<juce::AudioProcessorEditor> freshEditor (fresh.createEditor());
                savePng (freshEditor->createComponentSnapshot (freshEditor->getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_default_state.png"));

                std::cout << "     snapshot salvati in " << outDir.getFullPathName() << std::endl;
            }
        }

        for (int i = 0; i < 5; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            editor->setSize (editorW, editorH);
        }
        check (true, "apertura/chiusura ripetuta dell'editor senza crash");
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::File vst3, outDir;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--vst3" && i + 1 < argc) vst3 = juce::File (juce::String (argv[++i]));
        else if (arg == "--out" && i + 1 < argc) outDir = juce::File (juce::String (argv[++i]));
    }

    // --- versione 1.0: devono continuare a passare identici
    testMapping();
    testFlatPassThrough();
    testMidSideGains();
    testGraphMatchesAudio();
    testBellShape();
    testBlockSizes();
    testNoClicks();
    testState();

    // --- versione 1.1, una fase alla volta
    testPhase1Valve();
    testPhase2BandSaturation();
    testPhase3AllBands();
    testPhase4Interaction();
    testPhase5Transformer();
    testDeepSaturation();
    testPhase6HighPass();
    testPhase7Integration();

    testEditor (outDir);

    if (vst3 != juce::File())
        testHostedVst3 (vst3);

    std::cout << "\n" << checks << " controlli, " << failures << " falliti" << std::endl;
    return failures == 0 ? 0 : 1;
}
