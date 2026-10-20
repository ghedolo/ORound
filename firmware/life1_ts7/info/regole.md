# life1_ts7: regole

Descrizione completa del comportamento del firmware, sufficiente per rifarlo.

## Convenzioni

- Display rotondo 466 x 466 px, 1 px = 0,072 mm. Coordinate dei pixel: x da 0 a 465 verso destra, y da 0 a 465 verso il basso. Il centro del pixel (x, y) è il punto (x + 0,5; y + 0,5).
- Le tessere si costruiscono in coordinate centrate: X = x - 233, Y = y - 233 (Y verso il basso). Il centro del display è (0, 0), il cerchio visibile ha raggio 233 px.
- Angoli in gradi; un angolo positivo gira da +X verso +Y, cioè in senso orario sullo schermo.
- Colori RGB a 8 bit per canale (0..255).
- Tempi in secondi; il firmware misura il tempo in microsecondi dall'accensione.
- "Numero casuale" = numero uniforme in [0, 1] dal generatore hardware, senza seme fisso: ogni accensione è diversa.
- "Centro di una tessera" = media dei suoi vertici.
- Valori di partenza: lato del rombo di base 12 px (0,86 mm), 371 tessere. BOOT cambia la dimensione (vedi "Pulsanti").

## Tessere

Tassellatura isometrica progettata a partire da `data/tassel/fake_3d2.jpg`. L'immagine contiene la girandola di tre pezzi a L di `fake_3d1.jpg` e, intorno, dei cubi che chiudono la figura; i cubi non entrano nel periodo di `fake_3d1`, quindi il periodo è stato cercato con una ricerca esatta: ogni periodo contiene i tre pezzi a L, un cubo e tre parallelogrammi. Con S = lato del rombo di base in px, unità ux = S x radice di 3 / 2 in X e uy = S / 2 in Y:

1. Griglia di triangoli equilateri di lato S con lati verticali; i suoi punti sono (i, m) in unità con i + m pari. Tutti i vertici hanno coordinate intere.
2. Il motivo si ripete con i vettori V1 = (-5; 3) e V2 = (-2; -12) in unità (66 triangoli per periodo). Ogni periodo contiene 9 tessere, con vertici in unità attorno all'origine del periodo, in quest'ordine:
   - pezzo magenta (a L, 8 vertici): (-3; -1), (-2; 0), (0; -2), (0; 0), (1; 1), (1; -3), (-1; -5), (-1; -3);
   - pezzo bianco (a L): (-1; 1), (1; 3), (3; 1), (2; 0), (2; -4), (1; -3), (1; 1), (0; 0);
   - pezzo blu (a L): (-2; 0), (0; -2), (0; 0), (-1; 1), (1; 3), (1; 5), (-1; 3), (-2; 4);
   - cubo, faccia sopra (bianca): (-7; 11), (-9; 9), (-7; 7), (-5; 9);
   - cubo, faccia sinistra (blu): (-7; 11), (-9; 9), (-9; 13), (-7; 15);
   - cubo, faccia destra (magenta): (-7; 11), (-7; 15), (-5; 13), (-5; 9);
   - parallelogramma grigio: (-4; -8), (-2; -6), (-1; -7), (-3; -9);
   - parallelogramma grigio: (-6; 14), (-6; 18), (-5; 19), (-5; 15);
   - parallelogramma grigio: (-6; -4), (-6; 0), (-7; 1), (-7; -3).
3. Costruzione: con g = parte intera superiore di 233 / (radice di 21 x S), più 3, per b da -g a g e dentro per a da -g a g: origine del periodo = a V1 + b V2; le 9 tessere nell'ordine sopra. Vertici della tessera = (origine + vertice) x (ux, uy).
4. Si tengono le tessere con il centro a distanza al massimo 233 dal centro del display.

Risultato con S = 12: 371 tessere. Pezzi a L: 10 triangoli, area 4,33 S^2 = 624 px2. Facce del cubo: rombi di lato 2 S, 8 triangoli, area 3,46 S^2 = 499 px2. Parallelogrammi: lati S e 2 S, 4 triangoli, area 1,73 S^2 = 249 px2. Lato medio 21,4 px. Per colore: 82 magenta, 83 bianche, 83 blu (pezzi a L e facce del cubo), 123 grigie.

Colori per colore dell'immagine, per mantenere l'effetto 3D:

- tessera morta, solo toni di blu e di grigio: magenta grigio blu (32, 36, 48), bianco grigio chiaro (44, 44, 48), blu blu scuro (12, 30, 50), grigio grigio scuro (24, 24, 28);
- tessera viva: il colore per età (vedi "Colore obiettivo") moltiplicato per 0,85 (magenta), 1,00 (bianco), 0,70 (blu), 0,55 (grigio).

Vicine di una tessera: le tessere con almeno un vertice in comune (vertici moltiplicati per 4 e arrotondati all'intero più vicino; le coordinate in unità sono intere). Con S = 12 da 1 a 10, bordo compreso; la maggior parte ne ha 5 o 6.

## Stato di ogni tessera

- viva (sì/no) adesso;
- viva (sì/no) prima dell'ultima generazione, chiamato "stato precedente";
- età: numero intero da 0 a 12;
- momento: numero intero da 0 a 9;
- due colori interni, "colore intermedio" e "colore mostrato", ciascuno con tre valori reali.

All'accensione e a ogni cambio di dimensione la tassellatura viene costruita e tutte le tessere partono morte, con stato precedente morto, età 0 e colori interni neri (0, 0, 0). Subito dopo si generano tessere casuali (vedi "Nuove tessere casuali"); quindi tutto compare sfumando dal nero.

## Tempi che dipendono dalla dimensione

- Con D = lato del rombo di base in px: fattore = 1 + 0,06 x (D - 10) se D > 10, altrimenti 1.
- Durata della generazione G = 4 s x fattore (in microsecondi interi, parte frazionaria scartata).
- Costante di tempo della sfumatura T = 1,1 s x fattore.
- Alla dimensione di partenza: G = 4,48 s, T = 1,232 s.

## Generazione (ogni G secondi)

La prima generazione corrente inizia al primo fotogramma. A ogni fotogramma, prima di calcolare i colori, si controlla il tempo: se dall'inizio della generazione corrente sono passati almeno G secondi, si esegue una generazione e la generazione corrente inizia in quel fotogramma.

Una generazione fa, in quest'ordine:

1. Per ogni tessera conta le vicine vive (stato attuale, prima di cambiare qualunque tessera) e decide il nuovo stato con la regola B2/S23:
   - viva con 2 o 3 vicine vive: resta viva;
   - viva con un altro numero di vicine vive (0, 1, 4 o più): muore;
   - morta con esattamente 2 vicine vive: nasce;
   - morta con un altro numero: resta morta.
   La regola non è quella di Conway (B3/S23): con quasi sempre 5..6 vicine, B3/S23 si blocca in 15..26 generazioni, B2/S23 continua a cambiare.
2. Età, per ogni tessera con nuovo stato vivo: se era morta l'età diventa 0, se era viva l'età sale di 1 fino a un massimo di 12. Per le tessere con nuovo stato morto l'età non cambia (una tessera appena morta conserva l'età che aveva).
3. Ogni tessera, cambiata o no, riceve un nuovo momento casuale da 0 a 9 (tutti i valori con la stessa probabilità).
4. Lo stato precedente di ogni tessera diventa lo stato attuale; lo stato attuale diventa il nuovo stato.
5. Controllo di ripetizione e di assenza di vita (vedi "Ripartenza automatica").

## Colore obiettivo di una tessera

A ogni fotogramma, per ogni tessera:

- La tessera è "partita" se dall'inizio della generazione corrente sono passati almeno momento x G / 10 (in microsecondi interi). Alla dimensione di partenza gli istanti sono distanziati di 0,448 s.
- Stato da mostrare: lo stato attuale se la tessera è partita, altrimenti lo stato precedente. Così ogni cambio compare in uno di 10 istanti diversi della generazione.
- Se lo stato da mostrare è vivo, il colore obiettivo dipende dall'età. Colori di riferimento: C0 = arancio (255, 140, 30), C1 = giallo (255, 230, 40), C2 = verde (60, 255, 70). Con a = età / 6, j = parte intera di a ma al massimo 1, f = a - j: colore = Cj + f x (C(j+1) - Cj), canale per canale. Età 0 arancio, età 6 giallo, età 12 verde.
- Se lo stato da mostrare è morto, il colore obiettivo è il colore delle tessere morte del suo tipo (vedi "Tessere").

## Sfumatura (a ogni fotogramma)

- dt = tempo trascorso dal fotogramma precedente (circa 0,038 s, il firmware fa circa 26 fotogrammi al secondo; al primo fotogramma dt = 0).
- k = 1 - e^(-dt / T).
- Per ogni canale: colore intermedio = colore intermedio + (colore obiettivo - colore intermedio) x k; poi colore mostrato = colore mostrato + (colore intermedio - colore mostrato) x k.
- Effetto: due filtri esponenziali in serie con costante di tempo T. Dopo un cambio del colore obiettivo il colore mostrato parte piano, accelera e rallenta alla fine; arriva al 50% in 1,68 T e al 90% in 3,89 T (alla dimensione di partenza 2,1 s e 4,8 s).

## Disegno

- A ogni pixel si assegna al più una tessera. Si esaminano le tessere nell'ordine di costruzione; per ognuna, ogni pixel il cui centro (in coordinate centrate) è dentro il poligono riceve quella tessera, sostituendo un'eventuale tessera precedente.
- "Dentro il poligono": regola del raggio orizzontale. Dal centro del pixel si traccia una semiretta verso +X e si contano i lati attraversati; un lato da (X1, Y1) a (X2, Y2) conta se uno dei due estremi ha Y maggiore del punto e l'altro no, e il punto di incontro ha X maggiore della X del pixel. Il pixel è dentro se il conteggio è dispari. Con questa regola un pixel sul lato comune tra due tessere va a una sola delle due.
- Bordo nero: con B = 0 (nessun bordo nero tra le tessere), se B > 0 un pixel dentro resta senza tessera quando la sua distanza da almeno un lato del poligono (segmento) è minore di B. La linea nera tra due tessere è larga 2 B. Alla dimensione di partenza: nessun bordo a nessuna dimensione.
- Ogni pixel con una tessera prende il colore mostrato della tessera, arrotondato all'intero più vicino. I pixel senza tessera sono neri.

## Ripartenza automatica

Alla fine di ogni generazione:

- Si calcola un'impronta dello stato nuovo: si parte da 2166136261 e, per ogni tessera nell'ordine di costruzione, si fa XOR con 1 se la tessera è viva o con 0 se morta e si moltiplica per 16777619, tenendo solo 32 bit (impronta FNV-1a).
- Se l'impronta è uguale a una delle ultime tre, il contatore di ripetizione sale di 1, altrimenti torna a 0. Così si riconoscono stati fissi e cicli di 2 o 3 generazioni. Due stati diversi con la stessa impronta sono possibili ma molto rari.
- Il contatore di assenza sale di 1 se non c'è nessuna tessera viva, altrimenti torna a 0.
- Se il contatore di ripetizione supera 20 (cioè arriva a 21) o il contatore di assenza supera 3 (cioè arriva a 4), si generano nuove tessere casuali.

## Nuove tessere casuali

Per ogni tessera:

- lo stato precedente diventa lo stato attuale;
- nuovo momento casuale da 0 a 9;
- la tessera diventa viva se un numero casuale è minore di 0,30, altrimenti morta;
- se è diventata viva ed era morta, l'età diventa 0; se era già viva, l'età resta.

Poi il tempo della generazione riparte dal fotogramma precedente e i due contatori tornano a 0 (le ultime tre impronte restano). Le tessere che cambiano sfumano come in una generazione normale, ognuna nel suo momento.

## Tocco

- Al primo contatto e a ogni posizione letta mentre il dito scorre (circa 26 volte al secondo), nel pixel (x, y): si prende la tessera assegnata a quel pixel (vedi "Disegno"). Se il pixel non appartiene a nessuna tessera non succede niente.
- La tessera diventa viva sempre, ognuna delle sue vicine con probabilità 0,5 (un numero casuale minore di 0,5).
- Una tessera che diventa viva così: se era morta l'età diventa 0; stato attuale e stato precedente diventano vivi, quindi il colore obiettivo cambia subito senza attendere il momento; il colore mostrato sfuma comunque come sopra.
- Il contatore di ripetizione torna a 0.
- Il tocco non vale mentre è attiva la regolazione della luminosità.

## Luminosità

- Se il dito resta entro 15 px dal punto di primo contatto per più di 0,8 s, parte la regolazione della luminosità (il tocco ha già acceso le tessere sotto il dito al primo contatto).
- Durante la regolazione: posizione = posizione all'inizio + (y all'inizio - y attuale) / 300, limitata tra 0,15 e 1. Dito in alto = più luminoso. Valore inviato al display = 255 x posizione^2, arrotondato.
- Quando il dito si stacca la regolazione finisce e la posizione viene salvata in memoria permanente; all'accensione si riprende la posizione salvata (minimo 0,15), altrimenti 1.

## Pulsanti

- BOOT: a ogni pressione (antirimbalzo 150 ms) si passa al lato successivo tra 6, 8, 10, 12, 14, 16, 20, 24, 32, 40 px (dopo 40 si torna a 6). La tassellatura viene ricostruita, tutte le tessere ripartono nere e si generano nuove tessere casuali. A ogni accensione si riparte da 12 px.
- PWR tenuto premuto per più di 1,5 s, dopo essere stato rilasciato almeno una volta dall'accensione: spegnimento. Con la batteria l'alimentazione si interrompe. Con l'USB la scheda resta alimentata: il firmware attende il rilascio di PWR e va in sospensione profonda; la pressione successiva di PWR riaccende (il firmware riparte dall'inizio).

## File

- `preview.png`: anteprima del display alla dimensione di partenza, dopo 9 generazioni da tessere casuali.
- `tiles.png`: i tre pezzi a L, il cubo con le tre facce separate, i parallelogrammi e le vicine di un pezzo a L e di una faccia del cubo, separate.
