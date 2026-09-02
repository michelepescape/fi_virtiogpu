#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/delay.h>
#include <linux/ptrace.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/string.h>

static char *target_comm = "llama-cli";
module_param(target_comm, charp, 0644);
MODULE_PARM_DESC(target_comm, "Nome del processo da colpire (current->comm). Vuoto = qualunque processo (sconsigliato)");

static inline bool is_target_process(void)
{
    if (!target_comm || target_comm[0] == '\0') {
        return true; /* nessun filtro, comportamento originale */
    }
    return strncmp(current->comm, target_comm, TASK_COMM_LEN) == 0;
}

/* Nome della funzione da agganciare. Fisso al caricamento (register_kretprobe
 * usa symbol_name solo in init): per cambiare driver-point nella campagna,
 * rmmod + insmod con func_name diverso. */
static char *func_name = "virtio_gpu_execbuffer_ioctl"; 
module_param(func_name, charp, 0444); // la rendo modificabile se faccio 0644??
MODULE_PARM_DESC(func_name, "Nome della funzione da agganciare");

static int inject_delay_ms = 0;
module_param(inject_delay_ms, int, 0644);
MODULE_PARM_DESC(inject_delay_ms, "Ritardo in ms iniettato ad ogni occorrenza fino a reset. 0 = disabilitato");

/* Quale occorrenza della funzione colpire, contando da quando inject_error
 * viene scritto (evento di arming). 1 = la prossima chiamata in assoluto,
 * N = la N-esima. Default 1: adatto a quando l'hook applicativo scrive
 * inject_error esattamente subito prima della chiamata che vuoi colpire. */
static int target_occurrence = 1;
module_param(target_occurrence, int, 0644);
MODULE_PARM_DESC(target_occurrence, "Quale occorrenza della funzione colpire da quando inject_error viene scritto");

static atomic_t call_counter = ATOMIC_INIT(0);
static atomic_t armed        = ATOMIC_INIT(0);
static int last_inject_error = 0; /* per il getter */

/* IMPORTANTE: scrivere un valore non-zero su inject_error e' L'EVENTO DI ARMING.
 * Questo fa coincidere il comportamento con l'hook C++ applicativo, che scrive
 * solo questo parametro (nessun parametro 'arm' separato da settare a parte).
 * Scrivere 0 disarma esplicitamente senza attendere un trigger. */
static int set_inject_error(const char *val, const struct kernel_param *kp)
{
    int tmp;
    int ret = kstrtoint(val, 0, &tmp);
    if (ret) {
        return ret;
    }
    last_inject_error = tmp;
    if (tmp != 0) {
        atomic_set(&call_counter, 0);
        atomic_set(&armed, 1);
        pr_info("fault_hook: ARMATO da write inject_error=%d, target_occurrence=%d, func=%s\n",
                tmp, target_occurrence, func_name);
    } else {
        atomic_set(&armed, 0);
        pr_info("fault_hook: disarmato esplicitamente (inject_error=0)\n");
    }
    return 0;
}
static int get_inject_error(char *buffer, const struct kernel_param *kp)
{
    return sprintf(buffer, "%d\n", last_inject_error);
}
static const struct kernel_param_ops inject_error_ops = {
    .set = set_inject_error,
    .get = get_inject_error,
};
static int inject_error_placeholder;
module_param_cb(inject_error, &inject_error_ops, &inject_error_placeholder, 0644);
MODULE_PARM_DESC(inject_error, "Codice errore da iniettare (es. -5 = -EIO). Scrivere un valore non-zero ARMA il probe.");

static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (!atomic_read(&armed))
        return 1; /* non installare il return-probe, non ci interessa */
    if (!is_target_process())
        return 1; /* idem: processo diverso da target_comm, ignora */
    if (inject_delay_ms > 0) {
        pr_info("fault_hook: [LATENZA] Ritardo di %d ms iniettato su %s\n",
                inject_delay_ms, func_name);
        mdelay(inject_delay_ms);
    }
    return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    int original_retval;
    int n;

    if (!atomic_read(&armed))
        return 0;
    if (!is_target_process())
        return 0; /* difesa extra, non dovrebbe arrivare qui grazie a entry_handler */

    n = atomic_inc_return(&call_counter);

    /* target_occurrence <= 0: modalita' diagnostica "corrompi tutte
     * le occorrenze mentre armato" - utile per verificare in modo
     * grezzo se il path e' capace di avere un effetto, prima di
     * cercare la singola occorrenza esatta con precisione. In questa
     * modalita' NON si autodisarma: resta armato finche' l'host non
     * scrive esplicitamente inject_error=0 (disarm esplicito). */
    if (target_occurrence <= 0) {
        original_retval = regs_return_value(regs);
        if (last_inject_error != 0) {
            pr_info("fault_hook: [FAULT #%d / ALL] Sovrascrittura ritorno di %s: da %d a %d (jiffies=%lu)\n",
                    n, func_name, original_retval, last_inject_error, jiffies);
            regs_set_return_value(regs, last_inject_error);
        }
        return 0;
    }

    if (n != target_occurrence)
        return 0; /* non ancora l'occorrenza target, lascia passare pulita */

    original_retval = regs_return_value(regs);
    if (last_inject_error != 0) {
        pr_info("fault_hook: [FAULT #%d] Sovrascrittura ritorno di %s: da %d a %d (jiffies=%lu)\n",
                n, func_name, original_retval, last_inject_error, jiffies);
        regs_set_return_value(regs, last_inject_error);
    }

    /* ONE-SHOT: disarma subito dopo, cosi' le chiamate successive
     * tornano pulite senza intervento host aggiuntivo. */
    atomic_set(&armed, 0);
    return 0;
}

static struct kretprobe my_kretprobe = {
    .handler       = ret_handler,
    .entry_handler = entry_handler,
    .maxactive     = 20,
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
    pr_info("fault_hook: Hook inserito su %s (disarmato, in attesa di write su inject_error)\n", func_name);
    return 0;
}

static void __exit fault_hook_exit(void)
{
    unregister_kretprobe(&my_kretprobe);
    pr_info("fault_hook: Hook rimosso da %s\n", func_name);
}

module_init(fault_hook_init);
module_exit(fault_hook_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Fault injection one-shot, arming implicito su write di inject_error, per campagne FI su driver GPU");
