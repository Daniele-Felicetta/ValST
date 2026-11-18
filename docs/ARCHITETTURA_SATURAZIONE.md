# ValST MS EQ 1.1 – Architettura della saturazione

Questo documento spiega **come funziona** la saturazione analogica introdotta nella 1.1.0: percorso del segnale,
matematica, come nascono le armoniche, come frequenza / larghezza / gain deformano la curva, come funziona la
"casualità deterministica" delle bande, come interagiscono gli stadi e quali misure mostrano che tutto è reale.

Ogni numero citato esce dai test automatici (`Tests/Tests.cpp`: 284 controlli sul codice, 311 con il file VST3 compilato
caricato come farebbe un DAW; 0 falliti) e si può rigenerare (vedi [Come riprodurre le misure](#17-come-riprodurre-le-misure)). Il codice di riferimento è
[`Source/Saturation.h`](../Source/Saturation.h) (curve, anti-aliasing, seed) e
[`Source/MsEqDsp.h`](../Source/MsEqDsp.h) (motore audio).

1. [Principi di progetto](#1-principi-di-progetto)
2. [Percorso del segnale](#2-percorso-del-segnale)
3. [Il mattone di base: la forma "residuo"](#3-il-mattone-di-base-la-forma-residuo)
4. [Le curve di saturazione: da dove vengono pari e dispari](#4-le-curve-di-saturazione-da-dove-vengono-pari-e-dispari)
5. [Anti-aliasing (ADAA) e il difetto trovato in saturazione profonda](#5-anti-aliasing-adaa-e-il-difetto-trovato-in-saturazione-profonda)
6. [Input Gain: lo stadio valvolare](#6-input-gain-lo-stadio-valvolare)
7. [Saturazione di banda](#7-saturazione-di-banda)
8. [Identità deterministica delle bande](#8-identità-deterministica-delle-bande)
9. [Interazione seriale tra le bande](#9-interazione-seriale-tra-le-bande)
10. [Output Gain: lo stadio a trasformatore](#10-output-gain-lo-stadio-a-trasformatore)
11. [I due filtri passa-alto](#11-i-due-filtri-passa-alto)
12. [Qualità del DSP](#12-qualità-del-dsp)
13. [Gain staging](#13-gain-staging)
14. [Compatibilità con la 1.0](#14-compatibilità-con-la-10)
15. [Risultati misurati](#15-risultati-misurati)
16. [Scelte interpretative e limiti noti](#16-scelte-interpretative-e-limiti-noti)
17. [Come riprodurre le misure](#17-come-riprodurre-le-misure)

---

## 1. Principi di progetto

* **Solo DSP reale.** Ogni controllo nuovo agisce sul percorso audio. Il grafico usa gli stessi coefficienti dei filtri
  (classe `LinearResponse`), non una simulazione.
* **Trasparenza a controlli neutri.** Con IN = 0 dB, SAT = 0 %, LOW CUT = OFF e un segnale che resta sotto 0 dBFS,
  il plugin si comporta come l'EQ lineare della 1.0: con EQ piatto l'uscita è identica **bit per bit**; con un EQ
  impostato coincide con un calcolo indipendente della sola catena di campane entro 2·10⁻⁸ (arrotondamento a 32 bit), e un
  preset della 1.0 caricato nella 1.1 suona come impostando a mano gli stessi 12 parametri. Si ottiene lavorando sempre in
  *forma residuo* (sezione 3): la parte lineare del segnale non passa mai da un waveshaper.
* **Determinismo.** Nessun rumore, nessun generatore casuale nel percorso audio. Stesso audio + stessi parametri +
  stesso seed = stessa uscita bit per bit, anche tra istanze diverse e tra sessioni diverse.
* **Thread audio pulito.** Nessuna allocazione, nessun lock: verificato con una sonda che conta gli `operator new`
  (0 allocazioni in 400 blocchi, stereo e mono, con automazione di tutti gli stadi).
* **Compatibilità.** I 12 parametri della 1.0 hanno gli stessi ID, lo stesso significato e lo stesso ordine; i 7 nuovi
  sono accodati con valore predefinito neutro (sezione 14).

---

## 2. Percorso del segnale

```
 L,R in
   │
   ▼
 INPUT GAIN ─► VALVOLA (L e R separati) ─► compensazione (toglie metà dei dB in salita)
   │
   ▼
 M = (L+R)/2 ,  S = (L−R)/2
   │
   ▼
 LOW CUT 1   (1° ordine, 6 dB/ott: pendenza molto graduale)
   │
   ▼
 BANDA 1 ─► BANDA 2 ─► BANDA 3          ogni banda, su Mid e su Side, in serie con la precedente:
   │                                       y   = campana(x)                  EQ lineare, come la 1.0
   ▼                                       u   = passabanda(y)               la "fetta" di spettro della banda
 LOW CUT 3   (2° ordine, 12 dB/ott)        r   = residuo non lineare(u)      curva deformata da freq/larghezza/gain
   │                                       out = y + peso · r                peso = SAT · fader Mid/Side
   ▼
 L = M+S ,  R = M−S
   │
   ▼
 OUTPUT GAIN ─► TRASFORMATORE (L e R separati, entra in gioco da 0 dBFS in su)
   │
   ▼
 L,R out
```

Calcolo interno in `double`; buffer di ingresso e uscita a 32 bit. Latenza: 0 campioni. Con il layout mono il canale
destro è una copia del sinistro: Side = 0, lavora solo il Mid.

---

## 3. Il mattone di base: la forma "residuo"

Una curva di saturazione `f(s)` ha pendenza 1 nell'origine e uscita limitata. Invece di far passare il segnale dalla
curva, ogni stadio calcola

```
y = x + (1/g) · h(g·x)          con   h(s) = f(s) − s        (g = pilotaggio dello stadio)
```

cioè somma al segnale **soltanto lo scarto** della curva dalla retta. Conseguenze:

* **Trasparenza.** Per `s → 0` il residuo `h(s)` tende a 0 (per una curva tipo tanh vale circa `−s³/3`), quindi a livelli
  bassi `y = x` esattamente: nessun filtro, nessun ritardo, nessuna perdita in alta frequenza.
* **Transizione graduale lineare → non lineare.** Con `x = A·sin ωt` e `f = tanh`:
  `y ≈ x − g²x³/3`, fondamentale `A(1 − g²A²/4)`, terza armonica `g²A³/12`. Il livello relativo della terza
  armonica sale di **2 dB per ogni dB** di segnale in più; la distorsione totale cresce come `g²A²`. Quando il segnale si
  avvicina al ginocchio la curva comprime di più e compaiono armoniche superiori: progressione continua, senza soglie.
* **L'anti-aliasing lavora solo sulle armoniche generate** (sezione 5), non sul segnale.

---

## 4. Le curve di saturazione: da dove vengono pari e dispari

Tutte le curve sono combinazioni, con pesi continui, di tre funzioni dispari con pendenza 1 nell'origine e uscita
limitata a 1. Di ciascuna esistono in forma chiusa sia la funzione sia la primitiva (serve all'ADAA):

| Primitiva | Funzione `f(a)`, con `a = abs(s)` | Carattere |
|-----------|-----------------------------------|-----------|
| **SINE** | `sin(min(a, π/2))` | raccordo C¹ in π/2: armoniche che decadono in fretta (estremo *morbido*) |
| **TANH** | `tanh(a)` | saturazione classica, analitica ovunque |
| **HARD** | lineare fino a `1−δ`, ginocchio parabolico fino a `1+δ`, poi costante | tende all'onda quadra se pilotata forte (estremo *duro*) |

Una sola manopola matematica, la **durezza** `q ∈ [0,1]`, dosa le tre curve senza salti:

```
wS = max(0, 1 − 2q)        wH = max(0, 2q − 1)        wT = 1 − wS − wH
q = 0  → solo SINE      q = 0.5 → solo TANH      q = 1 → solo HARD
```

Non è una selezione tra algoritmi: spostando `q` di poco la forma cambia di poco.

**Armoniche pari e dispari.** Una curva dispari (`f(−s) = −f(s)`) genera solo armoniche dispari. Per ottenere anche le
pari la semionda positiva e quella negativa saturano con un ginocchio diverso:

```
f(s) = (1/kp)·φ(kp·s)   per s ≥ 0           kp = 1 + asym        φ = combinazione SINE/TANH/HARD
f(s) = −(1/kn)·φ(−kn·s) per s < 0           kn = 1 − asym
```

Per SINE e TANH, sviluppando `φ(x) = x − c·x³ + …`, si trova `f(s) ≈ s − c·k²·s³` con `k = kp` o `kn`: la pendenza
nell'origine resta 1 da entrambe le parti (quindi nessun "gradino" al passaggio per lo zero) e la parte pari è
`−2·asym·c·abs(s)³`, proporzionale a `asym`. Per HARD, lineare fino al ginocchio, le due semionde cominciano a comprimere a
livelli diversi. In tutti i casi: `asym = 0` → solo dispari; `asym` crescente → rapporto pari/dispari crescente.
L'asimmetria genera anche una componente continua, che viene eliminata (sezione 12).

---

## 5. Anti-aliasing (ADAA) e il difetto trovato in saturazione profonda

Per non sovracampionare (scelta richiesta) si usa **ADAA di primo ordine** (Antiderivative Anti-Aliasing, Parker et al.
2016) applicato al residuo: invece di valutare la curva in un punto, si integra sul tratto percorso dal segnale tra
due campioni:

```
r[n] = ( H1(s[n]) − H1(s[n−1]) ) / ( s[n] − s[n−1] )          H1 = primitiva di h  (forma chiusa)
```

Se `|s[n] − s[n−1]| < 10⁻⁴` si usa il valore nel punto medio (limite della formula).

**Difetto trovato e corretto.** La media del residuo è centrata *a metà* tra due campioni, mentre la parte lineare
`x[n]` è al campione `n`. A livelli bassi non conta (il residuo è ≈ 0). In saturazione profonda invece `h(s) = c − s`,
la media toglie `(s[n]+s[n−1])/2` e l'uscita diventa `c + (s[n]−s[n−1])/2`: la semi-differenza dell'ingresso passa
**senza essere limitata**. Lo ha rivelato il test con segnali estremi (Input +24 dB, Output +12 dB, SAT 100 %, bande ±18 dB,
passa-alto 400/1000 Hz): prima della correzione il picco in uscita arrivava a 13.47 con rumore, 8.79 con un'onda quadra a
200 Hz e 8.04 con impulsi a fondo scala, contro 1.50, 1.42 e 0.67 dopo.

*Soluzione* (`AdaaResidual` in `Saturation.h`): un inseguitore d'inviluppo di `|s|` (attacco immediato, rilascio ~10 ms)
misura quanto si è dentro la saturazione e porta il residuo dalla forma pura (trasparente) all'ADAA completo:

```
r' = r − (1 − g(env)) · scala · (s[n] − s[n−1]) / 2        g = 1 sotto la soglia, 0 in saturazione profonda (smoothstep)
```

L'inviluppo varia lentamente, quindi la correzione è un filtro lineare quasi costante e non genera componenti nuove.
Una versione con gate istantaneo, provata in un prototipo Python, peggiorava l'aliasing: per questo l'inviluppo è lento.
Effetto: picco massimo dei segnali estremi 13.47 → 1.52; valvola +24 dB con un seno a 0 dBFS, da 200 Hz a 15 kHz: picco ≤ 0.41;
rumore a −6 dB RMS nella valvola a +24 dB: picco 0.39.

**Quanto aiuta l'ADAA** (misurato come potenza inarmonica / fondamentale; senza sovracampionamento):

| Caso | Senza ADAA | Con ADAA | Guadagno |
|------|-----------:|---------:|---------:|
| valvola +24 dB, 3 kHz, 44.1 kHz | −21.0 dB | −29.1 dB | 8.1 dB |
| trasformatore +6 dBFS, 5 kHz, 44.1 kHz | −35.3 dB | −39.7 dB | 4.4 dB |
| trasformatore +12 dBFS, 5 kHz, 44.1 kHz | −20.2 dB | −26.7 dB | 6.5 dB |

L'aliasing viene **ridotto, non eliminato**: pilotando forte le alte frequenze ne resta una quota misurabile. L'unico modo di
abbassarlo ancora è il sovracampionamento, che per scelta del progetto non c'è (sezione 16).

---

## 6. Input Gain: lo stadio valvolare

```
u = x · 10^(G/20)                                  G = Input Gain, −12 … +24 dB
κ = clamp(G / 24 dB, 0, 1)                         "spinta" della curva: 0 a 0 dB, 1 a +24 dB
y = [ u + DC( h(κ·u) / κ ) ] · 10^(−0.5·max(G,0)/20)
```

* `h` è il residuo di una **tanh asimmetrica** (asimmetria 0.22: la semionda positiva comprime di più, come per la
  corrente di griglia di una valvola; tetti 1/1.22 sulla positiva e 1/0.78 sulla negativa): armoniche **pari e dispari**.
* **Legato al gain.** Più Input Gain significa più livello nella curva *e* una curva più spinta (`κ` cresce con `G`).
  Con `G ≤ 0` lo stadio è esattamente lineare (`κ = 0`).
* **Compensazione dopo la non linearità** (toglie metà dei dB in salita): il volume percepito cresce meno del gain, ma la
  struttura armonica non cambia, solo il livello.
* Canali L e R separati (come un preamplificatore stereo).

Misure: livello basso → +6 dB di gain danno +3.0 dB, +12 → +6.0 dB, +24 → +12.0 dB; nessuna distorsione sotto 0 dB.

| Input Gain (−18 dB RMS) | +1 | +3 | +6 | +9 | +12 | +18 | +24 |
|-------------------------|---:|---:|---:|---:|----:|----:|----:|
| THD valvola | 0.001 % | 0.011 % | 0.091 % | 0.40 % | 1.37 % | 8.9 % | 24.1 % |

A +12 dB: H2 −41.1, H3 −39.6, H4 −59.2, H5 −76.2, H9 −147 dBc (H2/H3 = 0.84: pari e dispari bilanciate, spettro morbido).
A +18 dB la compressione rispetto al comportamento lineare è 0 dB a −40 dBFS e cresce in modo continuo: −0.1 dB a −30,
−0.7 a −20, −3.3 a −12, −7.6 a −6, −13.0 a 0 dBFS. Nessun DC residuo (media 0.000000). Pendenza della curva: variazione
massima 0.00094 per passo (un clipper netto: 1.0).

---

## 7. Saturazione di banda

Ogni banda ha un controllo **SAT** (0–100 %, predefinito 0 = spenta). Per ciascun canale (Mid e Side) la catena è

```
y   = campana(x)                                   peaking RBJ, larghezza in ottave pre-warped (come la 1.0)
u   = passabanda(y)                                0 dB al centro, stessa freq e stessa larghezza della campana
s   = drive · u                                    drive = 10^((20 + 6·SAT + trim)/20) · driveScale
r   = DC( ADAA( h_banda(s) ) / drive )             residuo, solo armoniche generate
out = y + wet · w_canale · r                       wet = SAT (0..1), w_canale = quota Mid/Side del fader
```

**Perché una "fetta" di spettro.** La saturazione lavora sulla parte di spettro che la banda controlla: banda stretta =
pochi componenti nel waveshaper (saturazione focalizzata, più dura), banda larga = molti componenti, quindi più
intermodulazione (la curva si ammorbidisce). La saturazione sta *dopo* la campana: +6 dB di boost sono +6 dB di
pilotaggio. Si somma soltanto il residuo, quindi non si ricombinano copie filtrate del segnale (niente comb filtering,
niente problemi di fase come in un crossover multibanda).

**Dove lavora.** Come per il gain, il fader Mid/Side decide: il canale scelto (Side se fader > 0, Mid se < 0) riceve la
saturazione intera, l'altro la quota `1 − |fader|/100`. Misura: fader +100 → Mid 0.00000 %, Side 6.75 %; fader −100 → il
contrario; fader 0 → 6.750 % su entrambi.

**Come SAT dosa la saturazione.** `wet` cresce linearmente da 0 (bypass esatto) a 1; il pilotaggio parte da +20 dB (il
ginocchio sta a circa −20 dBFS sul picco della fetta) e sale solo di 6 dB. È una scelta voluta: se il pilotaggio crescesse
molto, il tetto della curva `1/drive` scenderebbe e le armoniche generate su materiale forte *diminuirebbero* proprio
mentre SAT sale. Così crescono sempre. THD banda 1, tono al centro a −20 dBFS: 5 % → 0.30 %, 10 % → 0.64 %, 25 % → 1.88 %,
50 % → 4.99 %, 75 % → 10.2 %, 100 % → 19.2 %. A −70 dBFS anche SAT = 100 % non cambia nulla (differenza 4.5·10⁻⁸); un tono
quattro ottave sotto la banda distorce 0.0008 % contro 4.99 % al centro.

### Come freq, larghezza e gain deformano la curva

La durezza della curva di una banda è

```
q = 0.5 − 0.40·freqTilt·φF + 0.30·bwTilt·φB + 0.12·φG + qOffset            (poi limitato a 0..1)
asym = polarity · ( asymBase + (asymMax − asymBase)·φG )

φF = clamp( log2(f / 632.455) / 4.9829 , −1, 1 )     −1 a 20 Hz … +1 a 20 kHz   (log, come la manopola FREQ)
φB = clamp( log2(0.77 / w) / 2.95 , −1, 1 )          +1 a 0.1 oct … −1 a 6 oct
φG = clamp( |gain| / 18 dB , 0, 1 )
```

I coefficienti `freqTilt`, `bwTilt`, `qOffset`, `asymBase`, `asymMax`, `polarity` sono diversi per ogni banda (sezione 8).

| Leva | Effetto sulla curva | Misura |
|------|---------------------|--------|
| **Frequenza ↑** | più morbida: armoniche che decadono in fretta (e quelle alte cadrebbero oltre Nyquist: meno aliasing) | armoniche alte/H3: banda a 202 Hz 0.025, a 3 kHz 0.010 |
| **Larghezza ↑** | più morbida (più componenti nella curva → più intermodulazione) | armoniche alte/H3: banda stretta 0.047, larga 0.007 |
| **\|Gain\| ↑** | un po' più dura e soprattutto più asimmetrica: rapporto pari/dispari crescente | H2/H3: 0.04 a 0 dB → 0.57 a +12 dB (e 0.57 anche a −12 dB) |

La deformazione è continua: la durezza non cresce mai con la frequenza né con la larghezza (controllato su tutta la
griglia dei valori). Gli estremi: `q = 0` è la curva SINE, spettro che decade ripido; `q = 1` è la curva HARD, che pilotata
forte tende all'onda quadra (armoniche che decadono piano, aggressive).

**Stessa dose, timbro diverso.** Le tre primitive iniziano a comprimere a livelli diversi (punto a −1 dB: 0.82 per SINE, 0.615
per TANH, 1.12 per HARD). Senza correzione una curva "dura" sembrerebbe molto meno saturata di una "morbida" con la stessa
manopola. `driveScale = 0.615 / (wS·0.82 + wT·0.615 + wH·1.12)` riporta il punto a −1 dB sempre allo stesso livello: la manopola
SAT dà la stessa quantità di saturazione su ogni banda e cambia solo il timbro (THD delle tre bande entro 0.9 dB a parità di
SAT).

---

## 8. Identità deterministica delle bande

Ogni banda ha un **seed a 64 bit persistente**. Da questo, con SplitMix64 (generatore deterministico, nessuna dipendenza
dal sistema), si ricavano sette numeri che compongono il *carattere* timbrico della banda:

| Campo | Intervallo | Cosa cambia |
|-------|------------|-------------|
| `qOffset` | −0.18 … +0.18 | sposta la durezza della curva |
| `asymBase` | 0.01 … 0.09 | asimmetria (pari/dispari) a gain 0 dB |
| `asymMax` | 0.14 … 0.28 | asimmetria a ±18 dB di gain |
| `freqTilt` | 0.6 … 1.4 | quanto la frequenza sposta la durezza |
| `bwTilt` | 0.6 … 1.4 | quanto la larghezza sposta la durezza |
| `driveTrimDb` | −1.5 … +1.5 dB | piccola correzione del pilotaggio |
| `polarity` | ±1 | quale semionda satura per prima (fase della 2ª armonica) |

La "casualità" sta quindi nella **scelta della funzione di saturazione**, non nel segnale: non c'è rumore, il risultato è
una funzione pura dei parametri.

**Persistenza.** I seed sono salvati nello stato del plugin (`satSeed1`, `satSeed2`, `satSeed3`, in esadecimale, più
`satAlgo` = versione dell'algoritmo seed → carattere). Riaprendo il progetto, ricaricando un preset o aprendo una seconda
istanza il carattere è identico. Test: lo stesso stato caricato due volte dà audio identico bit per bit; salvataggi e
ripristini ripetuti mantengono i seed; due istanze con gli stessi parametri producono lo stesso audio bit per bit. Uno
stato senza seed (preset 1.0) riceve i seed di fabbrica.

**Seed di fabbrica**, scelti una volta tra 500 candidati massimizzando la differenza di *forma* armonica tra le tre bande a
parità di intensità, e poi congelati:

```
banda 1: 0xCB8789E97A8D571C      banda 2: 0xA05417BB5FB3086E      banda 3: 0xCBD670BB78664394
```

A parità di parametri (stessa frequenza, larghezza, gain) le tre bande risultano diverse:

| | H2/H3 | armoniche alte/H3 | THD |
|---|------:|------------------:|----:|
| banda 1 | −16.8 dB | −20.3 dB | −20.6 dB |
| banda 2 | −12.1 dB | −23.9 dB | −21.4 dB |
| banda 3 | −15.3 dB | −26.8 dB | −20.9 dB |

(distanza di forma tra due bande qualsiasi: 4.3–6.7 dB; differenza di dose ≤ 0.9 dB). Cambiare il seed di una banda cambia il
suo carattere (5.1 dB di differenza armonica); rimettendo quello originale il suono torna identico bit per bit.

Il plugin non espone un controllo per cambiare i seed (non era richiesto): ogni banda ha un'identità fissa. `setSeed()`
esiste per i test e per eventuali sviluppi futuri.

---

## 9. Interazione seriale tra le bande

Le bande sono in cascata: la banda 2 riceve ciò che esce dalla banda 1 (campana *e* armoniche generate dalla sua
saturazione), la banda 3 il risultato di entrambe. Le armoniche create da una banda cadono nella fetta di spettro della
successiva e vengono deformate a loro volta: nasce intermodulazione, e l'ordine delle bande conta. I controlli restano
indipendenti: ogni banda cambia solo i propri filtri e la propria curva.

Come lo si dimostra (test F4). Si sottrae l'uscita con una sola banda attiva da quella con due bande attive: in una somma
*in parallelo* il termine d'interazione sarebbe esattamente zero (−∞ dB).

| Coppia | Residuo prima banda | Residuo seconda banda | Termine d'interazione |
|--------|--------------------:|----------------------:|----------------------:|
| banda 1 → 2 | −27.5 dBFS | −76.6 dBFS | −74.6 dBFS |
| banda 1 → 3 (con la 2 neutra in mezzo) | −27.5 dBFS | −71.5 dBFS | −69.7 dBFS |
| banda 2 → 3 | −29.4 dBFS | −71.5 dBFS | −69.3 dBFS |
| banda 1 → 2, segnale a −8 dBFS | −12.5 dBFS | −47.0 dBFS | −47.8 dBFS |

Il termine d'interazione pesa quanto il residuo della seconda banda (da +2.2 a −0.9 dB rispetto a esso): è la stessa
grandezza, non un artefatto numerico. Scambiando l'ordine di due bande con lo stesso seed il risultato cambia (differenza
−31.5 dBFS contro un residuo di distorsione di −11.1 dBFS: −20.4 dB), mentre con SAT a 0 l'ordine non conta (i filtri lineari
commutano: differenza 0.000000000). Cambiare frequenza, larghezza, fader e seed di due bande spente non altera in alcun modo
la terza (bit per bit).

---

## 10. Output Gain: lo stadio a trasformatore

Dopo la decodifica L/R il segnale passa dall'**Output Gain** (−24 … +12 dB) e da uno stadio che si comporta in modo diverso
dalla valvola: trasparente fino a 0 dBFS, poi compressione progressiva con armoniche dispari.

```
e = flusso(x)             shelf del 1° ordine: +0.35 (≈ +2.6 dB) sotto ~100 Hz, 1 sopra
s = 0.7 · e               il ginocchio comincia a s = 0.7  →  la zona lineare finisce ESATTAMENTE a |e| = 1 (0 dBFS)
r = 0.95 · (f(s) − s) / 0.7                    f = curva HARD, ginocchio parabolico di semi-larghezza 0.30 (s da 0.7 a 1.3)
y = x + flusso⁻¹(r)       l'inverso esatto dello shelf riporta il residuo alla risposta in frequenza originale
```

La curva risultante (per segnale in ingresso `e`, simmetrica):

```
|e| ≤ 1             y = e                                  zona morta del residuo: esattamente 0 (trasparente)
1 < |e| ≤ 1.857     y = e − 0.554·(|e| − 1)²                ginocchio graduale (pendenza continua)
|e| > 1.857         y = 0.05·|e| + 1.357                    la pendenza non va a zero: niente clipping netto
```

* **Trasparente sotto 0 dBFS**: picchi fino a −0.2 dBFS escono identici (diff 0.000000000 a regime).
* **Graduale**, non un limitatore: riduzione di guadagno sul fondamentale a +3 dBFS 0.30 dB, +6 → 1.69 dB, +9 → 4.01 dB,
  +12 → 6.53 dB; l'uscita continua a salire, piano, anche oltre +9 dBFS (con ingresso a +9 dBFS esce a +4.99 dBFS, con
  ingresso a +12 dBFS a +5.47 dBFS). Variazione massima della pendenza 0.0011 per passo (un clipper netto: 1.0).
* **Carattere diverso dalla valvola**: curva simmetrica → solo armoniche dispari. A +6 dBFS: H2 −172.9 dBc, H3 −19.2 dBc,
  H5 −36.7 dBc. Rapporto H2/H3: 0.84 per la valvola contro 0.0000 per il trasformatore.
* **Frequenza.** Il nucleo magnetico satura prima sulle basse frequenze (volt-secondi): lo shelf di flusso alza di 2.6 dB
  il livello visto dalla curva sotto ~100 Hz. A −1.5 dBFS un 60 Hz distorce 0.09 %, un 1 kHz 0 %. Quindi: sopra circa
  100 Hz la soglia è esattamente 0 dBFS; per il basso scende fino a −2.6 dBFS.
* **L'Output Gain pilota lo stadio**: un segnale a +3 dBFS satura (2.66 %) e con Output Gain −6 dB torna pulito (0 %);
  un segnale a −6 dBFS è pulito e con +9 dB satura (2.65 %).
* Un avvio brusco di un tono a −0.2 dBFS può produrre una deviazione massima di −84.8 dBFS (misurata sui primi 85 ms): il
  transitorio dello shelf di flusso attraversa per un attimo la zona morta.

---

## 11. I due filtri passa-alto

Due controlli **LOW CUT** (uno sulla banda 1, uno sulla banda 3), automatizzabili; al minimo (≤ 20.5 Hz) il filtro è spento.

| | Banda 1 | Banda 3 |
|---|---------|---------|
| Tipo | 1° ordine, bilineare con pre-warping | 2° ordine, RBJ, Q = 0.6 (senza picco) |
| Pendenza | **6 dB/ottava** (molto graduale) | **12 dB/ottava** (più ripida) |
| Range | 20 – 400 Hz | 20 – 1000 Hz |
| A fc | −3.01 dB | −4.44 dB |
| 2 ottave sotto fc | −12.3 dB | −24.3 dB |
| Posizione | **prima** della banda 1 | **dopo** la banda 3 |

* Il LOW CUT 1 sta prima della saturazione: la banda distorce meno il basso filtrato (basso a 60 Hz: THD 31.3 % senza filtro,
  5.8 % con taglio a 400 Hz).
* Il LOW CUT 3 sta dopo tutta la catena, quindi filtra anche ciò che le bande hanno generato: con un tono sotto il taglio la
  terza armonica sale di 15.8 dB rispetto alla fondamentale, esattamente come prevede la risposta del filtro (prova che il
  filtro agisce dopo la saturazione).
* Accensione/spegnimento tramite *mix* smussato (30 ms): nessun click; da spento il filtro è un bypass esatto.
* Il grafico li disegna (linee tratteggiate "LC1" / "LC3" e curve Mid/Side comprensive) con gli stessi coefficienti
  dell'audio: scarto massimo grafico/audio 0.0008 dB a 96 kHz.

---

## 12. Qualità del DSP

| Tema | Soluzione | Verifica |
|------|-----------|----------|
| **Aliasing** | ADAA1 sul solo residuo, nessun sovracampionamento (sezione 5) | riduzione di 4.4–8.1 dB rispetto al calcolo puntuale |
| **DC** | blocco del DC (passa-alto 1° ordine a 8 Hz) applicato solo al residuo; il percorso lineare non viene toccato | media 0.000000 anche con valvola a +12 dB, banda al 100 %, trasformatore |
| **Denormali** | `ScopedNoDenormals` (flush-to-zero) in `processBlock` | dopo 13 s di silenzio l'uscita è esattamente 0 |
| **Allocazioni / lock** | nulla nel thread audio; il buffer mono è allocato in `prepareToPlay` | 0 allocazioni in 400 blocchi (stereo e mono) |
| **Automazione** | parametri smussati 30 ms, coefficienti ricalcolati ogni 32 campioni, rampe per campione sui gain; stato ADAA riscalato quando cambia il pilotaggio | nessun click: salto massimo tra campioni confrontato con quello della sinusoide pulita (es. SAT: 0.0198 contro 0.0196) |
| **NaN / infiniti** | sanificati in ingresso, non entrano mai negli stati dei filtri | dopo un NaN il segnale riprende normalmente |
| **Blocchi** | risultato indipendente dalla dimensione del blocco | identico bit per bit con blocchi da 1, 7, 64, 480, 4096 |
| **Mono / stereo** | codifica `M=(L+R)/2`, `S=(L−R)/2`, decodifica `L=M+S`, `R=M−S` (inversa esatta); il layout mono equivale al Mid dello stereo | mono = Mid stereo (diff 0.000000000); un segnale mono resta mono (L = R bit per bit) |
| **Fase / ricombinazione** | niente crossover o copie filtrate sommate: si aggiunge il solo residuo non lineare | nessuna cancellazione di fase |
| **Frequenze di campionamento** | coefficienti dipendenti dal sample rate | 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz: distorsione di banda entro 0.05 dB, grafico = audio entro 0.0017 dB |
| **CPU** | tutto in double, senza allocazioni | 10 s di audio stereo a 48 kHz: 90.7 ms con tutti gli stadi attivi (110× tempo reale, 0.91 % di un core); l'EQ come la 1.0 7.4 ms |

---

## 13. Gain staging

| Stadio | Effetto sul livello |
|--------|---------------------|
| **Input Gain** `G ≤ 0` | `G` dB esatti, nessuna distorsione |
| **Input Gain** `G > 0` | a basso livello **+G/2 dB** (l'altra metà viene compensata dopo la valvola); a livelli alti compressione progressiva |
| **Gain di banda** | come nella 1.0: ±18 dB, ripartiti su Mid e Side dal fader |
| **SAT di banda** | a basso livello nessun effetto; su materiale molto forte riduce il livello di al massimo 2.2 dB (rumore, SAT 100 % su tutte le bande) e non lo alza mai (massimo −0.0 dB) |
| **Output Gain** | `G` dB esatti; satura (trasformatore) solo se il segnale supera 0 dBFS |

Relazione prevedibile a basso livello: `guadagno netto = (G_in se ≤ 0, G_in/2 se > 0) + gain di banda + Output Gain`.
Verifica (test F7: tono a −60 dBFS, banda a +6 dB con SAT 100 %, Output Gain −3 dB, Input Gain da −12 a +24 dB): il guadagno
netto misurato coincide con la formula entro 0.04 dB (da −9.00 a +14.96 dB, attesi −9.00 e +15.00); sulla valvola da sola,
Input Gain +24 dB → +11.995 dB misurati contro +12.000.

---

## 14. Compatibilità con la 1.0

* I 12 parametri originali (`b1_freq … b3_ms`) hanno gli stessi ID, lo stesso intervallo, lo stesso ordine: automazioni e
  preset salvati con la 1.0 restano validi.
* I 7 nuovi parametri sono accodati (versione 2 del parametro): `in_gain`, `out_gain`, `b1_sat`, `b2_sat`, `b3_sat`,
  `hpf1_freq`, `hpf3_freq`, tutti con valore predefinito neutro (0 dB, 0 %, OFF).
* Caricando uno stato della 1.0 i parametri nuovi tornano ai valori neutri (anche se prima erano spostati) e i seed a quelli
  di fabbrica: il preset suona come quando è stato salvato.
* Totale: 19 parametri automatizzabili (più il bypass del DAW), 16 pomelli + 3 fader nell'interfaccia.
* Novità non retroattive: layout **mono** supportato (la 1.0 era solo stereo).

---

## 15. Risultati misurati

Riepilogo del test completo (`ValSTTests.exe`, compilazione Release; 284 controlli sul codice + 27 sul VST3 caricato, 0 falliti):

| Area | Controlli principali |
|------|----------------------|
| 1.0 (regressione) | mappatura Mid/Side e fader log, trasparenza bit-exact, forma della campana, grafico = audio, blocchi, click, salvataggio |
| F1 Input / valvola | trasparenza sotto 0 dB, compensazione, THD monotono, pari + dispari, curva liscia, DC, ADAA, L/R indipendenti, mono, denormali, NaN |
| F2 banda singola | SAT 0 = campana lineare (diff 1.6·10⁻⁸), SAT monotono, effetto di gain/frequenza/larghezza, fader M/S, click, blocchi |
| F3 tre bande | stesse regole, tre identità distinte, stessa dose, indipendenza dei controlli, uscita limitata |
| F4 interazione | termine d'interazione reale per le coppie 1→2, 1→3, 2→3; ordine delle bande; filtri lineari commutano |
| F5 uscita | trasparenza < 0 dBFS, compressione graduale, solo dispari, dipendenza dalla frequenza, ADAA, L/R, mono, click |
| Saturazione profonda | picchi limitati a tutte le frequenze per valvola, banda e trasformatore |
| F6 passa-alto | risposta misurata = teorica a 44.1/48/96 kHz, pendenze 6 e 12 dB/ott, OFF = bypass, posizione, click |
| F7 integrazione | gain staging, mono = Mid, estremi (picco ≤ 1.52, DC ≤ 0.0001), blocchi, stato, preset 1.0, 0 allocazioni, automazione casuale su 19 parametri a 44.1/96/192 kHz, multi sample rate, CPU |
| GUI | 16 pomelli, 3 fader, interazione col grafico, snapshot, apertura/chiusura ripetuta |
| VST3 vero | il file `.vst3` estratto dallo zip viene scansionato e caricato con l'host di JUCE: 19 parametri + Bypass, raggiungibili per nome; accendendo uno stadio alla volta (banda 2, Input, Output, SAT 1/2/3, LOW CUT 1/3) l'audio è **identico bit per bit** a quello del codice in-process a ogni passo |

---

## 16. Scelte interpretative e limiti noti

Dove la richiesta lasciava margine, ecco cosa è stato deciso e perché; ognuno di questi punti si può cambiare.

1. **Nessun sovracampionamento.** La richiesta indica di evitare filtri di anti-aliasing che richiedano sovracampionamento.
   Si usa ADAA1 sul residuo: riduce l'aliasing di 4–8 dB ma non lo elimina (sezione 5). Con pilotaggi molto alti su
   contenuto di alta frequenza resta una quota udibile/misurabile.
2. **"Triangolare → quadra".** Una saturazione monotona non può produrre una vera onda triangolare (servirebbe una curva che
   *espande*). L'estremo morbido è la curva SINE, con spettro che decade ripido (come i vertici smussati di un triangolo); quello
   duro la HARD, che tende alla quadra. Il passaggio è continuo (sezione 7).
3. **Controlli aggiunti.** SAT per banda (necessario per poter spegnere la saturazione e restare compatibili con la 1.0) e due
   manopole LOW CUT (frequenza dei due passa-alto, OFF di default). Più IN e OUT.
4. **Valvola per canale (L/R).** Come un preamplificatore stereo: un segnale solo-Side può generare un po' di Mid attraverso
   le armoniche pari.
5. **Compensazione della valvola al 50 %.** Metà dei dB in salita: il volume percepito cresce, ma meno del gain. Costante
   `valveCompression` in `Saturation.h`.
6. **Trasformatore solo da 0 dBFS.** Sotto 0 dBFS il segnale resta identico alla 1.0; il basso (< ~100 Hz) comincia fino a 2.6 dB
   prima (comportamento volutamente "magnetico").
7. **Passa-alto.** Banda 1: 1° ordine; banda 3: 2° ordine con Q 0.6. L'ordine e il Q della banda 3 sono `hpf3Q` e
   `makeBand3HighPass` in `MsEqDsp.h`.
8. **Il carattere deriva dal seed, ma non c'è un controllo per cambiarlo.** Un eventuale pulsante "nuovo carattere" richiederebbe
   solo di scrivere un nuovo seed nello stato.
9. **Non verificato in un DAW commerciale.** I test caricano il VST3 reale con l'host di JUCE (parametri, stato, audio). I file
   non sono firmati digitalmente.
10. **In saturazione profonda l'uscita dipende un po' dalla storia recente del segnale**: la correzione di allineamento segue
    un inviluppo con rilascio di 10 ms, quindi non coincide istante per istante con la curva statica. È il prezzo per non far
    passare la semi-differenza dell'ingresso senza limiti (sezione 5).

---

## 17. Come riprodurre le misure

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVALST_BUILD_TESTS=ON
cmake --build build --config Release --target ValSTTests
build\ValSTTests_artefacts\Release\ValSTTests.exe --vst3 "build\ValSTMSEQ_artefacts\Release\VST3\ValST MS EQ.vst3" --out snapshot
```

L'opzione `--vst3` carica anche il plugin compilato come farebbe un DAW e lo confronta con il motore interno (19 parametri +
bypass, audio confrontato stadio per stadio con tolleranza 10⁻⁶: in pratica identico bit per bit); `--out` salva gli snapshot
dell'interfaccia. Il test completo gira in pochi secondi.
