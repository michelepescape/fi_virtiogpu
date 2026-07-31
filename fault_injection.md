# Modulo kernel e instrumentazione di llama.cpp

## Mappatura tramite `strace`

Prima di sviluppare il modulo, è stato fondamentale comprendere come l'inferenza di `llama.cpp` si traducesse in interazioni con la GPU virtualizzata. È stato utilizzato `strace` per tracciare le System Call di tipo `ioctl` dirette al driver grafico.

```bash
strace -f -tt -e trace=ioctl ~/llama.cpp/build/bin/llama-cli \
    -m qwen2.5-0.5b-instruct-q4_k_m.gguf -p "A ball is in a yellow box..."

```

L'analisi dell'output ha permesso di individuare specifiche chiamate del driver `drm/virtgpu` (es. `virtio_gpu_execbuffer_ioctl`, `virtio_gpu_resource_create_blob_ioctl`). Questo ha fornito i target precisi da agganciare nel kernel.

![image](api/attachments/Cljrpk7qoGtN/image/image.png)

## Kprobe vs Kretprobe

Per intercettare le funzioni a livello kernel, il framework offre due strumenti:

* **Kprobe:** Scatta *prima* che l'istruzione target venga eseguita.
* **Kretprobe:** Scatta nel momento in cui la funzione target *ritorna* al chiamante.

In questo caso utilizziamo la kretprobe, sfruttando il fatto che permette di eseguire:

- **Entry Handler (`entry_handler`):** Viene eseguito prima della funzione. 
    Viene usato per verificare se il processo/thread corrente è il nostro target tramite `target_pid` e,
    in caso di `descriptor_corruption`, per alterare gli argomenti passati alla funzione manipolando i registri (es. `RSI`, `RDX` su architettura x86_64).
- **Return Handler (`ret_handler`):** Viene eseguito al termine della funzione, prima di restituire il controllo a userspace. È qui che viene iniettata la
   latenza con `mdelay`, ed è qui che possiamo manipolare il valore di ritorno originale (`original_retval`) forzando un codice di errore simulando così un fallimento hardware o del driver.

---

## Analisi del Modulo Kernel `fault_injection.ko`

Il modulo è progettato per essere altamente configurabile a runtime tramite `sysfs`.

### Parametri di Selezione del Target

I seguenti parametri definiscono dove e chi colpire:

* `func_name`: Il nome della funzione del kernel da intercettare (es. `virtio_gpu_execbuffer_ioctl`).
Da inserire come argomento quando si carica il modulo.
* `target_comm`: Il nome del processo da colpire (es. `llama-cli`). Anche questo da inserire come argomento quando si carica il modulo
* `target_pid`: Il Thread ID (TID) esatto. Se impostato, ha la priorità su `target_comm`. È fondamentale per
le applicazioni multithreading per evitare di colpire thread di background al posto di quello usato per l'inferenza. Il TID viene derivato a runtime tramite l'instrumentazione
 di `server-context.cpp`.

### Parametri Temporali

Gestiscono *quando* e *per quanto tempo* iniettare il guasto:

* `target_occurrence`: Aspetta $N$ chiamate valide prima di attivare il guasto. [non usato]
* `burst_length`: Per quante chiamate consecutive mantenere attivo l'errore. [non usato]
* `trigger`: Variabile "interruttore". Quando viene scritta a `1`, resetta i contatori e arma il modulo per l'iniezione.

### Tipologie di Guasto Iniettabili

1. **Latenza (`inject_delay_ms`):** Introduce un blocco sincrono (`mdelay`) nel `ret_handler`.

```c
if (inject_delay_ms > 0) {
        pr_info("fault_hook: [LATENZA #%d] %d ms su %s\n", n, inject_delay_ms, func_name);
        mdelay(inject_delay_ms); 
    }
```

3. **Errore di Ritorno (`inject_error`):** Sovrascrive il valore restituito dalla funzione (es. per simulare un fail della ioctl).

```c
original_retval = regs_return_value(regs);
if (inject_error != 0) {
    pr_info("fault_hook: [FAULT #%d] Sovrascrittura %s: da %d a %d\n",
            n, func_name, original_retval, inject_error);
    regs_set_return_value(regs, inject_error);
}
```
5. **Corruzione Parametri della funzione (`descriptor_corruption`):** Eseguita nell'`entry_handler`, manipola le strutture dati prima che vengano elaborate dal kernel. Il modulo gestisce diversi casi a seconda della funzione:
   
* *Su virtio_gpu_execbuffer_ioctl:* Modifica la dimensione dei comandi o rimuove i flag di sincronizzazione (`IN_FENCE`) per simulare race conditions.
```c
 // Target1 : virtio_gpu_execbuffer_ioctl
switch (descriptor_corruption) {
    case 1:
    // Caso 1: Dimezza la dimensione del pacchetto comandi (allineato a 4 byte)
        exbuf->size = ALIGN(exbuf->size * 2, 4);
        break;
        
    case 2:
        // Caso 2: Azzera la dimensione dei comandi (sottomissione vuota)
        if (exbuf->size >= 8) {
            exbuf->size = 0;
        }
        break;

    case 3:
        // Caso 3: Rimuove la sincronizzazione In-Fence, test su race condition
        exbuf->flags &= ~VIRTGPU_EXECBUF_FENCE_FD_IN;
        break;

    default:
        // Test sintassi errata: forziamo un flag fuori maschera
        exbuf->flags |= 0x80000000;
        break;
}

pr_info("fault_hook: Execbuffer DOPO: size=%u, flags=0x%x\n",
        exbuf->size, exbuf->flags);

```


* *Su virtio_gpu_resource_create_blob_ioctl:* Altera i flag `MAPPABLE` o riduce la dimensione allocata del comando (`size`) o del buffer (`cmd_size`)  mantenendo l'allineamento di pagina.

```c
switch (descriptor_corruption) {
    case 1:
        // Caso 1: Riduciamo 'size' ma manteniamo l'allineamento a PAGE_SIZE (4096 byte)
        // In questo modo passa IS_ALIGNED() in verify_blob() ma alloca meno memoria
        if (blob->size > PAGE_SIZE) {
            blob->size = ALIGN_DOWN(blob->size / 2, PAGE_SIZE);
            if (blob->size == 0) blob->size = PAGE_SIZE; // Evitiamo size 0
        }
        break;

    case 2:
        // Caso 2: Riduciamo 'cmd_size' mantenendo l'allineamento a 4 byte (dword)
        // In questo modo passa (cmd_size % 4 == 0) in verify_blob()
        if (blob->cmd_size >= 8) {
            blob->cmd_size = ALIGN_DOWN(blob->cmd_size / 2, 4);
        }
        break;

    case 3:
        // Caso 3: Invertiamo MAPPABLE (0x0001) o CROSS_DEVICE (0x0004)
        // Rimane dentro VIRTGPU_BLOB_FLAG_USE_MASK, quindi supera verify_blob()
        blob->blob_flags ^= 0x0001; 
        break;

    default:
        // Test di errore sintattico: size non allineata per forzare EINVAL da verify_blob
        blob->size = 1; 
        break;
```

* *Su virtio_gpu_queue_fenced_ctrl_buffer (Data Poisoning):* Sostituisce bit del payload o parametri del comando con valori spazzatura (es. `0xFF`) o invalida gli Opcode dei comandi VirtIO.}

```c
switch (descriptor_corruption) {
    case 1:
        // Corruzione del Payload
        // Simula un bit-flip nella RAM o sul bus dati.
        if (vbuf->data_buf && vbuf->data_size > 0) {
            memset(vbuf->data_buf, 0xFF, min_t(size_t, 64, vbuf->data_size));
            pr_info("[FI] Case 1: Corrupted DMA Payload (data_buf)\n");
        } else if (vbuf->buf && vbuf->size > 8) {
            // Se non c'e' payload, corrompiamo i parametri del comando (lasciando intatto l'Opcode)
            memset((char *)vbuf->buf + 8, 0xAA, vbuf->size - 8);
            pr_info("[FI] Case 1: Corrupted Command Arguments (buf)\n");
        }
        break;

    case 2:
        // INVALID OPCODE 
        // Simula un errore nel controller VirtIO o un bug software.
        if (vbuf->buf && vbuf->size >= sizeof(u32)) {
            u32 *cmd_type = (u32 *)vbuf->buf;
            pr_info("[FI] Case 2: Replaced Opcode %u with INVALID (0xFFFF)\n", *cmd_type);
            *cmd_type = 0xFFFF;
        }
        break;

    case 3:
        // Errore di Dimensione/Bus
        // Simula una perdita di pacchetti o un difetto del DMA.
        if (vbuf->size > 4) {
            pr_info("[FI] Case 3: Truncated size from %d to 4 bytes\n", vbuf->size);
            vbuf->size = 4;
        }
        break;
}
```



## Instrumentazione `llama.cpp`

Per avere precisione  su quale token o fase dell'LLM far fallire, `llama.cpp` è stato instrumentato in `server-context.cpp` per comunicare con il modulo kernel.

### Configurazione tramite Variabili d'Ambiente

L'applicazione è stata modificata per leggere variabili di ambiente come `FI_TARGET_PHASE` (MODEL_LOAD, PREFILL, DECODE) e `FI_TARGET_TOKEN`. Questo permette di definire la campagna di test senza ricompilare il codice sorgente.

```cpp
 if (hook_initialized) return;
    hook_initialized = true;
    if (const char* p = std::getenv("FI_TARGET_PHASE")) target_phase = p;
    if (const char* t = std::getenv("FI_TARGET_TOKEN"))  target_token = std::atoi(t);
    if (const char* f = std::getenv("FI_FAULT_CODE"))    fault_code   = std::atoi(f);
    if (const char* d = std::getenv("FI_DELAY_MS"))     delay_ms     = std::atoi(d);
    if (const char* b = std::getenv("FI_BURST_LENGTH"))      burst_len    = std::atoi(b);
    if (const char* o = std::getenv("FI_TARGET_OCCURRENCE")) target_occ   = std::atoi(o);
    if (const char* c = std::getenv("FI_DESCRIPTOR_CORRUPTION")) descriptor_corruption = std::atoi(c);
```

### Isolamento del Thread

Prima di armare il modulo, il codice C++ cattura il TID del thread corrente tramite la syscall `SYS_gettid`:

```cpp
tid = static_cast<int>(syscall(SYS_gettid));

```

Questo valore viene scritto nel parametro `target_pid` del modulo. In questo modo, garantiamo che l'errore colpisca esclusivamente il thread di computazione e non i thread accessori di llama.

### Macchina a Stati per il Target "PREFILL" vs "DECODE"

(https://github.com/ggml-org/llama.cpp/pull/9283) 

```mermaid
graph TD;
    SLOT_STATE_IDLE-- new task -->SLOT_STATE_PROCESSING_PROMPT;
    SLOT_STATE_PROCESSING_PROMPT-- decode prompt -->SLOT_STATE_PROCESSING_PROMPT;
    SLOT_STATE_PROCESSING_PROMPT-- done processing prompt -->SLOT_STATE_DONE_PROMPT;
    SLOT_STATE_DONE_PROMPT-- is embedding -->SLOT_STATE_IDLE;
    SLOT_STATE_DONE_PROMPT-- is next-token prediction -->SLOT_STATE_GENERATING;
    SLOT_STATE_GENERATING-- decode next token -->SLOT_STATE_GENERATING;
    SLOT_STATE_GENERATING-- stop condition -->SLOT_STATE_IDLE;
```

Poiché `llama_decode()` viene invocata sia per il parsing del prompt iniziale sia per la generazione dei singoli token, la distinzione avviene ispezionando lo stato interno di `llama.cpp` (`slot.state`):

* Se lo slot è in `SLOT_STATE_PROCESSING_PROMPT`, ci troviamo nel Prefill.
* Se lo slot ha terminato il prompt ma `n_decoded == 0`, stiamo per calcolare il primo token. Consideriamo lo stato `SLOT_STATE_DONE_PROMPT` ancora come fase di Prefill.
* Se lo stato è `SLOT_STATE_GENERATING` siamo in fase di Decode, confrontiamo il contatore `n_decoded_before + 1` con il nostro `FI_TARGET_TOKEN` per colpire la generazione di una parola specifica.

Quando le condizioni coincidono, `trigger_fault()` scrive tutti i parametri (incluso il PID e i codici di errore) direttamente in `/sys/module/fault_injection/parameters/...` e infine scrive `1` su `trigger`, armando la Kretprobe istanti prima che parta l'effettiva inferenza verso il driver GPU.

```cpp
{
    bool is_prefill = false;
    int  n_decoded_before = -1;
    for (auto & slot : slots) {
        if (slot.is_processing()) {
            is_prefill = (slot.state == SLOT_STATE_PROCESSING_PROMPT ||
                          slot.state == SLOT_STATE_STARTED  ||
          (slot.state == SLOT_STATE_DONE_PROMPT && slot.n_decoded == 0));
            n_decoded_before = slot.n_decoded;
            break;// con --parallel 1 (cioè il default) ce n'e' solo uno di slot/job serviti contemporaneamnte
        }
    }
    if (n_decoded_before >= 0) {
        fi::hook_prefill_decode(is_prefill, n_decoded_before);
    }
}

```
---


## Guida all'Uso ed Esecuzione

### Caricamento del Modulo nel Kernel

Dopo aver compilato il modulo e averlo posizionato sulla vm, bisogna caricarlo specificando la funzione da agganciare.

```bash
sudo insmod fault_injection.ko func_name=virtio_gpu_execbuffer_ioctl target_comm=llama-cli

```

Per permettere a `llama.cpp`, che esegue in userspace senza privilegi di root, di armare il modulo in tempo reale, è necessario aprire i permessi dei parametri in `sysfs`:

```bash
sudo chmod 666 /sys/module/fault_injection/parameters/*
```

### Esecuzione 

Si può a questo punto avviare `llama-cli` passando le variabili d'ambiente per la fault injection. Nell'esempio seguente, si mira a fallire sul calcolo del **5° token** generato, iniettando l'errore `EIO` (-5) e un ritardo di 10ms.

```bash
FI_TARGET_PHASE=DECODE FI_TARGET_TOKEN=5 FI_FAULT_CODE=-5 FI_DELAY_MS=10 \
~/llama.cpp/build/bin/llama-cli \
    --no-display-prompt \
    -m ~/llama.cpp/models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
    -p "A ball is in a yellow box. Someone moves the ball to a blue box. Where is the ball now?" \
    -ngl 99 \
    -st \
    --simple-io \
    --color off \
    --seed 42 \
    --temp 0 \
```

