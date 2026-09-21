Progetto per l'esame di Real Time Systems and Industrial Applications A.A. 2025-2026

# Fault Injection su VirtioGPU

## Indice

- [1. Architettura del Sistema](#1-architettura-del-sistema)
- [2. Modello di Guasto e Metodologia di Injection](#2-modello-di-guasto-e-metodologia-di-injection)
  - [Tipologie di Guasto e Tassonomia degli Esiti](#tipologie-di-guasto-e-tassonomia-degli-esiti)
  - [Fasi di Injection](#fasi-di-injection)
  - [Architettura della Pipeline di Injection](#architettura-della-pipeline-di-injection)
- [3. Setup](#3-setup)
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

## 2. Modello di Guasto e Metodologia di Injection

Le funzioni bersaglio individuate nel driver `virtio-gpu` comprendono sia chiamate di interfaccia userspace-kernel (le ioctl `virtio_gpu_execbuffer_ioctl` per la sottomissione di comandi e `virtio_gpu_resource_create_blob_ioctl` per l'allocazione di memoria condivisa), sia routine interne del driver individuate tramite analisi del codice sorgente e validazione empirica con stampe di ricognizione a runtime (`virtio_gpu_queue_fenced_ctrl_buffer` per la gestione delle virtqueue con suppporto a fence di sincronizzazione, e `virtio_gpu_cmd_submit` per l'invio dei comandi).

### Tipologie di Guasto e Tassonomia degli Esiti

Per ciascun punto di iniezione vengono applicate tre classi distinte di fallimento:

1. **Override dei valori di ritorno:** Simulazione di fallimenti hardware o driver sovrascrivendo il valore di ritorno della funzione con codici errno standard DRM come indicato in [Recommended IOCTL Return Values](https://docs.kernel.org/gpu/drm-uapi.html#recommended-ioctl-return-values):
   - `-5 EIO`: Hardware I/O failure (GPU non responsiva o link interrotto).
   - `-22 EINVAL`: Invalid argument combination (parametri non conformi).
   - `-28 ENOSPC`: Out of memory / out of VRAM.  
   *(Questa tipologia non è applicabile a `virtio_gpu_cmd_submit`, essendo una funzione con tipo di ritorno `void`).*
2. **Latenze Temporali (Delays):** Iniezione di un ritardo sincrono di 1000ms nel `ret_handler` per simulare bus stall, saturazione dei canali o jitter hardware.
3. **Corruzione dei Parametri e dei Descrittori:** Mutazioni mirate nell'`entry_handler` sui parametri o sulle strutture dati scambiate (es. azzeramento o troncamento dimensioni, indici di coda non validi, opcode sconosciuti, inversione di flag di memoria `MAPPABLE`, puntatori nulli e bit-flip nel payload DMA).

Per ogni run di iniezione, il comportamento del sistema viene classificato secondo la seguente tassonomia degli esiti:

| Esito | Descrizione |
| :--- | :--- |
| **Masked (Success)** | L'esecuzione continua regolarmente senza anomalie e l'output generato corrisponde esattamente alla baseline. |
| **SDC (Silent Data Corruption)** | L'inferenza termina con successo, ma il testo generato differisce dalla baseline. |
| **App Error** | L'applicazione rileva un'anomalia, restituisce un codice di errore o logga un fallimento, terminando in modo controllato. |
| **App Crash** | L'applicazione termina bruscamente con segnale anomalo (es. SIGSEGV, SIGBUS). |
| **App Hang** | L'applicazione entra in stallo indefinito. |
| **Not Activated** | La funzione target non è stata invocata durante la finestra di attivazione del trigger, lasciando l'esecuzione intatta. |

### Fasi di Injection

L'iniezione viene sincronizzata con le fasi caratteristiche del ciclo di vita dell'inferenza in `llama.cpp`:

* **`MODEL_LOAD`**: Fase di avvio in cui i pesi del modello vengono caricati e mappati nei buffer GPU.
* **`PREFILL`**: Elaborazione iniziale del prompt utente e generazione del primo token.
* **`DECODE`**: Generazione sequenziale e autoregressiva dei singoli token successivi. Tramite il parametro `FI_TARGET_TOKEN`, è possibile selezionare l'istante di calcolo di uno specifico token $N$ (es. token 2 o 5).

### Architettura della Pipeline di Injection

L'infrastruttura sfrutta operazioni eseguite in spazio utente e spazio kernel:
* `fault_injection.ko`: Modulo basato su Kretprobe (`entry_handler` per le corruzioni prima dell'elaborazione e `ret_handler` per ritardi ed errori di ritorno).
*  Tramite la syscall `SYS_gettid` aggiunta nel codice instrumentato, `llama.cpp` acquisisce il proprio Thread ID (TID) e lo comunica al modulo nel parametro `target_pid`, garantendo che vengano colpite esclusivamente le operazioni del thread di inferenza e non i thread accessori o di background del sistema operativo.
* In userspace si configurano anche i parametri desiderati per l'iniezione tramite variabili d'ambiente `FI_*` indicate prima di eseguire `llama-cli`. L'applicazione instrumentata arma il modulo scrivendo `1` nel file `trigger` su `sysfs` (`/sys/module/fault_injection/parameters/*`) prima dell'invio della richiesta al driver.

Per l'analisi del codice C del modulo kernel, e la trattazione analitica dei 12 casi di corruzione dei descrittori, si rimanda a **[fault_injection.md](fault_injection.md)**.

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

Per ogni modello vengono testate diverse tipologie di prompt per verificare che, anche al variare dell'input fornito, non vi siano differenze di
comportamento. Sono stati utilizzati i seguenti prompt:

- Ragionamento: "A ball is in a yellow box. Someone moves the ball to a blue box. Where is the ball now?"
- Aritmetica: "What is 1542 + 2341? Provide only the number."
- Conoscenza, domanda a risposta multipla: "Question: Which planet is known as the Red Planet? A) Earth B) Mars C) Jupiter. Answer:"
- Estrazione Dati: "Extract the names of the cities from this text as a JSON list: 'I visited Paris, then took a train to Berlin, and ended up in Rome.' JSON:"

Per ogni combinazione di modello, prompt e funzione target, vengono eseguite run per ciascuna delle tre categorie di guasto (errori di ritorno, delay e corruzioni) attraverso le fasi `MODEL_LOAD`, `PREFILL` e `DECODE`.

Ogni run consiste nell'esecuzione di un singolo turno one-shot di inferenza. Si fornisce un prompt
e se ne riceve l'output prima che llama-cli termini automaticamente. Contestualmente al comando è possibile fornire
anche i parametri desiderati del fault tramite variabili d'ambiente come mostrato di seguito:

```sh
FI_TARGET_PHASE=DECODE \
FI_TARGET_TOKEN=5 \
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

Nella fase di DECODE, tramite `FI_TARGET_TOKEN` è possibile scegliere durante la generazione di quale token iniettare il guasto.
Da notare che la fase di PREFILL termina con la generazione del primo token, per cui il guasto in decode viene iniettato a partire dal secondo token. [Diagramma degli stati](fault_injection.md#macchina-a-stati-per-il-target-prefill-vs-decode).

Per ogni run gli output creati hanno la seguente struttura su disco:

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

La cartella `campaign_results` raccoglie l'output grezzo delle iniezioni (1400 esecuzioni valide).
L'elaborazione dei dati è demandata a due script di supporto:

- `analyze_results.py`: processa i metadati (`meta.json`) e i log di sistema per classificare l'esito delle iniezioni (Masked, SDC, App Error, App Crash, App Hang).
- `plot_results.py` / `plot_results_percentage.py`: generano i grafici relativi alla distribuzione dei guasti (salvati in `plots/`).

I risultati aggregati sono documentati nel file dedicato:

- [sintesi_risultati.md](sintesi_risultati.md)
