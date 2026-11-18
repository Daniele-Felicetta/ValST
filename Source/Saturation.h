#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

//==============================================================================
//  Saturazione di ValST MS EQ: funzioni di waveshaping, anti-aliasing (ADAA),
//  gestione del DC e identita' deterministica delle bande.
//
//  Tutto in double, senza allocazioni e senza dipendenze da JUCE, cosi' lo stesso codice
//  gira nel plugin e nei test.
//
//  Idea di fondo: ogni stadio non lineare lavora in "forma residuo"
//        y = x + (1/g) * h(g * x),        h(s) = f(s) - s
//  dove f e' la curva di saturazione con pendenza 1 nell'origine. La parte lineare del segnale
//  non passa mai dal waveshaper: se l'ingresso e' piccolo h(s) -> 0 e l'uscita e' identica
//  all'ingresso (nessun filtro, nessun ritardo, nessuna perdita in alta frequenza). L'anti-aliasing
//  (ADAA) agisce soltanto sul residuo, cioe' soltanto sulle armoniche generate.
//==============================================================================
namespace valst::sat
{
    constexpr double halfPi = 1.57079632679489661923;
    constexpr double ln2    = 0.69314718055994530942;

    //==========================================================================
    //  Primitive: funzioni dispari con pendenza 1 in 0 e uscita limitata a 1.
    //  Si usa il modulo a = |s| (f e' dispari, F1 e' pari).
    //    f  = funzione,   F1 = primitiva (integrale da 0), necessaria all'ADAA.
    //==========================================================================
    struct Prim
    {
        // SINE: sin(s) fino a pi/2, poi costante a 1. Raccordo C1: armoniche che decadono in fretta,
        // e' l'estremo "morbido / progressivo" (decadimento armonico ripido, come i vertici smussati
        // di un'onda triangolare).
        static double sineF  (double a) noexcept { return std::sin (std::min (a, halfPi)); }
        static double sineF1 (double a) noexcept { return a <= halfPi ? 1.0 - std::cos (a) : 1.0 + a - halfPi; }

        // TANH: saturazione classica, analitica ovunque (armoniche solo dispari se simmetrica).
        static double tanhF  (double a) noexcept { return std::tanh (a); }
        static double tanhF1 (double a) noexcept { return a + std::log1p (std::exp (-2.0 * a)) - ln2; }

        // HARD: lineare fino a 1-d, ginocchio parabolico tra 1-d e 1+d (raccordo C1), poi costante a 1.
        // E' l'estremo "duro / aggressivo": tende all'onda quadra quando e' pilotata forte.
        static double hardF (double a, double d) noexcept
        {
            const double t0 = 1.0 - d;
            if (a <= t0)        return a;
            if (a <= 1.0 + d)   { const double e = a - t0; return a - e * e / (4.0 * d); }
            return 1.0;
        }

        static double hardF1 (double a, double d) noexcept
        {
            const double t0 = 1.0 - d;
            if (a <= t0)        return 0.5 * a * a;
            if (a <= 1.0 + d)   { const double e = a - t0; return 0.5 * a * a - e * e * e / (12.0 * d); }
            return 0.5 * (1.0 + d) * (1.0 + d) - (8.0 * d * d * d) / (12.0 * d) + (a - (1.0 + d));
        }
    };

    //==========================================================================
    /** Famiglia di curve di saturazione, con due manopole matematiche indipendenti:

          q     (0..1)  durezza: 0 = SINE (morbida), 0.5 = TANH, 1 = HARD (dura). Le tre primitive vengono
                        combinate con pesi continui, quindi la forma cambia senza salti (non e' una
                        selezione tra algoritmi).
          asym  (-0.5..0.5)  asimmetria: la semionda positiva satura con ginocchio kp = 1+asym, la negativa
                        con kn = 1-asym. E' cio' che genera le armoniche PARI; con asym = 0 la curva e'
                        dispari e produce solo armoniche dispari.

        La curva e' f(s) = (1/k) * sum_i w_i * prim_i(k*s), con k = kp (s>=0) o kn (s<0). Sia la funzione sia la
        sua primitiva sono in forma chiusa. */
    struct Shaper
    {
        double wS = 0.0, wT = 1.0, wH = 0.0;    // pesi di SINE, TANH, HARD (somma = 1)
        double kp = 1.0, kn = 1.0;              // ginocchio semionda positiva / negativa
        double delta = 0.12;                    // semi-ampiezza del ginocchio della primitiva HARD
        double residualScale = 1.0;             // 1 - pendenza residua oltre il ginocchio (vedi trasformatore)
        double deadZone = 0.0;                  // se > 0: |s| <= deadZone => residuo esattamente 0
        double leakGateStart = 0.5;             // inviluppo di |s| sotto cui vale la forma "residuo" pura (trasparente)
        double leakGateEnd = 2.0;               // inviluppo di |s| sopra cui vale l'ADAA completo (saturazione profonda)

        /** Peso (1 = forma residuo, 0 = ADAA completo) in funzione dell'inviluppo di |s|, con raccordo C1. */
        double leakGate (double envelope) const noexcept
        {
            if (envelope <= leakGateStart) return 1.0;
            if (envelope >= leakGateEnd)   return 0.0;
            const double t = (envelope - leakGateStart) / (leakGateEnd - leakGateStart);
            return 1.0 - t * t * (3.0 - 2.0 * t);
        }

        void setShape (double q, double asym) noexcept
        {
            q = std::min (1.0, std::max (0.0, q));
            wS = std::max (0.0, 1.0 - 2.0 * q);
            wH = std::max (0.0, 2.0 * q - 1.0);
            wT = 1.0 - wS - wH;
            kp = 1.0 + asym;
            kn = 1.0 - asym;
        }

        double half (double a, double k) const noexcept
        {
            const double x = k * a;
            double y = 0.0;
            if (wS > 0.0) y += wS * Prim::sineF (x);
            if (wT > 0.0) y += wT * Prim::tanhF (x);
            if (wH > 0.0) y += wH * Prim::hardF (x, delta);
            return y / k;
        }

        double half1 (double a, double k) const noexcept
        {
            const double x = k * a;
            double y = 0.0;
            if (wS > 0.0) y += wS * Prim::sineF1 (x);
            if (wT > 0.0) y += wT * Prim::tanhF1 (x);
            if (wH > 0.0) y += wH * Prim::hardF1 (x, delta);
            return y / (k * k);
        }

        /** Curva completa f(s). */
        double f (double s) const noexcept { return s >= 0.0 ? half (s, kp) : -half (-s, kn); }

        /** Residuo non lineare h(s) = scale * (f(s) - s): e' la sola parte che viene aggiunta al segnale. */
        double h (double s) const noexcept { return residualScale * (f (s) - s); }

        /** Primitiva di h: H1(s) = scale * (F1(s) - s^2/2). */
        double H1 (double s) const noexcept
        {
            return residualScale * ((s >= 0.0 ? half1 (s, kp) : half1 (-s, kn)) - 0.5 * s * s);
        }
    };

    //==========================================================================
    /** Anti-aliasing ADAA di primo ordine (Antiderivative Anti-Aliasing, Parker et al. 2016) applicato al residuo:

            r[n] = ( H1(s[n]) - H1(s[n-1]) ) / ( s[n] - s[n-1] )

        Equivale a integrare la curva sul tratto percorso dal segnale tra due campioni invece di
        campionarla in un punto: lo spettro del residuo si "smorza" prima di ripiegarsi sotto Nyquist,
        senza sovracampionamento e senza filtri di anti-aliasing. Quando |ds| e' minuscolo si usa il
        valore nel punto medio (limite della formula).

        Correzione di allineamento. La media del residuo e' centrata a meta' tra due campioni, mentre la parte
        lineare del segnale (x[n]) e' al campione n. Con segnali piccoli non conta (il residuo e' ~0 e l'uscita e'
        esattamente x[n]); in saturazione profonda invece h(s) = c - s, quindi la media toglie (s[n]+s[n-1])/2 e
        l'uscita diventa c + (s[n]-s[n-1])/2: la "semi-differenza" dell'ingresso passa senza essere limitata, e in
        alta frequenza supera di molte volte il livello di saturazione. L'ADAA completo (parte lineare compresa)
        non ha questo difetto ma ha una perdita in alta frequenza e mezzo campione di ritardo anche a livelli
        bassi. Si fondono le due cose: un inseguitore di inviluppo di |s| (attacco immediato, rilascio ~10 ms)
        decide quanto si e' dentro la saturazione, e il residuo viene portato dalla forma pura (trasparente) a
        quella completa:

            r' = r - (1 - g(env)) * scale * (s[n] - s[n-1]) / 2,    g = 1 sotto la soglia, 0 in saturazione profonda

        L'inviluppo varia lentamente (non dentro il ciclo del segnale): la correzione e' un filtro lineare quasi
        costante e non genera componenti nuove, quindi non riporta aliasing. */
    struct AdaaResidual
    {
        double sPrev = 0.0, h1Prev = 0.0, env = 0.0;
        double decay = 0.9979;                // rilascio dell'inviluppo per campione (10 ms a 48 kHz)

        static constexpr double envelopeSeconds = 0.010;

        void prepare (double sampleRate) noexcept { decay = std::exp (-1.0 / (envelopeSeconds * sampleRate)); }

        void reset() noexcept { sPrev = 0.0; h1Prev = 0.0; env = 0.0; }

        /** Da chiamare quando cambia la scala dell'ingresso della curva (il pilotaggio): lo stato e' in unita' di s. */
        void rescale (double ratio) noexcept { sPrev *= ratio; env *= ratio; }

        /** Da chiamare quando cambiano i parametri della curva: ricalcola la primitiva del campione precedente
            con la curva nuova (altrimenti la differenza finita mescolerebbe due curve diverse). */
        void refresh (const Shaper& sh) noexcept { h1Prev = sh.H1 (sPrev); }

        inline double process (const Shaper& sh, double s, bool useAdaa) noexcept
        {
            const double a  = std::abs (s);
            const double ds = s - sPrev;

            if (! useAdaa)
            {
                sPrev = s;
                return sh.h (s);
            }

            double r;
            if (sh.deadZone > 0.0 && a <= sh.deadZone && std::abs (sPrev) <= sh.deadZone)
            {
                r = 0.0;                      // nella zona morta h e' esattamente 0...
                h1Prev = 0.0;                 // ...e anche la sua primitiva H1
            }
            else
            {
                const double h1 = sh.H1 (s);
                r = std::abs (ds) < 1.0e-4 ? sh.h (0.5 * (s + sPrev)) : (h1 - h1Prev) / ds;
                h1Prev = h1;
            }

            // la correzione di allineamento vale anche nella zona morta: se la si saltasse, a ogni passaggio per lo
            // zero (dove ds e' massimo) comparirebbe un gradino
            const double peak = std::max (a, std::abs (sPrev));
            env = std::max (peak, env * decay);

            if (env > sh.leakGateStart)
                r -= (1.0 - sh.leakGate (env)) * 0.5 * sh.residualScale * ds;

            sPrev = s;
            return r;
        }
    };

    //==========================================================================
    /** Blocco del DC (passa-alto di primo ordine a pochi Hz). Si applica al solo residuo: il percorso
        lineare del segnale non viene mai toccato. */
    struct DcBlocker
    {
        double x1 = 0.0, y1 = 0.0, coeff = 0.999;

        void reset() noexcept { x1 = y1 = 0.0; }

        void prepare (double sampleRate, double cutoffHz = 8.0) noexcept
        {
            coeff = 1.0 - 6.283185307179586 * cutoffHz / sampleRate;
        }

        inline double process (double x) noexcept
        {
            const double y = x - x1 + coeff * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    //==========================================================================
    //  Identita' deterministica delle bande
    //==========================================================================

    /** Seed predefiniti delle tre bande. Sono costanti fisse: scelte una volta (tra 500 candidati, massimizzando la
        differenza di FORMA armonica tra le tre bande a parita' di intensita') e poi congelate. Il plugin le salva
        comunque nello stato, cosi' un progetto resta identico anche se in futuro cambiassero i valori predefiniti. */
    constexpr uint64_t defaultSeeds[3] = { 0xCB8789E97A8D571CULL, 0xA05417BB5FB3086EULL, 0xCBD670BB78664394ULL };

    /** Versione dell'algoritmo che trasforma un seed in carattere timbrico (salvata nello stato). */
    constexpr int characterAlgorithmVersion = 1;

    inline uint64_t splitmix64 (uint64_t& state) noexcept
    {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /** Carattere timbrico di una banda: sette numeri ricavati dal seed con un generatore pseudocasuale
        deterministico (SplitMix64). Il seed e' persistente, quindi il carattere non cambia mai tra una
        sessione e l'altra: la "casualita'" sta nella scelta della funzione di saturazione, non nel segnale. */
    struct Character
    {
        double qOffset     = 0.0;    // spostamento della durezza della curva (-0.18 .. +0.18)
        double asymBase    = 0.04;   // asimmetria (pari/dispari) a gain 0 dB (0.01 .. 0.09)
        double asymMax     = 0.2;    // asimmetria a +/-18 dB di gain (0.14 .. 0.28)
        double freqTilt    = 1.0;    // quanto la frequenza sposta la durezza (0.6 .. 1.4)
        double bwTilt      = 1.0;    // quanto la larghezza sposta la durezza (0.6 .. 1.4)
        double driveTrimDb = 0.0;    // piccola correzione del pilotaggio (-1.5 .. +1.5 dB)
        int    polarity    = 1;      // quale semionda satura per prima (fase della 2a armonica)
    };

    inline Character makeCharacter (uint64_t seed) noexcept
    {
        uint64_t state = seed ^ 0xD1B54A32D192ED03ULL;
        auto u01 = [&state] { return (double) (splitmix64 (state) >> 11) * (1.0 / 9007199254740992.0); };

        Character c;
        c.qOffset     = -0.18 + 0.36 * u01();
        c.asymBase    = 0.01 + 0.08 * u01();
        c.asymMax     = 0.14 + 0.14 * u01();
        c.freqTilt    = 0.6 + 0.8 * u01();
        c.bwTilt      = 0.6 + 0.8 * u01();
        c.driveTrimDb = -1.5 + 3.0 * u01();
        c.polarity    = u01() < 0.5 ? -1 : 1;
        return c;
    }

    //==========================================================================
    //  Mappa parametri EQ -> forma della saturazione di una banda
    //==========================================================================
    //  La manopola SAT di una banda e' la QUOTA di saturazione che entra nel segnale (0..100 %): a 0 % la banda e'
    //  esattamente la campana lineare, a 100 % entra tutto il residuo della curva. Il pilotaggio della curva ha una
    //  base fissa (il ginocchio sta a circa -20 dBFS sul picco della fetta di spettro isolata) e cresce solo di
    //  qualche dB con SAT. Scelta voluta: se il pilotaggio crescesse molto, il tetto della curva (1/pilotaggio)
    //  scenderebbe tanto da far diminuire le armoniche generate su materiale forte, proprio quando SAT sale.
    //  Cosi', invece, le armoniche crescono sempre con SAT. L'altra leva e' il GAIN della banda: la saturazione
    //  sta dopo la campana, quindi +6 dB di boost sono +6 dB di pilotaggio nella curva.
    constexpr double satBaseDriveDb  = 20.0;
    constexpr double satDriveRangeDb = 6.0;

    /** Parametri della curva di una banda per un canale (Mid o Side).

          Frequency  : bande basse -> curva piu' dura (armoniche alte che decadono piano, aggressive);
                       bande alte  -> curva piu' morbida (armoniche che decadono in fretta: piu' fluida
                       e con meno aliasing, dato che le armoniche cadrebbero sopra Nyquist).
          Bandwidth  : bande strette -> curva piu' dura, focalizzata sulla fetta di spettro isolata;
                       bande larghe  -> curva piu' morbida (piu' componenti nel waveshaper = piu'
                       intermodulazione, quindi si ammorbidisce).
          Gain       : |gain| alto -> curva un po' piu' dura e, soprattutto, piu' asimmetrica
                       (rapporto armoniche pari/dispari crescente).
          Seed       : sposta tutto questo in modo diverso per ogni banda (Character). */
    constexpr double kneeSine = 0.82, kneeTanh = 0.615, kneeHard = 1.12;    // ingresso a cui ogni primitiva comprime di 1 dB

    /** Le tre primitive iniziano a comprimere a livelli diversi (punto a -1 dB: 0.82, 0.62 e 1.12): senza correzione
        una banda "dura" sembrerebbe molto meno saturata di una "morbida" con la stessa manopola SAT. driveScale
        riporta il punto a -1 dB sempre allo stesso livello, cosi' la manopola da' la stessa "dose" su ogni banda
        e cambia solo il TIMBRO. Il pilotaggio effettivo e' (pilotaggio della banda) * driveScale. */
    inline void computeBandShape (const Character& c, double freqHz, double widthOct, double gainAbsDb,
                                  Shaper& out, double& driveScale) noexcept
    {
        const double phiF = std::min (1.0, std::max (-1.0, std::log2 (freqHz / 632.455532) / 4.9829));    // -1 (20 Hz) .. +1 (20 kHz)
        const double phiB = std::min (1.0, std::max (-1.0, std::log2 (0.77 / widthOct) / 2.95));           // +1 (0.1 oct) .. -1 (6 oct)
        const double phiG = std::min (1.0, std::max (0.0, gainAbsDb / 18.0));

        const double q = 0.5 - 0.40 * c.freqTilt * phiF + 0.30 * c.bwTilt * phiB + 0.12 * phiG + c.qOffset;
        const double asym = (double) c.polarity * (c.asymBase + (c.asymMax - c.asymBase) * phiG);

        out.setShape (q, asym);
        out.delta = 0.12;
        out.residualScale = 1.0;
        out.deadZone = 0.0;

        driveScale = kneeTanh / (out.wS * kneeSine + out.wT * kneeTanh + out.wH * kneeHard);
    }

    /** Pilotaggio lineare (g) della saturazione di una banda a partire dalla manopola SAT (0..1). */
    inline double bandDriveLinear (const Character& c, double sat01) noexcept
    {
        const double db = satBaseDriveDb + sat01 * satDriveRangeDb + c.driveTrimDb;
        return std::pow (10.0, db / 20.0);
    }

    /** Quota del residuo che arriva in uscita: 0 a SAT = 0 (bypass esatto), 1 a SAT = 100 %. */
    inline double bandWet (double sat01) noexcept
    {
        return std::min (1.0, std::max (0.0, sat01));
    }

    //==========================================================================
    //  Stadio valvolare (ingresso)
    //==========================================================================
    constexpr double valveMaxGainDb   = 24.0;    // a +24 dB il ginocchio e' a 0 dBFS (kappa = 1)
    constexpr double valveAsymmetry   = 0.22;    // semionda positiva piu' compressa (corrente di griglia)
    constexpr double valveCompression = 0.5;     // quota (in dB) del guadagno di ingresso che viene compensata

    /** kappa = scala del ginocchio della valvola: 0 a 0 dB (stadio perfettamente lineare), cresce fino a 1 a +24 dB.
        Il pilotaggio e' quindi legato al guadagno: piu' gain = piu' livello nella curva E curva piu' "spinta". */
    inline double valveKappa (double gainDb) noexcept
    {
        return std::min (1.0, std::max (0.0, gainDb / valveMaxGainDb));
    }

    /** Compensazione automatica dopo la valvola: toglie meta' dei dB di guadagno positivo. Sta DOPO la non
        linearita', quindi non cambia la struttura armonica: cambia solo il livello. */
    inline double valveCompensationLinear (double gainDb) noexcept
    {
        return std::pow (10.0, -valveCompression * std::max (0.0, gainDb) / 20.0);
    }

    //==========================================================================
    //  Stadio trasformatore (uscita)
    //==========================================================================
    // La curva e' la primitiva HARD (ginocchio parabolico) con la zona lineare che finisce ESATTAMENTE a 0 dBFS:
    //   - fino a 0 dBFS il residuo e' esattamente zero (trasparente, bit-identico alla 1.0)
    //   - da 0 dBFS in su la compressione comincia in modo graduale (raccordo C1) e diventa via via piu' forte
    //   - oltre il ginocchio la pendenza non va a zero ma a trafoTailSlope: il nucleo non si appiattisce mai
    //     del tutto, quindi niente clipping netto (e non e' un limitatore: nessun muro a 0 dBFS).
    constexpr double trafoKneeHalfWidth = 0.30;   // ginocchio: da 0 dBFS (1.0) a 1.857 (+5.4 dBFS), tetto a 1.43 (+3.1 dBFS)
    constexpr double trafoTailSlope     = 0.05;   // pendenza oltre il ginocchio
    constexpr double trafoDrive         = 1.0 - trafoKneeHalfWidth;   // s = trafoDrive * e  ->  la zona lineare finisce a |e| = 1
    constexpr double trafoFluxBoost     = 0.35;   // il nucleo satura prima sulle basse frequenze (volt-secondi): +2.6 dB sotto ~100 Hz
    constexpr double trafoFluxCorner    = 100.0;  // Hz

    inline Shaper makeTransformerShaper() noexcept
    {
        Shaper s;
        s.setShape (1.0, 0.0);                    // solo la primitiva HARD, simmetrica
        s.delta = trafoKneeHalfWidth;
        s.residualScale = 1.0 - trafoTailSlope;
        s.deadZone = 1.0 - trafoKneeHalfWidth;
        s.leakGateStart = 0.8;                    // il ginocchio finisce a s = 1.3: la correzione di allineamento
        s.leakGateEnd = 1.8;                      // e' completa poco oltre
        return s;
    }

    inline Shaper makeValveShaper() noexcept
    {
        Shaper s;
        s.setShape (0.5, valveAsymmetry);         // TANH asimmetrica: armoniche pari e dispari
        return s;
    }
}
