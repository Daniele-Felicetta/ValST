# ValST MS EQ

<p align="center">
  <img src="docs/anteprima.png" alt="Interfaccia di ValST MS EQ" width="860">
</p>

Equalizzatore parametrico **Mid/Side a 3 bande**: plugin **VST3** per Windows (64 bit), costruito con JUCE 8.

**[⬇ Scarica l'ultima versione](https://github.com/Daniele-Felicetta/ValST/releases/latest)** (file `ValST_MS_EQ_v…_Windows_x64.zip`)

## Installazione

1. Scarica lo zip dalla pagina delle [Release](https://github.com/Daniele-Felicetta/ValST/releases/latest) ed **estrai tutto il contenuto** in una cartella.
2. Doppio clic su **`Installa.bat`** e conferma la richiesta di Windows: il plugin viene copiato in
   `C:\Program Files\Common Files\VST3`, la cartella standard letta da tutti i DAW.
3. Apri il DAW; se **ValST MS EQ** non compare, rifai la scansione dei plugin VST3.

Per rimuoverlo: **`Disinstalla.bat`**. Non serve installare nient'altro: il runtime C++ è già dentro il plugin.

> I file non sono firmati digitalmente: Windows può mostrare un avviso di sicurezza, conferma per procedere.
> Se preferisci, compila il plugin dal sorgente (vedi [Ricompilare](#ricompilare)).

## Cosa fa

Ogni banda ha quattro controlli:

| Controllo | Funzione |
|-----------|----------|
| **FREQ**  | frequenza centrale della campana, 20 Hz – 20 kHz |
| **WIDTH** | larghezza della campana in ottave (0.1 – 6); più alto = più larga |
| **GAIN**  | guadagno della banda, ±18 dB |
| **Fader Mid/Side** | accanto al GAIN: decide su quale segnale lavora il GAIN (vedi sotto) |

### Il fader Mid/Side (scala logaritmica, da -100 a +100)

| Posizione | Effetto del pomello GAIN |
|-----------|--------------------------|
| **0** (centro) | **neutro**: agisce sul segnale stereo, cioè su Mid e Side insieme |
| verso **+100** (in alto) | agisce sul **Side**; il Mid ne riceve sempre meno, a +100 resta invariato |
| verso **-100** (in basso) | agisce sul **Mid**; il Side ne riceve sempre meno, a -100 resta invariato |

In formule, con `G` = valore del GAIN in dB e `f` = valore del fader:

```
quota   = 1 - |f| / 100
canale scelto (Side se f > 0, Mid se f < 0)  ->  G dB
altro canale                                  ->  G * quota dB
```

La corsa del fader è **logaritmica**: il valore mostrato (da -100 a +100) è legato alla posizione fisica del
cursore da una legge a decade, `valore = 100 * (10^t - 1) / 9` (con `t` = distanza dal centro, da 0 a 1).
Intorno allo 0 c'è molta corsa per pochi punti (regolazione fine, utile per dosare leggermente Mid/Side),
verso gli estremi i valori cambiano più in fretta. La scala disegnata a fianco del fader segue la stessa legge.
La definizione è in `msRange()` (`Source/Params.h`).

### Il grafico

* curva **ambra** = risposta del segnale **Mid** (somma delle 3 bande);
* curva **ciano** = risposta del segnale **Side** (somma delle 3 bande);
* con il fader a 0 le due curve coincidono;
* i nodi numerati sono le bande: trascinali (orizzontale = frequenza, verticale = guadagno), rotella del mouse
  sul nodo = larghezza, doppio clic = azzera il guadagno;
* passando sopra un nodo, o sul pannello di una banda, si vedono le curve della singola banda e i valori esatti
  applicati a Mid e Side.

Il grafico usa gli stessi coefficienti dei filtri audio: quello che vedi è quello che senti.
Il pomello GAIN e i nodi cambiano colore con il fader: bianco = stereo, ciano = Side, ambra = Mid.
Doppio clic su qualsiasi controllo = valore predefinito. La finestra si ridimensiona dall'angolo in basso a destra.

## Dettagli tecnici

* Codifica `M = (L+R)/2`, `S = (L-R)/2`; tre filtri *peaking* (RBJ, in double, con larghezza in ottave
  pre-warped) per ciascun canale; decodifica `L = M+S`, `R = M-S`. Con tutti i gain a 0 il segnale esce
  identico bit per bit. Latenza zero.
* Parametri smussati (30 ms): nessun click durante le automazioni.
* 12 parametri automatizzabili (`b1_freq`, `b1_width`, `b1_gain`, `b1_ms`, … `b3_ms`) più il bypass del DAW.
* Solo stereo 2 in / 2 out.

## Ricompilare

Requisiti: Visual Studio 2022 (o Build Tools) con il carico di lavoro *Sviluppo di applicazioni desktop con C++*,
CMake e git. Poi basta un doppio clic su **`build.bat`**: scarica JUCE (la prima volta), compila in Release e
genera la cartella `dist`, pronta da installare.

Test automatici (matematica del fader, guadagni su Mid/Side, accordo grafico/audio, assenza di click, caricamento
del VST3 vero come farebbe un DAW, interazione con il grafico, snapshot della GUI):

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVALST_BUILD_TESTS=ON
cmake --build build --config Release
build\ValSTTests_artefacts\Release\ValSTTests.exe --vst3 "build\ValSTMSEQ_artefacts\Release\VST3\ValST MS EQ.vst3" --out snapshot
```

```
Source/Params.h          parametri, range, scala logaritmica del fader, testi
Source/MsEqDsp.h         filtri, distribuzione del gain tra Mid e Side, motore stereo
Source/PluginProcessor   plugin (bus, stato, parametri)
Source/EqGraph           grafico EQ interattivo
Source/PluginEditor      pannelli delle bande e finestra
Source/LookAndFeel.h     colori, pomelli, fader
Tests/Tests.cpp          test automatici
Installer/               Installa.bat, Disinstalla.bat, LEGGIMI.txt (copiati in dist)
docs/                    anteprima dell'interfaccia
```

Per cambiare nome, produttore o categoria del plugin: `CMakeLists.txt`, blocco `juce_add_plugin`.

## Licenza

Il codice è rilasciato con licenza **GNU AGPL v3.0** (vedi [`LICENSE`](LICENSE)): puoi usarlo, studiarlo,
modificarlo e ridistribuirlo, purché le versioni derivate (anche solo in forma binaria) restino sotto AGPLv3
con il sorgente disponibile.

La scelta deriva da [JUCE](https://juce.com), distribuito con doppia licenza AGPLv3 / commerciale: per usare
questo codice in un prodotto chiuso serve una licenza commerciale JUCE.

Copyright (C) 2026 Daniele Felicetta
