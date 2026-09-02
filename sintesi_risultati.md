# Sintesi dei Risultati

Questo documento raccoglie una sintesi delle metriche e delle combinazioni di parametri più rilevanti emerse dalla campagna di Fault Injection durante le 1396 run totali. 

---

## Distribuzione Globale degli Esiti 

Riepilogo complessivo della distribuzione degli esiti registrati sull'intera campagna di iniezioni:

| Esito | Conteggio | Percentuale |
| :--- | :---: | :---: |
| **Masked** | 641 | 45.9% |
| **SDC** | 0 | 0.0% |
| **App Error** | 99 | 7.1% |
| **App Crash** | 82 | 5.9% |
| **App Hang** | 220 | 15.8% |
| **Not Activated** | 354 | 25.4% |
| **Totale** | **1396** | **100.0%** |

![Esiti Complessivi Globali](plots/plot_overall_outcomes.png)

Dal grafico si nota come circa un quarto dei tentativi di iniezione (25.4%) non abbia prodotto effetti in quanto la funzione target non è stata eseguita durante la finestra di trigger (Not Activated). Nei casi in cui la funzione è stata effettivamente intercettata, in quasi la metà delle run complessive (45.9%) il sistema non ha manifestato comportamenti anomali, producendo l'output atteso; in questi casi il guasto è da considerarsi mascherato.

---

## Modello vs. Affidabilità Effettiva 

Confronto tra le tre taglie del modello Qwen 2.5 (0.5B, 1.5B, 3B), calcolato escludendo le esecuzioni in cui la kprobe non è stata attivata.

| Modello | Not Activated Rate | Masked (Assoluto) | Masked (Effettivo) | SDC (Effettivo) | Failure Rate (Effettivo) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **qwen2.5-0.5b** | 41.1% (185/450) | 30.4% | **51.7%** | **0.0%** | 48.3% |
| **qwen2.5-1.5b** | 19.5% (92/471)  | 49.7% | **61.7%** | **0.0%** | 38.3% |
| **qwen2.5-3b**   | 16.2% (77/475)  | 56.8% | **67.8%** | **0.0%** | 32.2% |

![Affidabilità Effettiva](plots/plot_effective_reliability.png)

- Al crescere della dimensione del modello si riconosce anche un aumento del tasso di mascheramento effettivo. Modelli con più parametri presentano una maggiore ridondanza computazionale e tendono ad assorbire meglio singoli disturbi transitori. Contemporaneamente, però, si osserva un aumento del tasso di mancate attivazioni in maniera inversamente proporzionale alla dimensione del modello. Probabilmente, la spiegazione risiede nel fatto che per i modelli più piccoli, l'elaborazione dei singoli token risulti più leggera (come si vedrà in seguito le mancate attivazioni si concentrano nella fase di DECODE), per cui solo con modelli più grandi si richiede l'utilizzo di funzioni di sottomissione di comandi al driver per ogni token, garantendo che la funzione target venga effettivamente eseguita mentre il trigger è attivo.

- In nessuna configurazione sono state rilevate Silent Data Corruption. Il sistema manifesta una risposta "binaria": o il guasto viene mascherato, oppure sfocia in un blocco/errore esplicito. La motivazione principale risiede nella natura dei punti di iniezione selezionati: le funzioni intercettate (`cmd_submit`, `execbuffer`, `queue_fenced`, `create_blob`) gestiscono il controllo del flusso, la sincronizzazione e l'allocazione delle risorse, e non il trasferimento diretto dei dati numerici (si è notato come funzioni come `transfer_to_host` non venivano chiamate durante l'inferenza, rendendole non adatte ad essere utilizzate come punto di injection).

---

## Funzione Target vs. Modalità di Fallimento

Distribuzione complessiva degli esiti per ciascun punto di injection del driver `virtio-gpu`.

| Funzione Kernel Target | Masked | SDC | App Error | App Crash | App Hang | Not Activated | Totale |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `virtio_gpu_cmd_submit` | 131 | 0 | 0 | 23 | **26** | 44 | 224 |
| `virtio_gpu_execbuffer_ioctl` | 213 | 0 | 0 | 0 | **100** | 79 | 392 |
| `virtio_gpu_queue_fenced_ctrl_buffer` | 223 | 0 | 5 | 12 | **94** | 58 | 392 |
| `virtio_gpu_resource_create_blob_ioctl` | 74 | 0 | **94** | **47** | **0** | 173 | 388 |

![Esiti per Funzione](plots/plot_function_outcomes.png)

Dai dati emerge una netta distinzione nel comportamento delle diverse funzioni kernel a fronte dei guasti:

- La funzione di allocazione della memoria (`virtio_gpu_resource_create_blob_ioctl`) è l'unica a generare esclusivamente fallimenti software espliciti, senza registrare alcun Hang. Corrompere parametri come la dimensione o i flag di allocazione provoca errori immediati non appena l'applicazione tenta di utilizzare i buffer. Inoltre, questa funzione è quella che registra il numero più alto di mancate attivazioni (173 su 388), poiché la memoria GPU viene allocata principalmente all'avvio durante il caricamento del modello e riutilizzata nei passaggi successivi, riducendo drasticamente le invocazioni al driver durante l'inferenza.

- Al contrario, le funzioni dedicate alla gestione e sottomissione dei comandi (`virtio_gpu_execbuffer_ioctl`, `virtio_gpu_queue_fenced_ctrl_buffer` e `virtio_gpu_cmd_submit`) concentrano la totalità dei 220 App Hang registrati. Durante l'inferenza, quando la CPU invia un comando alla GPU si mette in attesa di un segnale di completamento per poter proseguire. La corruzione dei descrittori o degli identificativi dei comandi impedisce all'acceleratore di elaborare la richiesta e di notificare la fine delle operazioni, lasciando il processo dell'LLM bloccato a tempo indeterminato in attesa di una risposta.  
  - I casi residui di App Crash registrati su queste funzioni sono dovuti principalmente a Segmentation Fault (quando un bit-flip nel Ring Buffer altera un puntatore a memoria) o all'intervento dei controlli interni del runtime Vulkan, che interrompono bruscamente il processo a fronte di strutture di comando non conformi.

---

## Dinamica Temporale: Fasi di Inferenza e Attivazione del Carico

Analisi incrociata tra le fasi del ciclo di vita dell'LLM (`MODEL_LOAD`, `PREFILL`, `DECODE`) e le funzioni bersaglio, suddivisa per modello.

![Fase e Funzione per Modello](plots/plot_phase_function_by_model.png)

L'analisi temporale evidenzia come l'impatto dei guasti dipenda strettamente dalla fase del ciclo di vita dell'LLM in cui avviene l'iniezione:

- Le fasi di inizializzazione (`MODEL_LOAD` e `PREFILL`) presentano la vulnerabilità più elevata e la quasi totalità dei fallimenti fatali. Durante questi passaggi viene infatti caricata la struttura dei pesi, allocata la memoria e costruito il contesto iniziale, rendendole evidentemente più vulnerabili a corruzione di comandi.

- Al contrario, la fase di generazione (`DECODE`) mostra un tasso di mascheramento molto alto. Trattandosi di un processo iterativo token per token, un disturbo transitorio di un comando, limitato al calcolo di una singola parola, viene frequentemente assorbito senza compromettere la stabilità complessiva dell'inferenza.

- Il confronto tra modelli durante la `DECODE` fa emergere un comportamento architetturale rilevante: nel modello da 0.5B molte iniezioni risultano non attivate (34 su 40 per `cmd_submit`) perché la computazione di un singolo token è talmente leggera da non richiedere sempre sottomissioni immediate al driver; nel modello da 3B, invece, la complessità di calcolo costringe il runtime a interagire costantemente con il driver a ogni token, portando le attivazioni al 100%. Contestualmente, l'allocazione memoria (`create_blob`) nel 3B risulta non attivata al 100% in decode, poiché la memoria viene interamente pre-allocata all'avvio e mai più richiesta durante la generazione.

---

## Tipologia di Guasto: Delay, Errori di Ritorno e Corruzioni Descrittori

Confronto tra le tre macro-categorie di guasto iniettate (`delay`, `error`, `corruption`) e le singole configurazioni sperimentali.

| Categoria di Guasto | Masked | SDC | App Error | App Crash | App Hang | Not Activated | Totale |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **delay** | **167** | 0 | 0 | 0 | 0 | 57 | 224 |
| **error** | **279** | 0 | **56** | **35** | **0** | 130 | 500 |
| **corruption** | **195** | 0 | **43** | **47** | **220** | 167 | 672 |

![Esiti per Categoria di Guasto](plots/plot_fault_type_outcomes.png)

![Esiti per Configurazione](plots/plot_fault_config_outcomes.png)

Il confronto tra le diverse modalità di iniezione evidenzia una netta separazione negli effetti prodotti:

- I ritardi temporali (`delay`), che simulano latenze del bus o della GPU di 1000ms, risultano mascherati nel 100% delle esecuzioni attivate (167 su 167). Questo dimostra che i timeout del runtime e dell'infrastruttura sono sufficientemente tolleranti da assorbire ritardi transitori senza provocare fallimenti.

- Gli errori sui codici di ritorno (`error`, con iniezione di codici errno `-5`, `-22` e `-28`) generano esclusivamente App Error (56) e App Crash (35) con 0 Hang. Quando una chiamata di sistema o una ioctl restituisce un codice di errore esplicito, l'applicazione intercetta il valore e termina immediatamente in modo controllato.

- Le corruzioni strutturali dei descrittori (`corruption`) sono le uniche responsabili di tutti i 220 App Hang registrati nella campagna. L'alterazione dei contenuti dei pacchetti o dei puntatori nel Ring Buffer inganna i controlli preliminari del driver ma impedisce la sincronizzazione con l'acceleratore, lasciando i thread in attesa indefinita.

---

## Efficacia del Fuzzing dei Descrittori

Comportamento delle funzioni sottoposte a mutazione strutturale dei parametri (Casi 1, 2, 3).

| Funzione Bersaglio e Caso | Obiettivo della Mutazione | Masked | Crash/Error | App Hang | Not Act. | Totale |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| `cmd_submit` (Case 1) | Bit-flip Ring Buffer (Dati Comandi) | **38** | 5 | 0 | 13 | 56 |
| `cmd_submit` (Case 2) | Invalidazione Res ID (`0xDEAD`) | 7 | 12 | **26** | 11 | 56 |
| `cmd_submit` (Case 3) | Azzeramento 16 byte Ring Buffer | **41** | 6 | 0 | 9 | 56 |
| `execbuffer` (Case 1) | Azzeramento Dimensione Pacchetto (`size=0`) | 12 | 0 | **32** | 12 | 56 |
| `execbuffer` (Case 2) | Indice Coda Invalido (`ring_idx=0xFFFFFFFF`) | 13 | 0 | **34** | 9 | 56 |
| `execbuffer` (Case 3) | Puntatore Comandi Invalido (`command=0xFFFFFFFF`) | 10 | 0 | **34** | 12 | 56 |
| `queue_fenced` (Case 1) | Bit-flip nel Payload DMA | 8 | 12 | **27** | 9 | 56 |
| `queue_fenced` (Case 2) | Opcode Invalido (`type=0xFFFF`) | 11 | 3 | **34** | 8 | 56 |
| `queue_fenced` (Case 3) | Troncamento Comando a solo Header | 12 | 2 | **33** | 9 | 56 |
| `create_blob` (Case 1) | Riduzione `size` (Allineata a PAGE_SIZE) | 12 | **19** | 0 | 25 | 56 |
| `create_blob` (Case 2) | Riduzione `cmd_size` (Allineata a dword) | **31** | 0 | 0 | 25 | 56 |
| `create_blob` (Case 3) | Inversione Flag `MAPPABLE` (`^0x0001`) | 0 | **31** | 0 | 25 | 56 |

![Esiti per Caso di Corruzione](plots/plot_corruption_outcomes.png)

L'analisi dei 12 casi di corruzione dei parametri dimostra come il sistema risponda in modo differente a seconda del livello di astrazione colpito:

- Quando la mutazione colpisce identificativi, opcode o indici di coda (come il cambio di `resource_handle` in `0xDEAD` su `cmd_submit`, il tipo comando in `0xFFFF` su `queue_fenced` o l'indice di ring in `0xFFFFFFFF` su `execbuffer`), il pacchetto supera i controlli sintattici del kernel ma blocca l'infrastruttura di virtualizzazione sottostante, generando sistematicamente App Hang per la mancata risposta dell'acceleratore.

- I singoli bit-flip nei dati o nel Ring Buffer (Caso 1 di `cmd_submit` e `queue_fenced`) mostrano invece un'alta tendenza al mascheramento: lievi alterazioni nei comandi o nei parametri dei compute shader vengono spesso tollerate dalla GPU senza interrompere l'esecuzione, a meno che il bit invertito non vada a corrompere direttamente un puntatore di memoria, provocando in quel caso un Segmentation Fault immediato.

- La manipolazione dei parametri di memoria (`create_blob`) produce fallimenti software mirati: ridurre la dimensione totale della memoria o invertire il flag `MAPPABLE` genera errori espliciti di allocazione o crash applicativi non appena `llama.cpp` tenta di accedere a buffer sottodimensionati o non mappabili, mentre ridurre la sola dimensione del comando (Caso 2) viene mascherato poiché la memoria allocata per i tensori rimane valida.

---

## Verifica dell'Ortogonalità rispetto al Task 

Verifica della distribuzione dei guasti al variare della tipologia di prompt somministrato (Aritmetica, Estrazione JSON, Ragionamento, Scelta Multipla).

| Tipologia di Task (Prompt) | Masked | App Error | App Crash | App Hang | Not Activated | Totale |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Aritmetica** | 134 (45.3%) | 14 (4.7%) | 20 (6.8%) | 52 (17.6%) | 76 (25.7%) | 296 |
| **Estrazione Dati (JSON)** | 174 (46.4%) | 34 (9.1%) | 20 (5.3%) | 54 (14.4%) | 93 (24.8%) | 375 |
| **Ragionamento** | 165 (44.0%) | 31 (8.3%) | 22 (5.9%) | 60 (16.0%) | 97 (25.9%) | 375 |
| **Scelta Multipla** | 168 (48.0%) | 20 (5.7%) | 20 (5.7%) | 54 (15.4%) | 88 (25.1%) | 350 |

![Esiti per Task](plots/plot_task_outcomes.png)

Il confronto tra i diversi prompt somministrati (Aritmetica, Estrazione JSON, Ragionamento e Scelta Multipla) mostra una distribuzione degli esiti sostanzialmente uniforme:

- Tutte le categorie registrano percentuali di mascheramento comprese tra il 44% e il 48%, tassi di blocco (App Hang) attorno al 15-17% e tassi di crash tra il 5% e il 6%.

- Questa costanza nei risultati conferma che l'affidabilità del sistema non dipende dalla semantica, dalla difficoltà logica o dalla lunghezza del testo generato, bensì dalle caratteristiche del driver e dell'infrastruttura di sincronizzazione sottostante.
