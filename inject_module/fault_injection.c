#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/delay.h>
#include <linux/ptrace.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

#include <uapi/drm/virtgpu_drm.h>  // per la definizione esatta delle struct per la corruzione delle ioctl

// Per la definizione della struct necessaria a queue_fenced_ctrl, convertendo  virtio_gpu_resp_cb e struct virtio_gpu_object_array * in void * per evitare di dover importare altri header non essendo parametri necessari.
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
MODULE_PARM_DESC(target_comm, "Nome del processo da colpire (vuoto = tutti)"); // se si imposta il target_pid è ridondante

static int target_pid = -1;
module_param(target_pid, int, 0644);
MODULE_PARM_DESC(target_pid, "TID (SYS_gettid lato userspace) del thread esatto da colpire. -1 = usa target_comm come fallback");


static inline bool is_target_pid(void)
{
    if (target_pid != -1) {
        return current->pid == target_pid;
    }

    if (!target_comm || target_comm[0] == '\0') return true;
    return strncmp(current->comm, target_comm, TASK_COMM_LEN) == 0;
}

static char *func_name = "virtio_gpu_execbuffer_ioctl"; 
module_param(func_name, charp, 0644);
MODULE_PARM_DESC(func_name, "Nome della funzione da agganciare");


// I permessi dei parametri vanno ridefiniti tramite chmod dopo il caricamento del modulo per permettere la loro modifica da utente non sudo quando si lancia llama-cli

/* --- PARAMETRI DEL GUASTO --- */
static int inject_error = 0;
module_param(inject_error, int, 0644);
MODULE_PARM_DESC(inject_error, "Codice errore da iniettare (es. -5). 0 = nessun errore.");

static int inject_delay_ms = 0;
module_param(inject_delay_ms, int, 0644);
MODULE_PARM_DESC(inject_delay_ms, "Latenza (mdelay) in ms.");

static int descriptor_corruption = 0; 
module_param(descriptor_corruption, int, 0644);
MODULE_PARM_DESC(descriptor_corruption, "0=Disabilitato, 1=Size a 0, 2=Truncate Size, 3=Corrompi Flags/Puntatore");

/* --- PARAMETRI TEMPORALI --- */
static int target_occurrence = 1;
module_param(target_occurrence, int, 0644);
MODULE_PARM_DESC(target_occurrence, "Aspetta N chiamate prima di colpire");

static int burst_length = 1;
module_param(burst_length, int, 0644);
MODULE_PARM_DESC(burst_length, "Per quante chiamate consecutive mantenere il guasto attivo");

/* --- STATO INTERNO --- */
static atomic_t call_counter = ATOMIC_INIT(0);
static atomic_t armed        = ATOMIC_INIT(0);

struct probe_data {
    int call_num;
};

/* --- TRIGGER ATTIVAZIONE --- */
static int set_trigger(const char *val, const struct kernel_param *kp) 
{
    int tmp;
    int ret = kstrtoint(val, 0, &tmp);
    if (ret) {
    // Se kstrtoint non ritorna zero, non ha avuto successo
      return ret;
    }
    
    if (tmp != 0) {
        atomic_set(&call_counter, 0); // Azzera il conteggio
        atomic_set(&armed, 1);        // ARMA
        pr_info("fault_hook: TRIGGER ATTIVO! Attendo %d chiamate, colpiro' per %d volte consecutive. (err=%d, delay=%dms)\n",
                target_occurrence, burst_length, inject_error, inject_delay_ms);
    } else {
        atomic_set(&armed, 0);        // DISARMA
        pr_info("fault_hook: TRIGGER DISARMATO esplicitamente.\n");
    }
    return 0;
}

static const struct kernel_param_ops trigger_ops = { .set = set_trigger };
static int trigger_placeholder;
module_param_cb(trigger, &trigger_ops, &trigger_placeholder, 0200);
MODULE_PARM_DESC(trigger, "Scrivi 1 per armare l'errore, 0 per disarmarlo forzatamente.");


/* --- HANDLER KPROBE --- */
static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct probe_data *data = (struct probe_data *)ri->data; 
  
    if (!atomic_read(&armed)) return 1; //ogni volta che ritorno != 0 non eseguirò il ret_handler
    if (!is_target_pid()) return 1;
    
    int n = atomic_inc_return(&call_counter);
    data->call_num = n;

    // Se abbiamo superato la lunghezza del burst (+ target occorrenza), disarma ed esci
    if (n >= burst_length + target_occurrence) {
        atomic_set(&armed, 0);
        return 1;
    }
    
    // Se non siamo ancora arrivati all'occorrenza desiderata, salta l'iniezione
    if (n < target_occurrence) {
        return 1; 
    }
    
    // --- ZONA DI RICOGNIZIONE (RECON) ---
    if (strcmp(func_name, "virtio_gpu_fence_ack") == 0) {
        // Leggiamo i registri secondo la calling convention AMD64 (x86_64)
        uint64_t offset = regs->dx; // 3° argomento
        uint32_t stride = regs->r8; // 5° argomento
        
        pr_info("fault_hook: [RECON] transfer_to_host_3d | offset=%llu, stride=%u\n", 
                (unsigned long long)offset, stride);
        
        // Se in futuro vorrai corromperli, lo farai qui. Es:
        if (descriptor_corruption == 4) { // Nuovo caso ad-hoc
            regs->dx = offset + 1024; // Spostiamo l'offset!
            pr_info("fault_hook: [FI] Corrotto offset da %llu a %llu\n", 
                    (unsigned long long)offset, (unsigned long long)regs->dx);
        }
    }
    else if (strcmp(func_name, "virtio_gpu_ctrl_ack") == 0) {
        // Questa prende solo (struct work_struct *work) in RDI. 
        // Per ora ci basta sapere se e quante volte viene chiamata.
        pr_info("fault_hook: [RECON] dequeue_ctrl_func | Bottom Half attivato (Risposta pronta!)\n");
    }
    // ------------------------------------
    
    if (descriptor_corruption > 0){
        void *data_ptr = (void *)regs->si; // nell'architettura x86_64: il secondo argomento è nel registro RSI
        
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
        }
        // Target 2: virtio_gpu_resource_create_blob_ioctl
        else if (strcmp(func_name, "virtio_gpu_resource_create_blob_ioctl") == 0) {
            struct drm_virtgpu_resource_create_blob *blob = (struct drm_virtgpu_resource_create_blob *)data_ptr;

            pr_info("fault_hook: CreateBlob prima: size=%llu\n", blob->size);
            
            switch (descriptor_corruption) {
                case 1:
                    // Caso 1: Riduciamo 'size' (dimensione comando) ma manteniamo l'allineamento a PAGE_SIZE (4096 byte)
                    // In questo modo passa IS_ALIGNED() in verify_blob() ma alloca meno memoria
                    if (blob->size > PAGE_SIZE) {
                        blob->size = ALIGN_DOWN(blob->size / 2, PAGE_SIZE);
                        if (blob->size == 0) blob->size = PAGE_SIZE; // Evitiamo size 0
                    }
                    break;

                case 2:
                    // Caso 2: Riduciamo 'cmd_size' (dimensione buffer comandi) mantenendo l'allineamento a 4 byte (dword)
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
            }

            pr_info("fault_hook: CreateBlob DOPO: size=%llu\n",  blob->size);
        }
        else if (strcmp(func_name, "virtio_gpu_queue_fenced_ctrl_buffer") == 0){
            struct virtio_gpu_vbuffer *vbuf = (struct virtio_gpu_vbuffer *) data_ptr;
            
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
        }
           
    }
        
    
    // Il ritardo lo facciamo solo quando colpiamo il bersaglio
    // Ma l'entry_handler non sa ancora a che chiamata siamo con precisione per via della concorrenza,
    // quindi spostiamo tutto il carico utile (ritardo compreso) nel ret_handler per averlo coordinato
    return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    int original_retval;
    struct probe_data *data = (struct probe_data *)ri->data;
    int n = data->call_num;

//    if (!atomic_read(&armed)) return 0; //se arrivo nel ret_handler, l'entry ha già controllato quindi non serve ricontrollare
 //   if (!is_target_pid()) return 0;

    // Se non siamo ancora arrivati all'occorrenza target, passa oltre
  //  if (n < target_occurrence) return 0;

 /*   // Se abbiamo superato l'occorrenza target + burst_length, disarma e passa oltre
    if (n >= target_occurrence + burst_length) {
        atomic_set(&armed, 0);
        pr_info("fault_hook: Burst terminato. Disarmo automatico.\n");
        return 0;
    }*/

    // --- ZONA DI SABOTAGGIO ---
    
    // Inietta il ritardo se presente
    if (inject_delay_ms > 0) {
        pr_info("fault_hook: [LATENZA #%d] %d ms su %s\n", n, inject_delay_ms, func_name);
        mdelay(inject_delay_ms); 
    }

    // Inietta l'errore se presente
    original_retval = regs_return_value(regs);
    if (inject_error != 0) {
        pr_info("fault_hook: [FAULT #%d] Sovrascrittura %s: da %d a %d\n",
                n, func_name, original_retval, inject_error);
        regs_set_return_value(regs, inject_error);
    }

    return 0;
}

static struct kretprobe my_kretprobe = {
    .handler       = ret_handler,
    .entry_handler = entry_handler,
    .maxactive     = 20,
    .data_size = sizeof(struct probe_data),
};

static int __init fault_hook_init(void)
{
    int ret;
    my_kretprobe.kp.symbol_name = func_name;
    ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("fault_hook: Registrazione fallita su %s, codice: %d\n", func_name, ret);
        return ret;
    }
    pr_info("fault_hook: Hook inserito su %s. Pronto per il trigger.\n", func_name);
    return 0;
}

static void __exit fault_hook_exit(void)
{
    unregister_kretprobe(&my_kretprobe);
    pr_info("fault_hook: Hook rimosso.\n");
}

module_init(fault_hook_init);
module_exit(fault_hook_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fault Injection LLM");
