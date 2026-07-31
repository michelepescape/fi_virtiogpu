// Da aggiungere all'inizio del file 


// -------------Fault injection addition-----------
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace fi {

static bool        hook_initialized = false;
static std::string target_phase     = "";   // "MODEL_LOAD" | "PREFILL" | "DECODE"
static int         target_token     = -1;   // rilevante solo per DECODE
static int         fault_code       = 0;
static int         delay_ms         = 0;
static int         burst_len        = 1; // default 1 = singola ioctl
static int         target_occ       = 1; // default 1 = prossima chiamata
static int         descriptor_corruption = 0; // 0=Disattivato, 1=Size, 2=CmdSize/InFence, 3=Flags/Ring
static bool         fault_fired      = false;

constexpr const char* SYSFS_DIR = "/sys/module/fault_injection/parameters/";

inline void write_sysfs(const std::string& param, int value) {
    std::ofstream f(std::string(SYSFS_DIR) + param);
    if (f.is_open()) {
        f << value;
        f.close();
    }  else {
        std::cerr << "[FI] ERROR: cannot open " << param << std::endl;
    }
}

inline void load_config_once() {
    if (hook_initialized) return;
    hook_initialized = true;
    if (const char* p = std::getenv("FI_TARGET_PHASE")) target_phase = p;
    if (const char* t = std::getenv("FI_TARGET_TOKEN"))  target_token = std::atoi(t);
    if (const char* f = std::getenv("FI_FAULT_CODE"))    fault_code   = std::atoi(f);
    if (const char* d = std::getenv("FI_DELAY_MS"))     delay_ms     = std::atoi(d);
    if (const char* b = std::getenv("FI_BURST_LENGTH"))      burst_len    = std::atoi(b);
    if (const char* o = std::getenv("FI_TARGET_OCCURRENCE")) target_occ   = std::atoi(o);
    if (const char* c = std::getenv("FI_DESCRIPTOR_CORRUPTION")) descriptor_corruption = std::atoi(c);

    std::cerr << "[FI] config: phase=" << target_phase
              << " token=" << target_token
              << " fault_code=" << fault_code
              << " delay_ms=" << delay_ms 
              << " burst=" << burst_len
              << " occ=" << target_occ
              << " corruption=" << descriptor_corruption << std::endl;
}


// storage del TID del thread corrente, popolato da capture_current_tid()
// e usato da trigger_hardware_fault() al momento di armare il fault.
static int target_tid_cache = -1;
 
// Cattura il TID (SYS_gettid) del thread che sta per invocare il punto
// strumentato. Va chiamata SUL THREAD GIUSTO, subito prima dell'hook,
// non una volta sola all'avvio: isola il fault al thread esatto che fa
// la chiamata di interesse (es. il thread di inferenza, non un thread
// di cleanup che condivide lo stesso nome processo).
inline void capture_current_tid() {
    // current->pid nel kernel == SYS_gettid() in userspace (nomenclatura
    // storicamente invertita: quello che il kernel chiama "pid" e' il TID,
    // "tgid" e' il vero PID di processo condiviso tra thread).
    static thread_local int tid = -1;
    if (tid == -1) {
        tid = static_cast<int>(syscall(SYS_gettid));
    }
    target_tid_cache = tid;
}



inline void trigger_fault() {
    write_sysfs("inject_error", fault_code);
    write_sysfs("inject_delay_ms", delay_ms);
    write_sysfs("burst_length", burst_len);
    write_sysfs("target_occurrence", target_occ);
    write_sysfs("descriptor_corruption", descriptor_corruption); 
    write_sysfs("target_pid", target_tid_cache);
    write_sysfs("trigger", 1); // Arma la kprobe

    fault_fired = true;
    std::cerr << "[FI] FAULT ARMED code=" << fault_code
              << " delay=  " << delay_ms << "ms burst=" << burst_len
              << " occ=" << target_occ
              << " corruption=" << descriptor_corruption
              << " tid=" << target_tid_cache
              << " phase=" << target_phase << std::endl;
}

// chiamata da load_model(), prima di common_init_from_params()
inline void hook_model_load() {
    load_config_once();
    if (fault_fired) return;
    capture_current_tid();
    if (target_phase == "MODEL_LOAD") {
        trigger_fault();
    }
}

// chiamata da decode(), prima di llama_decode()
// n_decoded_before: slot.n_decoded del (singolo, con --parallel 1) slot attivo,
//                   letto PRIMA di questa chiamata a llama_decode()
inline void hook_prefill_decode(bool is_prefill, int n_decoded_before) {
    load_config_once();
    if (fault_fired) return;
    capture_current_tid();

    if (is_prefill) {
        if (target_phase == "PREFILL") {
            trigger_fault();
        }
        return;
    }

    // DECODE: la chiamata che sta per partire produrra' il token
    // numero (n_decoded_before + 1)
    if (target_phase == "DECODE" && (n_decoded_before + 1) == target_token) {
        trigger_fault();
    }
}

} // namespace fi

// --------------------End fault injection Addition-----------------




// Da aggiungere in bool load_model(common_params & params), tra le istruzioni riportate

        {
            params_base.load_progress_callback = load_progress_callback;
            params_base.load_progress_callback_user_data = &load_progress_text;
        }

        // ---------------Fault injection addition--------------
	fi::hook_model_load();

	
	// - -------------------End Fault injection addition---------------

	llama_init = common_init_from_params(params_base);





// Da aggiungere in bool decode(int32_t & n_batch, int32_t off, llama_batch & batch_view) subito prima di llama_decode()
      } else {
            n_empty_consecutive = 0;
        }

	// ------------------ Fault injection Addition ----------------------
        {
            bool is_prefill = false;
            int  n_decoded_before = -1;
            for (auto & slot : slots) {
                if (slot.is_processing()) {
                    is_prefill = (slot.state == SLOT_STATE_PROCESSING_PROMPT ||
                                  slot.state == SLOT_STATE_STARTED  ||
				  (slot.state == SLOT_STATE_DONE_PROMPT && slot.n_decoded == 0));
                    n_decoded_before = slot.n_decoded;
                    break; // con --parallel 1 (cioè il default) ce n'e' solo uno di slot/job serviti contemporaneamnte
                }
            }
            if (n_decoded_before >= 0) {
                fi::hook_prefill_decode(is_prefill, n_decoded_before);
            }
        }

	// -----------------------Fault injection end addition ------------------

        const int ret = llama_decode(ctx_tgt, batch_view);