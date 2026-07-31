# Fault Injection su VirtioGPU durante inferenza


## Indice
- [1. Architettura del Sistema](#1-architettura-del-sistema)
- [2. Modello di Guasto (Fault Model)](#2-modello-di-guasto-fault-model)
- [3. Setup e Prerequisiti](#3-setup-e-prerequisiti)
- [4. Instrumentazione llama.cpp](#4-instrumentazione-llama)
- [5. Esecuzione delle Campagne](#4-esecuzione-delle-campagne)
- [6. Analisi dei Risultati](#5-analisi-dei-risultati)

---

## 1. Architettura del Sistema

In questo progetto configureremo un ambiente per eseguire
fault injection durante l'inferenza di SLM/LLM all'interno di un guest QEMU/KVM, sfruttando
la paravirtualizzazione dell'acceleratore grafico.

Nello specifico, l'inferenza avverrà tramite il framework llama.cpp configurato 
con il backend Vulkan/Mesa. 
Verrà usata la famiglia di modelli Qwen 2.5 in diverse dimensioni (0.5B, 1.5B, 3B, 7B), 
per valutare se, al variare del numero di parametri, ci siano 
variazioni nella robustezza del processo di inferenza.

La fault injection sarà effettuata su chiamate ioctl e funzioni 
della libreria virtio-gpu ([drivers/gpu/drm/virtio/virtgpu_drv.h](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/virtio))
utilizzando il meccanismo delle kprobe.



## 2. Modello di Guasto (Fault Model)

Al momento, per ognuno dei punti di iniezione si iniettano i seguenti fallimenti:

- valori di ritorno errati/errori
- delay
- modifiche a valori di strutture dati

Sto finendo di verificare l'applicabilità, dato che le funzioni di libreria virtio-gpu spesso non 
prevedono un valore di ritorno.

Per ognuno dei fault ci si aspetta il seguente modello di guasti:

- Success/Masked, se l'esecuzione continua senza errori
    - se l'esecuzione continua senza errori, ma con output non coerente con quello individuato come baseline,
    si parlerebbe di Silent Data Corruption. Al momento non sono ancora riuscito a causare un fault del genere.
- Hang, il programma si blocca senza concludere la sua esecuzione e diventa non responsivo.
- Crash, il programma termina bruscamente.




## 3. Setup

Il guest esegue come VM tramite QEMU/KVM. Al suo interno è stato installato llama.cpp compilandolo dal sorgente con il 
supporto al backend Vulkan.

Per eseguire la fault injection tramite le kprobe è stato preparato un modulo da caricare nel kernel del guest a runtime (`fault_injection.c`). La funzione target va indicata al momento del caricamento e per modificarla 
è necessario rimuovere e ricaricarei il modulo.

Si rimanda a [setup.md](setup.md) per i dettagli della preparazione dell'ambiente, a [fault_injection.md](fault_injection.md) per
i dettagli del modulo kernel.


## 4. Instrumentazione llama.cpp

Al fine di sincronizzare l'iniezione dei fault con le fasi dell'inferenza, è stato instrumentato il codice sorgente di
llama.cpp. In particolare, dalle funzioni presenti nel file `server-context.cpp`, che vengono utilizzate anche nel caso in cui
si utilizzi `llama-cli`, si ha accesso alle informazioni che ci servono per attivare le kprobe in maniera controllata.

Il file `server-context.cpp` in questo repo contiene il codice già instrumentato, mentre il file `instrumentation.cpp` contiene 
solamente il codice aggiuntivo. 

Maggiori dettagli in (Instrumentazione llama.cpp)[instrumentation.md]

## 5. Esecuzione della Campagna

Lo script `run_campaign.py` permette di eseguire automaticamente la vm, eseguire le run necessarie a raccogliere i dati di baseline
ed eseguire la campagna effettiva di fallimenti.

Per ogni modello vengono testati diverse tipologie di prompt per verificare che, anche al variare dell'input fornito, non vi siano differenze di 
comportamento. Sono stati utilizzati i seguenti prompt:

- Ragionamento: "A ball is in a yellow box. Someone moves the ball to a blue box. Where is the ball now?"
- Aritmetica: "What is 1542 + 2341? Provide only the number."
- Conoscenza, domanda a risposta multipla: "Question: Which planet is known as the Red Planet? A) Earth B) Mars C) Jupiter. Answer:"

Per ogni prompt viene iniettato un fault in ognuna delle tre fasi `MODEL_LOAD`, `PREFILL` o `DECODE`. 
Per quanto riguarda i valori di ritorno errati, sono stati selezionati alcuni valori tra quelli attesi in caso di errore 
in una ioctl (https://docs.kernel.org/gpu/drm-uapi.html#recommended-ioctl-return-values), cioè:

- -5 EIO ("The GPU died and couldn’t be resurrected through a reset. Modesetting hardware failures are signalled through the “link status” connector property.")
- -22 EINVAL ("Catch-all for anything that is an invalid argument combination which cannot work.")
- -28 ENOSPC ("Some drivers use this to differentiate “out of kernel memory” from “out of VRAM”")

Per quanto riguarda il delay, viene iniettato un delay di 1s prima di ritornare dalla funzione target.

Per quanto riguarda le modifiche alle strutture dati utilizzate come argomenti delle funzioni target, lo scopo sarebbe 
tentare di corrompere l'esecuzione con comandi spuri, dati errati o, in generale, modifiche non previste.

A tale scopo, per ogni funzione target sono state individuati 3 possibili "corruzioni" a valle di un'analisi dei parametri
utilizzati dalla specifica funzioni, pertanto questa tipologia di fault è diversa per ognuna di loro. [corruption.md]

Al momento, per ogni modello vengono eseguite delle run per ognuno dei fault individuati.

Ogni run consiste nell'esecuzione di un singolo turno one-shot di inferenza. Si fornisce un prompt
e se ne riceve l'output prima che llama-cli termini automaticamente. Contestualmente al comando è possibile fornire
anche i parametri desiderati del fault come si vede di seguito:

```sh
FI_TARGET_PHASE=DECODE \
FI_TARGET_TOKEN=5 \
FI_TARGET_OCCURRENCE=1 \
FI_BURST_LENGTH=1 \
FI_FAULT_CODE=0 \
FI_DELAY_MS=0 \
FI_DESCRIPTOR_CORRUPTION=1 \
strace -f -tt -e trace=ioctl \
~/llama.cpp/build/bin/llama-cli \
--no-display-prompt     \
-c 2048     \ 
# -b e -ub necessari per evitare errori di allocazione in fase di caricamento del modello
-b 128     \ 
-ub 128     \  
-m ~/llama.cpp/models/qwen2.5-3b-instruct-q4_k_m.gguf     \
-p "Question: Which planet is known as the Red Planet? 
A) Earth B) Mars C) Jupiter. Answer:"     \
-ngl 99 \
# single-turn, evita la modalità conversazione e termina dopo aver eseguito 
-st \ 
--simple-io \
--color off \
--seed 1234 \
# greedy decoding
--temp 0 
```

Possiamo, quindi, scegliere in quale fase iniettare il fault. Nella fase di DECODE, utilizzando il parametro
`FI_TARGET_TOKEN`, è possibile scegliere durante la generazione di quale token iniettare il guasto.
Da notare che la fase di PREFILL termina con la generazione del primo token, per cui il guasto può
essere iniettato dal secondo in poi. [diagramma-degli-stati]

Con i parametri `FI_FAULT_CODE`, `FI_DELAY_MS`, `FI_DESCRIPTOR_CORRUPTION` si può gestire l'iniezione 
dei fault secondo quanto detto in precedenza.


Per ogni run gli output creati hanno la seguente struttura:

```sh
campaign_results/
└── {model_name}/                         # Es. qwen2.5-0.5b
    └── {prompt_name}/                    # Es. ragionamento, aritmetica
        │
        ├── baseline/                     # Esecuzione di riferimento 
        │   ├── meta.json                 # Metadati (num. token, stato)
        │   ├── stdout.txt                # Risposta "pulita" dell'LLM
        │   ├── stderr.txt                # Log di llama.cpp
        │   └── dmesg.txt                 # Log di sistema 
        │
        ├── {target_func} /  # Funzione Kernel Bersaglio
            ├── {fault} /               # Configurazione Guasto (Tipo_Valore)
                ├── {PHASE} /           # Fase
                    └── {token_n} /          # Token di iniezione (1 per LOAD/PREFILL)
                        ├── meta.json     
                        ├── stdout.txt    
                        ├── stderr.txt    
                        └── dmesg.txt     
```



## 6. Analisi dei Risultati

La cartella `campaign_results` contiene un esempio dei risultati di una campagna parziale. È ancora da implementare uno script che aggreghi i risultati per analisi e visualizzazione.


## TODO

- Aggiungere gli altri modelli.
- Provare altri tipi di corruzione per le funzioni utilizzate, specialmente se si riesce a causare SDC
- Verificare iniezione per altre funzioni
- Implementare parser/script per grafici
