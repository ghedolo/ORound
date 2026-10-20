# life1_conway: regole

Descrizione completa del comportamento del firmware, sufficiente per rifarlo.

## Convenzioni

- Display rotondo 466 x 466 px, 1 px = 0,072 mm. Coordinate dei pixel: x da 0 a 465 verso destra, y da 0 a 465 verso il basso. Il centro del pixel (x, y) è il punto (x + 0,5; y + 0,5).
- Colori RGB a 8 bit per canale (0..255).
- Tempi in secondi; il firmware misura il tempo in microsecondi dall'accensione.
- "Numero casuale" = numero uniforme in [0, 1] dal generatore hardware, senza seme fisso: ogni accensione è diversa.

## Celle

- Griglia di 23 x 23 celle quadrate di 20 px (1,44 mm). La cella (i, j), con i colonna e j riga da 0 a 22, occupa i pixel da x = 3 + 20 i a 3 + 20 i + 19 e da y = 3 + 20 j a 3 + 20 j + 19 (3 px di margine su ogni lato dell'immagine).
- Una cella è "dentro" se tutti i suoi 4 angoli, cioè i punti (3 + 20 i; 3 + 20 j), (3 + 20 (i + 1); 3 + 20 j), (3 + 20 i; 3 + 20 (j + 1)), (3 + 20 (i + 1); 3 + 20 (j + 1)), distano al massimo 232 px dal punto (233; 233). Le celle dentro sono 385.
- Le celle non dentro sono sempre morte e sempre nere.
- Vicine di una cella: le 8 celle della griglia che la circondano (lati e angoli); vicine fuori dalla griglia non esistono. Le celle non dentro contano come vicine morte.

## Stato di ogni cella

- viva (sì/no) adesso;
- viva (sì/no) prima dell'ultima generazione, chiamato "stato precedente";
- età: numero intero da 0 a 12;
- momento: numero intero da 0 a 9;
- due colori interni, "colore intermedio" e "colore mostrato", ciascuno con tre valori reali.

All'accensione: tutte le celle morte, stato precedente morto, età 0, colori interni neri (0, 0, 0). Subito dopo si generano celle casuali (vedi "Nuove celle casuali").

## Generazione (ogni 3 s)

La prima generazione corrente inizia al primo fotogramma. A ogni fotogramma, prima di calcolare i colori, si controlla il tempo: se dall'inizio della generazione corrente sono passati almeno 3 s, si esegue una generazione e la generazione corrente inizia in quel fotogramma.

Una generazione fa, in quest'ordine:

1. Per ogni cella dentro conta le vicine vive (stato attuale, prima di cambiare qualunque cella) e decide il nuovo stato con la regola B3/S23:
   - viva con 2 o 3 vicine vive: resta viva;
   - viva con 0, 1 o da 4 a 8 vicine vive: muore;
   - morta con esattamente 3 vicine vive: nasce;
   - morta con un altro numero: resta morta.
   Le celle non dentro hanno sempre nuovo stato morto.
2. Età, per ogni cella con nuovo stato vivo: se era morta l'età diventa 0, se era viva l'età sale di 1 fino a un massimo di 12. Per le celle con nuovo stato morto l'età non cambia (una cella appena morta conserva l'età che aveva).
3. Ogni cella, cambiata o no, riceve un nuovo momento casuale da 0 a 9 (tutti i valori con la stessa probabilità).
4. Lo stato precedente di ogni cella diventa lo stato attuale; lo stato attuale diventa il nuovo stato.
5. Controllo di ripetizione e di assenza di vita (vedi "Ripartenza automatica").

## Colore obiettivo di una cella

A ogni fotogramma, per ogni cella dentro:

- La cella è "partita" se dall'inizio della generazione corrente sono passati almeno momento x 0,3 s (0; 0,3; 0,6; ... 2,7 s).
- Stato da mostrare: lo stato attuale se la cella è partita, altrimenti lo stato precedente. Così ogni cambio compare in uno di 10 istanti diversi della generazione.
- Se lo stato da mostrare è vivo, il colore obiettivo dipende dall'età e: con t = età / 12, colore = arancio (255, 190, 70) + t x (blu (50, 140, 255) - arancio), canale per canale. Età 0 è arancio, età 12 è blu.
- Se lo stato da mostrare è morto, il colore obiettivo è (14, 14, 24).

## Sfumatura (a ogni fotogramma)

- dt = tempo trascorso dal fotogramma precedente (circa 0,038 s, il firmware fa circa 26 fotogrammi al secondo; al primo fotogramma dt = 0).
- k = 1 - e^(-dt / 0,8).
- Per ogni canale: colore intermedio = colore intermedio + (colore obiettivo - colore intermedio) x k; poi colore mostrato = colore mostrato + (colore intermedio - colore mostrato) x k.
- Il colore mostrato viene arrotondato all'intero più vicino.
- Effetto: due filtri esponenziali in serie, costante di tempo 0,8 s ciascuno. Dopo un cambio del colore obiettivo il colore mostrato parte piano, accelera e rallenta alla fine; arriva al 50% in circa 1,3 s e al 90% in circa 3,1 s.

## Disegno

- Ogni cella dentro si disegna come un quadrato di 16 px con angoli arrotondati (raggio 6 px) e 2 px di nero per lato: nelle coordinate interne della cella (da 0 a 20) la forma è l'insieme dei punti con x e y tra 2 e 18 e distanza al massimo 6 px dal quadrato interno da 8 a 12 (cioè il rettangolo da 2 a 18 con gli angoli arrotondati).
- Bordo sfumato: in ogni pixel della cella si prendono 4 x 4 punti campione (a 1/8, 3/8, 5/8, 7/8 del pixel in x e in y) e si conta quanti cadono nella forma, n da 0 a 16. Il pixel vale colore mostrato x n / 16, con la divisione intera (parte frazionaria scartata).
- Tutti gli altri pixel sono neri.

## Ripartenza automatica

Alla fine di ogni generazione:

- Si calcola un'impronta dello stato nuovo: si parte da 2166136261 e, per ogni cella della griglia in ordine (riga j da 0 a 22, colonna i da 0 a 22), si fa XOR con 1 se la cella è viva o con 0 se morta e si moltiplica per 16777619, tenendo solo 32 bit (impronta FNV-1a).
- Se l'impronta è uguale a una delle ultime tre, il contatore di ripetizione sale di 1, altrimenti torna a 0. Così si riconoscono stati fissi e cicli di 2 o 3 generazioni. Due stati diversi con la stessa impronta sono possibili ma molto rari.
- Il contatore di assenza sale di 1 se non c'è nessuna cella viva, altrimenti torna a 0.
- Se il contatore di ripetizione supera 20 (cioè arriva a 21) o il contatore di assenza supera 3 (cioè arriva a 4), si generano nuove celle casuali.

## Nuove celle casuali

Per ogni cella:

- lo stato precedente diventa lo stato attuale;
- nuovo momento casuale da 0 a 9;
- la cella diventa viva se è dentro e un numero casuale è minore di 0,30, altrimenti morta;
- se è diventata viva ed era morta, l'età diventa 0; se era già viva, l'età resta.

Poi il tempo della generazione riparte dal fotogramma precedente e i due contatori tornano a 0 (le ultime tre impronte restano). Le celle che cambiano sfumano come in una generazione normale, ognuna nel suo momento.

## Tocco

- Al primo contatto e a ogni posizione letta mentre il dito scorre (circa 26 volte al secondo), nel punto (x, y) in pixel: colonna i = (x - 3) / 20 e riga j = (y - 3) / 20, divisione intera con troncamento verso zero.
- Per la cella (i, j) e per le 8 celle intorno, se esistono nella griglia e sono dentro: la cella (i, j) diventa viva sempre, ognuna delle altre con probabilità 0,5 (un numero casuale minore di 0,5).
- Una cella che diventa viva così: se era morta l'età diventa 0; stato attuale e stato precedente diventano vivi, quindi il colore obiettivo cambia subito senza attendere il momento; il colore mostrato sfuma comunque come sopra.
- Il contatore di ripetizione torna a 0.
- Il tocco non vale mentre è attiva la regolazione della luminosità.

## Luminosità

- Se il dito resta entro 15 px dal punto di primo contatto per più di 0,8 s, parte la regolazione della luminosità (il tocco ha già acceso le celle sotto il dito al primo contatto).
- Durante la regolazione: posizione = posizione all'inizio + (y all'inizio - y attuale) / 300, limitata tra 0,15 e 1. Dito in alto = più luminoso. Valore inviato al display = 255 x posizione^2, arrotondato.
- Quando il dito si stacca la regolazione finisce e la posizione viene salvata in memoria permanente; all'accensione si riprende la posizione salvata (minimo 0,15), altrimenti 1.

## Pulsanti

- BOOT: a ogni pressione (antirimbalzo 150 ms) nuove celle casuali. La dimensione delle celle non cambia.
- PWR tenuto premuto per più di 1,5 s, dopo essere stato rilasciato almeno una volta dall'accensione: spegnimento. Con la batteria l'alimentazione si interrompe. Con l'USB la scheda resta alimentata: il firmware attende il rilascio di PWR e va in sospensione profonda; la pressione successiva di PWR riaccende (il firmware riparte dall'inizio).

## File

- `preview.png`: anteprima del display dopo 9 generazioni da celle casuali.
- `tiles.png`: forma della cella e celle vicine.
