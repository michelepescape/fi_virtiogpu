# Fault Injection su VirtioGPU

## Indice

- [1. Architettura del Sistema](#1-architettura-del-sistema)
- [2. Fault Model](#2-fault-model)
- [3. Setup e Prerequisiti](#3-setup)
- [4. Instrumentazione llama.cpp](#4-instrumentazione-llamacpp)
- [5. Esecuzione della Campagna](#5-esecuzione-della-campagna)
- [6. Analisi dei Risultati](#6-analisi-dei-risultati)

---

## 1. Architettura del Sistema

In questo progetto configureremo un ambiente per eseguire
fault injection durante l'inferenza di SLM/LLM all'interno di un guest QEMU/KVM, sfruttando
la paravirtualizzazione dell'acceleratore grafico.

Nello specifico, l'inferenza avverrà tramite il framework `llama.cpp` configurato
con il backend Vulkan/Mesa.
Verrà usata la famiglia di modelli Qwen 2.5 in diverse dimensioni (0.5B, 1.5B, 3B),
per valutare se, al variare del numero di parametri, ci siano
variazioni nella robustezza del processo di inferenza.

La fault injection sarà effettuata su chiamate ioctl e funzioni
della libreria virtio-gpu ([drivers/gpu/drm/virtio/virtgpu_drv.h](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/virtio))
utilizzando il meccanismo delle kprobe.

<img width="600" height="800" alt="diagramma" src="https://github.com/user-attachments/assets/9e2ea790-3d82-4e9e-b499-4856d0edd2fb" />

## 2. Fault Model

Per ognuno dei punti di iniezione si iniettano i seguenti fallimenti:

- valori di ritorno errati/errori
- delay
- modifiche a valori di strutture dati

Tuttavia, l'iniezione di un valore di ritorno errato non viene applicata alle funzioni di libreria virtio-gpu che prevedono un tipo di ritorno `void`i.

Per ognuno dei fault ci si aspetta uno dei seguenti esiti:

- **Masked (Success)**: l'esecuzione continua senza errori apparenti e produce l'output corretto.
- **SDC (Silent Data Corruption)**: l'esecuzione continua, ma l'output prodotto non è coerente con la baseline.
- **App Error**: l'applicazione restituisce un codice d'errore e termina in maniera pulita.
- **App Crash**: l'applicazione termina bruscamente.
- **App Hang**: l'applicazione si blocca senza concludere la sua esecuzione e diventa non responsiva.
- **Not Activated**: il fault non è stato iniettato con successo, ad esempio se la funzione bersaglio non è stata invocata.

## 3. Setup

Il guest esegue come VM tramite QEMU/KVM. Al suo interno è stato installato llama.cpp compilandolo dal sorgente con il
supporto al backend Vulkan.

Per eseguire la fault injection tramite le kprobe è stato preparato un modulo da caricare nel kernel del guest a runtime (`fault_injection.c`). La funzione target va indicata al momento del caricamento e per modificarla
è necessario rimuovere e ricaricare il modulo.

Si rimanda a [setup.md](setup.md) per i dettagli della preparazione dell'ambiente, a [fault_injection.md](fault_injection.md) per
i dettagli del modulo kernel.

## 4. Instrumentazione llama.cpp

Al fine di sincronizzare l'iniezione dei fault con le fasi dell'inferenza, è stato instrumentato il codice sorgente di
llama.cpp. In particolare, dalle funzioni presenti nel file `server-context.cpp`, che vengono utilizzate anche nel caso in cui
si utilizzi `llama-cli`, si ha accesso alle informazioni che ci servono per attivare le kprobe in maniera controllata.

Il file `server-context.cpp` in questo repo contiene il codice già instrumentato, mentre il file `instrumentation.cpp` contiene
solamente il codice aggiuntivo.

Maggiori dettagli in [Instrumentazione llama.cpp](fault_injection.md#instrumentazione-llamacpp)

## 5. Esecuzione della Campagna

Lo script `run_campaign.py` permette di eseguire automaticamente la vm, eseguire le run necessarie a raccogliere i dati di baseline
ed eseguire la campagna effettiva di fallimenti.

Per ogni modello vengono testati diverse tipologie di prompt per verificare che, anche al variare dell'input fornito, non vi siano differenze di
comportamento. Sono stati utilizzati i seguenti prompt:

- Ragionamento: "A ball is in a yellow box. Someone moves the ball to a blue box. Where is the ball now?"
- Aritmetica: "What is 1542 + 2341? Provide only the number."
- Conoscenza, domanda a risposta multipla: "Question: Which planet is known as the Red Planet? A) Earth B) Mars C) Jupiter. Answer:"
- Estrazione Dati: "Extract the names of the cities from this text as a JSON list: 'I visited Paris, then took a train to Berlin, and ended up in Rome.' JSON:"

Per ogni prompt viene iniettato un fault in ognuna delle tre fasi `MODEL_LOAD`, `PREFILL` o `DECODE`.
Per quanto riguarda i valori di ritorno errati, sono stati selezionati alcuni valori tra quelli attesi in caso di errore in una ioctl (https://docs.kernel.org/gpu/drm-uapi.html#recommended-ioctl-return-values). (Per la funzione `virtio_gpu_cmd_submit`, essendo di tipo `void`, questo guasto non viene iniettato). I valori scelti sono:

- \-5 EIO ("The GPU died and couldn’t be resurrected through a reset. Modesetting hardware failures are signalled through the “link status” connector property.")
- \-22 EINVAL ("Catch-all for anything that is an invalid argument combination which cannot work.")
- \-28 ENOSPC ("Some drivers use this to differentiate “out of kernel memory” from “out of VRAM”")

Per quanto riguarda il delay, viene iniettato un delay di 1s prima di ritornare dalla funzione target.

Per quanto riguarda le modifiche alle strutture dati utilizzate come argomenti delle funzioni target, lo scopo sarebbe
tentare di corrompere l'esecuzione con comandi spuri, dati errati o, in generale, modifiche non previste.

A tale scopo, per ogni funzione target sono state individuate possibili "corruzioni" a valle di un'analisi dei parametri
utilizzati dalla specifica funzione, pertanto questa tipologia di fault è diversa per ognuna di loro. [Dettagli in fault_injection.md](fault_injection.md#tipologie-di-guasto-iniettabili)

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
essere iniettato dal secondo in poi. [Diagramma degli stati](fault_injection.md#macchina-a-stati-per-il-target-prefill-vs-decode)

Con i parametri `FI_FAULT_CODE`, `FI_DELAY_MS`, `FI_DESCRIPTOR_CORRUPTION` si può gestire l'iniezione
dei fault secondo quanto detto in precedenza.

Per ogni run gli output creati hanno la seguente struttura:

```sh
campaign_results/
└── {model_name}/                         # Es. qwen2.5-0.5b
    └── {prompt_name}/                    # Es. ragionamento, aritmetica
        ├── baseline/                     # Esecuzione di riferimento 
        │   ├── meta.json                 # Metadati (num. token, stato)
        │   ├── stdout.txt                # Risposta "pulita" dell'LLM
        │   ├── stderr.txt                # Log di llama.cpp
        │   ├── dmesg.txt                 # Log di sistema (kprobe)
        │   ├── guest_kernel.log          # (Eventuale log kernel guest)
        │   └── qemu_host.log             # (Eventuale log QEMU host)
        │
        └── {target_func}/                # Funzione Kernel Bersaglio (es. virtio_gpu_cmd_submit)
            └── {fault_type}_{val}/       # Configurazione Guasto (es. delay_1000, corruption_1)
                └── {PHASE}/              # Fase di inferenza (MODEL_LOAD, PREFILL, DECODE)
                    └── token_{n}/        # Identificativo token (1 per LOAD/PREFILL)
                        ├── meta.json     # Metadati iniezione e match risultato
                        ├── stdout.txt    # Output testuale generato 
                        ├── stderr.txt    # Timing e log del server
                        ├── dmesg.txt     # Verifica attivazione kprobe e dmesg
                        ├── guest_kernel.log 
                        └── qemu_host.log 
```

## 6. Analisi dei Risultati

La cartella `campaign_results` raccoglie l'output grezzo delle iniezioni (1396 esecuzioni valide).
L'elaborazione dei dati è demandata a due script di supporto:

- `analyze_results.py`: processa i metadati (`meta.json`) e i log di sistema per classificare l'esito delle iniezioni (Masked, SDC, App Error, App Crash, App Hang).
- `plot_results.py`: genera i grafici relativi alla distribuzione dei guasti (salvati in `plots/`).

I risultati aggregati sono documentati nel file dedicato:

- [sintesi_risultati.md](sintesi_risultati.md)
