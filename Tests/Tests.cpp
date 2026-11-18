//==============================================================================
//  Test di ValST MS EQ
//    1. matematica del mapping Mid/Side e del fader logaritmico
//    2. DSP: guadagni su Mid / Side, accordo tra audio e grafico, assenza di click
//    3. stato (salvataggio / ripristino)
//    4. il VST3 compilato, caricato come farebbe un DAW (se si passa --vst3 <percorso>)
//    5. GUI: trascinamento dei nodi, rotella, snapshot PNG (se si passa --out <cartella>)
//==============================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>
#include <cmath>
#include <random>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MsEqDsp.h"

using namespace valst;

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
        check (same, "tutti i 12 parametri tornano identici dopo il ripristino");

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

        // 12 parametri del plugin + il parametro "Bypass" che il wrapper VST3 di JUCE aggiunge per i DAW
        int pluginParams = 0, bypassParams = 0;
        for (auto* param : instance->getParameters())
        {
            std::cout << "     - " << param->getName (64) << "  [" << param->getText (param->getDefaultValue(), 64) << "]" << std::endl;
            if (param->getName (64) == "Bypass") ++bypassParams; else ++pluginParams;
        }
        check (pluginParams == 12 && bypassParams == 1, "espone i 12 parametri (" + juce::String (pluginParams) + ") piu' il Bypass dell'host (" + juce::String (bypassParams) + ")");

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
        p.setRateAndBufferSizeDetails (48000.0, 512);

        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            check (editor != nullptr, "l'editor viene creato");

            auto& ed = static_cast<ValstMsEqEditor&> (*editor);
            check (ed.getWidth() == 960 && ed.getHeight() == 600, "dimensione predefinita 960 x 600");

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
            ed.setSize (960, 600);

            if (outDir != juce::File())
            {
                outDir.createDirectory();

                graph.setExternalBand (-1);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_960x600.png"));
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f), outDir.getChildFile ("editor_960x600_2x.png"));

                graph.setExternalBand (1);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f), outDir.getChildFile ("editor_band2_highlight_2x.png"));

                graph.setExternalBand (-1);
                ed.setSize (1440, 900);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_1440x900.png"));

                ed.setSize (720, 450);
                savePng (ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f), outDir.getChildFile ("editor_720x450.png"));

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
            editor->setSize (960, 600);
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

    testMapping();
    testFlatPassThrough();
    testMidSideGains();
    testGraphMatchesAudio();
    testBellShape();
    testBlockSizes();
    testNoClicks();
    testState();
    testEditor (outDir);

    if (vst3 != juce::File())
        testHostedVst3 (vst3);

    std::cout << "\n" << checks << " controlli, " << failures << " falliti" << std::endl;
    return failures == 0 ? 0 : 1;
}
