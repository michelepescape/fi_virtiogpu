#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <uapi/drm/virtgpu_drm.h> // per la definizione esatta delle struct per la corruzione delle ioctl

#include <linux/byteorder/generic.h>
#include <uapi/linux/virtio_gpu.h>

// Per la definizione della struct necessaria a queue_fenced_ctrl, convertendo
// virtio_gpu_resp_cb e struct virtio_gpu_object_array * in void * per evitare
// di dover importare altri header non essendo parametri necessari.
struct virtio_gpu_vbuffer {
  char *buf;
  int size;

  void *data_buf;
  uint32_t data_size;

  char *resp_buf;
  int resp_size;
  void *resp_cb;
  void *resp_cb_data;

  void *objs;
  struct list_head list;

  uint32_t seqno;
};

static char *target_comm = "llama-cli";
module_param(target_comm, charp, 0644);
MODULE_PARM_DESC(
    target_comm,
    "Nome del processo da colpire (vuoto = tutti)"); // se si imposta il
                                                     // target_pid è ridondante

static int target_pid = -1;
module_param(target_pid, int, 0644);
MODULE_PARM_DESC(target_pid,
                 "TID (SYS_gettid lato userspace) del thread esatto da "
                 "colpire. -1 = usa target_comm come fallback");

static inline bool is_target_pid(void) {
  if (target_pid != -1) {
    return current->pid == target_pid;
  }

  if (!target_comm || target_comm[0] == '\0')
    return true;
  return strncmp(current->comm, target_comm, TASK_COMM_LEN) == 0;
}

static char *func_name = "virtio_gpu_execbuffer_ioctl";
module_param(func_name, charp, 0644);
MODULE_PARM_DESC(func_name, "Nome della funzione da agganciare");

// I permessi dei parametri vanno ridefiniti tramite chmod dopo il caricamento
// del modulo per permettere la loro modifica da utente non sudo quando si
// lancia llama-cli

/* --- PARAMETRI DEL GUASTO --- */
static int inject_error = 0;
module_param(inject_error, int, 0644);
MODULE_PARM_DESC(inject_error,
                 "Codice errore da iniettare (es. -5). 0 = nessun errore.");

static int inject_delay_ms = 0;
module_param(inject_delay_ms, int, 0644);
MODULE_PARM_DESC(inject_delay_ms, "Latenza (mdelay) in ms.");

static int descriptor_corruption = 0;
module_param(descriptor_corruption, int, 0644);
MODULE_PARM_DESC(
    descriptor_corruption,
    "0=Disabilitato, 1=Size a 0, 2=Truncate Size, 3=Corrompi Flags/Puntatore");

/* --- PARAMETRI TEMPORALI --- */
static int target_occurrence = 1;
module_param(target_occurrence, int, 0644);
MODULE_PARM_DESC(target_occurrence, "Aspetta N chiamate prima di colpire");

static int burst_length = 1;
module_param(burst_length, int, 0644);
MODULE_PARM_DESC(burst_length,
                 "Per quante chiamate consecutive mantenere il guasto attivo");

/* --- STATO INTERNO --- */
static atomic_t call_counter = ATOMIC_INIT(0);
static atomic_t armed = ATOMIC_INIT(0);

struct probe_data {
  int call_num;
};

/* --- TRIGGER ATTIVAZIONE --- */
static int set_trigger(const char *val, const struct kernel_param *kp) {
  int tmp;
  int ret = kstrtoint(val, 0, &tmp);
  if (ret) {
    // Se kstrtoint non ritorna zero, non ha avuto successo
    return ret;
  }

  if (tmp != 0) {
    atomic_set(&call_counter, 0); // Azzera il conteggio
    atomic_set(&armed, 1);        // ARMA
    pr_info("fault_hook: TRIGGER ATTIVO! Attendo %d chiamate, colpiro' per %d "
            "volte consecutive. (err=%d, delay=%dms)\n",
            target_occurrence, burst_length, inject_error, inject_delay_ms);
  } else {
    atomic_set(&armed, 0); // DISARMA
    pr_info("fault_hook: TRIGGER DISARMATO esplicitamente.\n");
  }
  return 0;
}

static const struct kernel_param_ops trigger_ops = {.set = set_trigger};
static int trigger_placeholder;
module_param_cb(trigger, &trigger_ops, &trigger_placeholder, 0200);
MODULE_PARM_DESC(
    trigger, "Scrivi 1 per armare l'errore, 0 per disarmarlo forzatamente.");

/* --- HANDLER KPROBE --- */
static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
  struct probe_data *data = (struct probe_data *)ri->data;

  if (!atomic_read(&armed))
    return 1; // ogni volta che ritorno != 0 non eseguirò il ret_handler
  if (!is_target_pid())
    return 1;

  int n = atomic_inc_return(&call_counter);
  data->call_num = n;

  // Se abbiamo superato la lunghezza del burst (+ target occorrenza), disarma
  // ed esci
  if (n >= burst_length + target_occurrence) {
    atomic_set(&armed, 0);
    return 1;
  }

  // Se non siamo ancora arrivati all'occorrenza desiderata, salta l'iniezione
  if (n < target_occurrence) {
    return 1;
  }

  // --- ZONA DI RICOGNIZIONE (RECON) ---
  if (strcmp(func_name, "virtio_gpu_cmd_map") == 0) {
    /*
     * Mappatura registri ABI x86_64:
     * 1 (rdi) : struct virtio_gpu_device *vgdev
     * 2 (rsi) : struct virtio_gpu_object_array *objs
     * 3 (rdx) : uint64_t offset
     */
    void *objs = (void *)regs->si;
    uint64_t offset = (uint64_t)regs->dx;

    pr_info_ratelimited(
        "fault_hook: [RECON] virtio_gpu_cmd_map | objs_ptr: %p, offset: %llu\n",
        objs, (unsigned long long)offset);
  } else if (strcmp(func_name, "virtio_gpu_cmd_submit") == 0) {
    void *submit_data = (void *)regs->si;
    uint32_t submit_size = (uint32_t)regs->dx;
    uint32_t submit_ctx = (uint32_t)regs->cx;

    pr_info("fault_hook: [RECON] cmd_submit | ctx_id=%u, data_size=%u byte\n",
            submit_ctx, submit_size);

    if (submit_data && submit_size > 0) {
      print_hex_dump(KERN_INFO, "fault_hook [RAW SUBMIT]: ", DUMP_PREFIX_OFFSET,
                     16, 1, submit_data, min_t(size_t, 32, submit_size), true);
    }
  }

  // ------------------------------------

  if (descriptor_corruption > 0) {
    void *data_ptr = (void *)regs->si; // nell'architettura x86_64: il secondo
                                       // argomento è nel registro RSI

    // Target1 : virtio_gpu_execbuffer_ioctl
    if (strcmp(func_name, "virtio_gpu_execbuffer_ioctl") == 0) {
      struct drm_virtgpu_execbuffer *exbuf =
          (struct drm_virtgpu_execbuffer *)data_ptr;

      switch (descriptor_corruption) {
      case 1:
        // Caso 1: Azzera la dimensione dei comandi (sottomissione vuota)
        if (exbuf->size >= 8) {
          exbuf->size = 0;
        }
        break;
      case 2:
        // Forza un ring_idx fuori scala per far fallire la selezione della coda
        pr_info("fault_hook: [FI] Case 2: Corrotto ring_idx (0x%x) con "
                "0xFFFFFFFF\n",
                exbuf->ring_idx);
        exbuf->ring_idx = 0xFFFFFFFF;
        break;

      case 3:
        // Corrompe l'indirizzo del buffer comandi
        pr_info("fault_hook: [FI] Case 2: Puntatore command alterato da 0x%llx "
                "a 0xFFFFFFFF\n",
                exbuf->command);
        exbuf->command = 0xFFFFFFFF;

        break;
      }
      pr_info("fault_hook: Execbuffer DOPO: size=%u, flags=0x%x\n", exbuf->size,
              exbuf->flags);

    }
    // Target 2: virtio_gpu_resource_create_blob_ioctl
    else if (strcmp(func_name, "virtio_gpu_resource_create_blob_ioctl") == 0) {
      struct drm_virtgpu_resource_create_blob *blob =
          (struct drm_virtgpu_resource_create_blob *)data_ptr;

      pr_info("fault_hook: CreateBlob prima: size=%llu\n", blob->size);

      switch (descriptor_corruption) {
      case 1:
        // Caso 1: Riduciamo 'size' (dimensione comando) ma manteniamo
        // l'allineamento a PAGE_SIZE (4096 byte) In questo modo passa
        // IS_ALIGNED() in verify_blob() ma alloca meno memoria
        if (blob->size > PAGE_SIZE) {
          blob->size = ALIGN_DOWN(blob->size / 2, PAGE_SIZE);
          if (blob->size == 0)
            blob->size = PAGE_SIZE; // Evitiamo size 0
        }
        break;

      case 2:
        // Caso 2: Riduciamo 'cmd_size' (dimensione buffer comandi) mantenendo
        // l'allineamento a 4 byte (dword) In questo modo passa (cmd_size % 4 ==
        // 0) in verify_blob()
        if (blob->cmd_size >= 8) {
          blob->cmd_size = ALIGN_DOWN(blob->cmd_size / 2, 4);
        }
        break;

      case 3:
        // Caso 3: Invertiamo MAPPABLE (0x0001) o CROSS_DEVICE (0x0004)
        // Rimane dentro VIRTGPU_BLOB_FLAG_USE_MASK, quindi supera verify_blob()
        blob->blob_flags ^= 0x0001;
        break;
      }

      pr_info("fault_hook: CreateBlob DOPO: size=%llu\n", blob->size);
    } else if (strcmp(func_name, "virtio_gpu_queue_fenced_ctrl_buffer") == 0) {
      struct virtio_gpu_vbuffer *vbuf = (struct virtio_gpu_vbuffer *)data_ptr;

      pr_info("fault_hook: FencedCtrl PRIMA: size=%d, data_size=%u\n",
              vbuf->size, vbuf->data_size);

      // Ispezione Byte per Byte dei primi 16 byte del comando
      if (vbuf->buf && vbuf->size >= sizeof(struct virtio_gpu_ctrl_hdr)) {
        struct virtio_gpu_ctrl_hdr *hdr =
            (struct virtio_gpu_ctrl_hdr *)vbuf->buf;

        pr_info("fault_hook: [RECON HDR] type=0x%x, flags=0x%x, fence_id=%llu, "
                "ctx_id=%u\n",
                le32_to_cpu(hdr->type), le32_to_cpu(hdr->flags),
                le64_to_cpu(hdr->fence_id), le32_to_cpu(hdr->ctx_id));

        print_hex_dump(KERN_INFO,
                       "fault_hook [RAW CMD_BUF]: ", DUMP_PREFIX_OFFSET, 16, 1,
                       vbuf->buf, min_t(size_t, 16, vbuf->size), true);
      }

      // Ispezione Payload Dati (DMA) se presente
      if (vbuf->data_buf && vbuf->data_size > 0) {
        pr_info("fault_hook: [RECON DATA] Payload DMA presente! Dimensione: %u "
                "byte\n",
                vbuf->data_size);
        print_hex_dump(
            KERN_INFO, "fault_hook [RAW DATA_BUF]: ", DUMP_PREFIX_OFFSET, 16, 1,
            vbuf->data_buf, min_t(size_t, 16, vbuf->data_size), true);
      } else {
        pr_info("fault_hook: [RECON DATA] Nessun payload DMA separato "
                "(data_buf nullo o data_size=0)\n");
      }

      switch (descriptor_corruption) {
      case 1:
        // Bit-flip nel payload DMA
        // Simula un singolo bit-flip nella RAM o sul bus PCIe.
        // Mutazione: esattamente 1 bit invertito al centro del data_buf.
        if (vbuf->data_buf && vbuf->data_size > 0) {
          uint32_t flip_off = vbuf->data_size / 2;
          unsigned char original = ((unsigned char *)vbuf->data_buf)[flip_off];
          ((unsigned char *)vbuf->data_buf)[flip_off] ^= 0x01;
          pr_info(
              "[FI] Case 1 (SDC): Bit-flip in data_buf[%u]: 0x%02x -> 0x%02x\n",
              flip_off, original, ((unsigned char *)vbuf->data_buf)[flip_off]);
        }
        break;

      case 2:
        // INVALID OPCODE — Corruzione del tipo di comando
        // Simula un errore o corruzione dell'header sul
        // bus. Esattamente 1 campo (hdr->type) sovrascritto con
        // valore invalido.
        if (vbuf->buf && vbuf->size >= sizeof(struct virtio_gpu_ctrl_hdr)) {
          struct virtio_gpu_ctrl_hdr *hdr =
              (struct virtio_gpu_ctrl_hdr *)vbuf->buf;
          u32 old_type = le32_to_cpu(hdr->type);
          hdr->type = cpu_to_le32(0xFFFF);
          pr_info("[FI] Case 2 (OPCODE): type 0x%x -> 0xFFFF\n", old_type);
        }
        break;

      case 3:
        // SIZE TRUNCATION — Troncamento del comando all'header
        // Simula un DMA underrun o perdita dati sul bus.
        // Mutazione: esattamente 1 campo (vbuf->size) ridotto al solo header.
        if (vbuf->size > (int)sizeof(struct virtio_gpu_ctrl_hdr)) {
          int old_size = vbuf->size;
          vbuf->size = sizeof(struct virtio_gpu_ctrl_hdr);
          pr_info("[FI] Case 3 (TRUNC): size %d -> %zu (header-only)\n",
                  old_size, sizeof(struct virtio_gpu_ctrl_hdr));
        }
        break;
      }
    }
    // Target 4: virtio_gpu_cmd_submit
    else if (strcmp(func_name, "virtio_gpu_cmd_submit") == 0) {
      void *data = (void *)regs->si;
      uint32_t data_size = (uint32_t)regs->dx;

      // DEBUG: Vediamo le dimensioni di tutti i pacchetti
      pr_info(
          "fault_hook: [DEBUG] cmd_submit intercettata! data_size=%u byte\n",
          data_size);

      /*
       * Struttura del comando (24 byte, scoperta tramite RECON):
       *   [0-3]   le32 resource_handle  (es. 0xBE = 190)
       *   [4-7]   le32 padding/flags    (sempre 0)
       *   [8-15]  le64 base_address     (costante per sessione)
       *   [16-23] le64 data_offset      (incrementa ~126KB per dispatch)
       *
       * Registri ABI x86_64:
       *   rsi = void *data (puntatore al buffer sopra)
       *   edx = uint32_t data_size (sempre 24)
       *   ecx = uint32_t ctx_id
       */
      if (data && data_size >= 24) {
        unsigned char *payload = (unsigned char *)data;

        switch (descriptor_corruption) {
        case 1: {
          // OBIETTIVO: Bit-flip nel buffer circolare (Ring Buffer) condiviso.
          // L'applicazione usa questo buffer per accodare i comandi per la GPU.
          // Il payload di 24 byte intercettato contiene l'indirizzo base del buffer
          // (base_addr) e l'indice di scrittura attuale (tail_off).
          // Tentiamo di invertire 1 bit negli ultimi byte appena accodati,
          // con la speranza di alterare un parametro senza rompere del tutto l'esecuzione,
          // per osservare un potenziale SDC (spesso però viene mascherato o porta a crash).
          uint64_t base_addr = *(uint64_t *)(payload + 8);
          uint64_t tail_off = *(uint64_t *)(payload + 16);

          if (base_addr && tail_off >= 16) {
            void __user *target_user_addr =
                (void __user *)(base_addr + tail_off - 8);
            unsigned char byte_val;
            if (copy_from_user_nofault(&byte_val, target_user_addr, 1) == 0) {
              unsigned char corrupted = byte_val ^ 0x01;
              if (copy_to_user_nofault(target_user_addr, &corrupted, 1) == 0) {
                pr_info("[FI] Case 1 (SDC USER RING): Bit-flip at [0x%llx]: "
                        "0x%02x -> 0x%02x\n",
                        (unsigned long long)(base_addr + tail_off - 8),
                        byte_val, corrupted);
              }
            }
          }
          break;
        }

        case 2: {
          // OBIETTIVO: Invalidazione dell'identificativo del Ring Buffer (Hang).
          // Sovrascriviamo l'identificativo della risorsa (byte 0-3) con 0xDEAD.
          // In questo modo, quando il pacchetto arriva all'hypervisor, quest'ultimo
          // non trova il ring associato e ignora la sottomissione.
          // Questo spesso impedisce il completamento dell'operazione bloccando il programma (Hang).
          uint32_t *res_handle = (uint32_t *)payload;
          uint32_t old_handle = le32_to_cpu(*res_handle);
          *res_handle = cpu_to_le32(0xDEAD);
          pr_info(
              "[FI] Case 2 (RES_ID INVALID): resource_handle 0x%x -> 0xDEAD\n",
              old_handle);
          break;
        }

        case 3: {
          // OBIETTIVO: Azzeramento parziale dei comandi nel Ring Buffer.
          // Come nel Case 1, sfruttiamo base_addr e tail_off per accedere
          // agli ultimi 16 byte accodati nel ring e li forziamo a zero.
          // L'idea è testare la resilienza della GPU quando riceve un comando
          // valido ma con parametri azzerati.
          uint64_t base_addr = *(uint64_t *)(payload + 8);
          uint64_t tail_off = *(uint64_t *)(payload + 16);

          if (base_addr && tail_off >= 16) {
            void __user *target_user_addr =
                (void __user *)(base_addr + tail_off - 16);
            unsigned char zeros[16] = {0};
            if (copy_to_user_nofault(target_user_addr, zeros, sizeof(zeros)) ==
                0) {
              pr_info("[FI] Case 3 (SDC ZERO PARAMS): Cleared 16 bytes in ring "
                      "at 0x%llx\n",
                      (unsigned long long)(base_addr + tail_off - 16));
            }
          }
          break;
        }
        }
      }
    }
  }

  // Il ritardo lo facciamo solo quando colpiamo il bersaglio
  // Ma l'entry_handler non sa ancora a che chiamata siamo con precisione per
  // via della concorrenza, quindi spostiamo tutto il carico utile (ritardo
  // compreso) nel ret_handler per averlo coordinato
  return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
  int original_retval;
  struct probe_data *data = (struct probe_data *)ri->data;
  int n = data->call_num;

  //    if (!atomic_read(&armed)) return 0; //se arrivo nel ret_handler,
  //    l'entry ha già controllato quindi non serve ricontrollare
  //   if (!is_target_pid()) return 0;

  // Se non siamo ancora arrivati all'occorrenza target, passa oltre
  //  if (n < target_occurrence) return 0;

  /*   // Se abbiamo superato l'occorrenza target + burst_length, disarma e
     passa oltre if (n >= target_occurrence + burst_length) {
     atomic_set(&armed, 0); pr_info("fault_hook: Burst terminato. Disarmo
     automatico.\n"); return 0;
     }*/

  // --- ZONA DI SABOTAGGIO ---

  // Inietta il ritardo se presente
  if (inject_delay_ms > 0) {
    pr_info("fault_hook: [LATENZA #%d] %d ms su %s\n", n, inject_delay_ms,
            func_name);
    mdelay(inject_delay_ms);
  }

  // Inietta l'errore se presente
  original_retval = regs_return_value(regs);
  if (inject_error != 0) {
    pr_info("fault_hook: [FAULT #%d] Sovrascrittura %s: da %d a %d\n", n,
            func_name, original_retval, inject_error);
    regs_set_return_value(regs, inject_error);
  }

  return 0;
}

static struct kretprobe my_kretprobe = {
    .handler = ret_handler,
    .entry_handler = entry_handler,
    .maxactive = 20,
    .data_size = sizeof(struct probe_data),
};

static int __init fault_hook_init(void) {
  int ret;
  my_kretprobe.kp.symbol_name = func_name;
  ret = register_kretprobe(&my_kretprobe);
  if (ret < 0) {
    pr_err("fault_hook: Registrazione fallita su %s, codice: %d\n", func_name,
           ret);
    return ret;
  }
  pr_info("fault_hook: Hook inserito su %s. Pronto per il trigger.\n",
          func_name);

  return 0;
}

static void __exit fault_hook_exit(void) {
  unregister_kretprobe(&my_kretprobe);
  pr_info("fault_hook: Hook rimosso.\n");
}

module_init(fault_hook_init);
module_exit(fault_hook_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fault Injection LLM");
