# life1_ts1: regole

Descrizione completa del comportamento del firmware, sufficiente per rifarlo.

## Convenzioni

- Display rotondo 466 x 466 px, 1 px = 0,072 mm. Coordinate dei pixel: x da 0 a 465 verso destra, y da 0 a 465 verso il basso. Il centro del pixel (x, y) è il punto (x + 0,5; y + 0,5).
- Le tessere si costruiscono in coordinate centrate: X = x - 233, Y = y - 233 (Y verso il basso). Il centro del display è (0, 0), il cerchio visibile ha raggio 233 px.
- Angoli in gradi; un angolo positivo gira da +X verso +Y, cioè in senso orario sullo schermo.
- Colori RGB a 8 bit per canale (0..255).
- Tempi in secondi; il firmware misura il tempo in microsecondi dall'accensione.
- "Numero casuale" = numero uniforme in [0, 1] dal generatore hardware, senza seme fisso: ogni accensione è diversa.
- "Centro di una tessera" = media dei suoi vertici.
- Valori di partenza: lunghezza delle frecce 28 px (2,02 mm), 436 tessere. BOOT cambia la dimensione (vedi "Pulsanti").

## Tessere

Tassellatura a frecce ricavata da `data/tassel/Alhambra.jpg`. Con A = lunghezza della freccia in px:

1. Griglia quadrata di passo A con un punto in (i A, j A) per ogni coppia di interi i, j. Un punto è "pari" se i + j è pari.
2. Ogni lato della griglia porta una freccia. Le frecce verticali hanno la punta sul punto pari del loro lato, quelle orizzontali sul punto dispari.
3. Freccia di riferimento, in unità A, con la punta in (0, 0) e il corpo verso +Y, 8 vertici in ordine: (0; 0), (-(1 - qy); qx), (-0,5; 0,5), (-qx; qy), (0; 1), (qx; qy), (0,5; 0,5), (1 - qy; qx), con qx = 629/1024 = 0,6143 e qy = 772/1024 = 0,7539 (misurati sull'immagine). (0, 1) è la tacca della coda. La freccia è simmetrica rispetto al suo asse e non è convessa.
4. Rotazione di un quarto di giro: (X, Y) diventa (-Y, X). Rotazione r = numero di quarti di giro.
5. Costruzione: con g = parte intera superiore di 233 / A, più 1, per j da -g a g e dentro per i da -g a g:
   - lato verticale da (i, j) a (i, j + 1): se (i, j) è pari, freccia con punta in (i, j) e r = 0; altrimenti punta in (i, j + 1) e r = 2;
   - lato orizzontale da (i, j) a (i + 1, j): se (i, j) è pari, punta in (i + 1, j) e r = 1; altrimenti punta in (i, j) e r = 3.
   Vertici della tessera = (punta + vertice di riferimento ruotato) x A. Prima la verticale, poi l'orizzontale.
6. Si tengono le tessere con il centro a distanza al massimo 233 dal centro del display.

Risultato con A = 28: 436 frecce, 220 verticali e 216 orizzontali, area A^2 / 2 = 392 px2 ciascuna, lato medio 19,8 px. Colore da morta: verticali (22, 22, 38), orizzontali (12, 12, 22).

Vicine di una tessera: le tessere con almeno un vertice in comune (vertici moltiplicati per 4 e arrotondati all'intero più vicino; i valori sono esatti perché qx e qy sono multipli di 1/1024). Ogni freccia lontana dal bordo ha 8 vicine; con A = 28 da 4 a 8.

## Stato di ogni tessera

- viva (sì/no) adesso;
- viva (sì/no) prima dell'ultima generazione, chiamato "stato precedente";
- età: numero intero da 0 a 12;
- momento: numero intero da 0 a 9;
- due colori interni, "colore intermedio" e "colore mostrato", ciascuno con tre valori reali.

All'accensione e a ogni cambio di dimensione la tassellatura viene costruita e tutte le tessere partono morte, con stato precedente morto, età 0 e colori interni neri (0, 0, 0). Subito dopo si generano tessere casuali (vedi "Nuove tessere casuali"); quindi tutto compare sfumando dal nero.

## Tempi che dipendono dalla dimensione

- Con D = lunghezza delle frecce in px: fattore = 1 + 0,03 x (D - 20) se D > 20, altrimenti 1.
- Durata della generazione G = 4 s x fattore (in microsecondi interi, parte frazionaria scartata).
- Costante di tempo della sfumatura T = 1,1 s x fattore.
- Alla dimensione di partenza: G = 4,96 s, T = 1,364 s.

## Generazione (ogni G secondi)

La prima generazione corrente inizia al primo fotogramma. A ogni fotogramma, prima di calcolare i colori, si controlla il tempo: se dall'inizio della generazione corrente sono passati almeno G secondi, si esegue una generazione e la generazione corrente inizia in quel fotogramma.

Una generazione fa, in quest'ordine:

1. Per ogni tessera conta le vicine vive (stato attuale, prima di cambiare qualunque tessera) e decide il nuovo stato con la regola B3/S23:
   - viva con 2 o 3 vicine vive: resta viva;
   - viva con un altro numero di vicine vive (0, 1, 4 o più): muore;
   - morta con esattamente 3 vicine vive: nasce;
   - morta con un altro numero: resta morta.
2. Età, per ogni tessera con nuovo stato vivo: se era morta l'età diventa 0, se era viva l'età sale di 1 fino a un massimo di 12. Per le tessere con nuovo stato morto l'età non cambia (una tessera appena morta conserva l'età che aveva).
3. Ogni tessera, cambiata o no, riceve un nuovo momento casuale da 0 a 9 (tutti i valori con la stessa probabilità).
4. Lo stato precedente di ogni tessera diventa lo stato attuale; lo stato attuale diventa il nuovo stato.
5. Controllo di ripetizione e di assenza di vita (vedi "Ripartenza automatica").

## Colore obiettivo di una tessera

A ogni fotogramma, per ogni tessera:

- La tessera è "partita" se dall'inizio della generazione corrente sono passati almeno momento x G / 10 (in microsecondi interi). Alla dimensione di partenza gli istanti sono distanziati di 0,496 s.
- Stato da mostrare: lo stato attuale se la tessera è partita, altrimenti lo stato precedente. Così ogni cambio compare in uno di 10 istanti diversi della generazione.
- Se lo stato da mostrare è vivo, il colore obiettivo dipende dall'età. Colori di riferimento: C0 = arancio (255, 140, 30), C1 = giallo (255, 230, 40), C2 = verde (60, 255, 70). Con a = età / 6, j = parte intera di a ma al massimo 1, f = a - j: colore = Cj + f x (C(j+1) - Cj), canale per canale. Età 0 arancio, età 6 giallo, età 12 verde.
- Se lo stato da mostrare è morto, il colore obiettivo è il colore delle tessere morte del suo tipo (vedi "Tessere").

## Sfumatura (a ogni fotogramma)

- dt = tempo trascorso dal fotogramma precedente (circa 0,038 s, il firmware fa circa 26 fotogrammi al secondo; al primo fotogramma dt = 0).
- k = 1 - e^(-dt / T).
- Per ogni canale: colore intermedio = colore intermedio + (colore obiettivo - colore intermedio) x k; poi colore mostrato = colore mostrato + (colore intermedio - colore mostrato) x k.
- Effetto: due filtri esponenziali in serie con costante di tempo T. Dopo un cambio del colore obiettivo il colore mostrato parte piano, accelera e rallenta alla fine; arriva al 50% in 1,68 T e al 90% in 3,89 T (alla dimensione di partenza 2,3 s e 5,3 s).

## Disegno

- A ogni pixel si assegna al più una tessera. Si esaminano le tessere nell'ordine di costruzione; per ognuna, ogni pixel il cui centro (in coordinate centrate) è dentro il poligono riceve quella tessera, sostituendo un'eventuale tessera precedente.
- "Dentro il poligono": regola del raggio orizzontale. Dal centro del pixel si traccia una semiretta verso +X e si contano i lati attraversati; un lato da (X1, Y1) a (X2, Y2) conta se uno dei due estremi ha Y maggiore del punto e l'altro no, e il punto di incontro ha X maggiore della X del pixel. Il pixel è dentro se il conteggio è dispari. Con questa regola un pixel sul lato comune tra due tessere va a una sola delle due.
- Bordo nero: con B = il più grande tra 0,55 e D / 40 se D è almeno 24 px, altrimenti 0, se B > 0 un pixel dentro resta senza tessera quando la sua distanza da almeno un lato del poligono (segmento) è minore di B. La linea nera tra due tessere è larga 2 B. Alla dimensione di partenza: linea di 1,4 px.
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

- BOOT: a ogni pressione (antirimbalzo 150 ms) si passa alla lunghezza successiva tra 12, 16, 20, 24, 28, 32, 40, 48, 64, 80 px (dopo 80 si torna a 12). La tassellatura viene ricostruita, tutte le tessere ripartono nere e si generano nuove tessere casuali. A ogni accensione si riparte da 28 px.
- PWR tenuto premuto per più di 1,5 s, dopo essere stato rilasciato almeno una volta dall'accensione: spegnimento. Con la batteria l'alimentazione si interrompe. Con l'USB la scheda resta alimentata: il firmware attende il rilascio di PWR e va in sospensione profonda; la pressione successiva di PWR riaccende (il firmware riparte dall'inizio).

## File

- `preview.png`: anteprima del display alla dimensione di partenza, dopo 9 generazioni da tessere casuali.
- `tiles.png`: la freccia con le misure e una tessera con le sue vicine, separate.
