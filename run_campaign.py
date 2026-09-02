import os
import json
import time
import subprocess
import re
from pathlib import Path

# ================= CONFIGURAZIONE INFRASTRUTTURA =================
SSH_HOST = "localhost"
SSH_PORT = "2222"
SSH_USER = "mp"
RESULTS_DIR = Path("campaign_results")

# Timeout massimo di esecuzione per singola run (in secondi)
EXEC_TIMEOUT = 45

QEMU_CMD = [
    "qemu-system-x86_64", "-enable-kvm", "-m", "12G", "-smp", "4", "-cpu", "host",
    "-machine", "q35",
    "-drive", "file=ubuntu-test.qcow2,if=virtio,format=qcow2",
    "-snapshot",
    "-netdev", "user,id=net0,hostfwd=tcp::2222-:22",
    "-device", "virtio-net-pci,netdev=net0",
    "-device", "virtio-gpu-gl,venus=on,blob=on,hostmem=8G",
    "-display", "egl-headless,gl=on",
    "-kernel", "/home/mp/Downloads/linux-7.1.2/linux/arch/x86/boot/bzImage",
    "-initrd", "./initrd_guest.img",
    "-append", "root=/dev/mapper/ubuntu--vg-ubuntu--lv console=tty0 console=ttyS0",
    "-serial", "file:guest_kernel.log"
]

# ================= CONFIGURAZIONE ESPERIMENTI =================
LLAMA_CMD_BASE = (
    "/home/mp/llama.cpp/build/bin/llama-cli --no-display-prompt "
    "-c 2048 -b 128 -ub 128 -ngl 99 -st --simple-io --color off --seed 1234 --temp 0"
)

MODELS = {
    "qwen2.5-0.5b": "/home/mp/llama.cpp/models/qwen2.5-0.5b-instruct-q4_k_m.gguf",
    "qwen2.5-1.5b": "/home/mp/llama.cpp/models/qwen2.5-1.5b-instruct-q4_k_m.gguf",
    "qwen2.5-3b": "/home/mp/llama.cpp/models/qwen2.5-3b-instruct-q4_k_m.gguf",
}

PROMPTS = {
    "ragionamento": "A ball is in a yellow box. Someone moves the ball to a blue box. Where is the ball now?",
    "aritmetica": "What is 1542 + 2341? Provide only the number.",
    "scelta_multipla": "Question: Which planet is known as the Red Planet? A) Earth B) Mars C) Jupiter. Answer:",
    "estrazione_dati": "Extract the names of the cities from this text as a JSON list: 'I visited Paris, then took a train to Berlin, and ended up in Rome.' JSON:",
    #"linguistica_it": "Traduci la seguente frase in italiano: 'The system has encountered a critical hardware failure and must be restarted immediately.'"
}

# Verrà popolato automaticamente dalla fase di Baseline
# Useremo i campi:
# "decode_tokens" per memorizzare il numero di token della risposta
# "decode_targets" per memorizzare i token sul quale fare injection (secondo, mediano, finale)
#" expected" prende l'output, potrebbe servire per confronti per accuratezza ma al momento non è usato 
BASELINE_DATA = {} # useremo i campi 

TARGET_FUNCTIONS = [
    "virtio_gpu_execbuffer_ioctl",
    "virtio_gpu_resource_create_blob_ioctl",
    "virtio_gpu_queue_fenced_ctrl_buffer",
    "virtio_gpu_cmd_submit",
]

ALL_FAULTS = [
    {"type": "delay", "val": 1000},
    {"type": "error", "val": -28}, # ENOSPC
    {"type": "error", "val": -5},  # EIO
    {"type": "error", "val": -22}, # EINVAL
    {"type": "corruption", "val": 1},
    {"type": "corruption", "val": 2},
    {"type": "corruption", "val": 3},
]

# Configurazione guasti per funzione: escludiamo "error" da virtio_gpu_cmd_submit (funzione void)
FAULTS_PER_FUNCTION = {
    "virtio_gpu_execbuffer_ioctl": ALL_FAULTS,
    "virtio_gpu_resource_create_blob_ioctl": ALL_FAULTS,
    "virtio_gpu_queue_fenced_ctrl_buffer": ALL_FAULTS,
    "virtio_gpu_cmd_submit": [f for f in ALL_FAULTS if f["type"] != "error"],
}

PHASES = ["MODEL_LOAD", "PREFILL", "DECODE"]

# ================= FUNZIONI DI SUPPORTO =================

def start_vm():
    """Avvia la VM e aspetta che il server SSH sia pronto."""
    print("    [*] Avvio VM...")
    # Avvia QEMU in background e ridireziona i log su file
    global qemu_log_file
    qemu_log_file = open("qemu_host.log", "w")
    qemu_proc = subprocess.Popen(QEMU_CMD, stdout=qemu_log_file, stderr=subprocess.STDOUT)
    
    # Polling per aspettare che SSH risponda (la VM ha finito il boot)
    max_retries = 30
    for _ in range(max_retries):
        rc, _, _ = run_ssh_cmd("echo 'ready'", timeout=5)
        if rc == 0:
            return qemu_proc
        time.sleep(2)
        
    print("    [!] Timeout attesa avvio VM.")
    stop_vm(qemu_proc)
    return None

def stop_vm(qemu_proc):
    """Spegne la VM."""
    if qemu_proc:
        qemu_proc.terminate()
        qemu_proc.wait() # Aspetta che si chiuda davvero
    global qemu_log_file
    if 'qemu_log_file' in globals() and not qemu_log_file.closed:
        qemu_log_file.close()

def run_ssh_cmd(cmd, timeout=EXEC_TIMEOUT):
    ssh_cmd = [
        "ssh", "-p", SSH_PORT,
        "-o", "StrictHostKeyChecking=no",
        "-o", "BatchMode=yes",
        f"{SSH_USER}@{SSH_HOST}",
        cmd
    ]
    try:
        proc = subprocess.run(ssh_cmd, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        return 124, e.stdout.decode() if e.stdout else "", "[TIMEOUT EXPIRED]"

def extract_decode_tokens(stderr_text):
    """Estrae i token di DECODE generati leggendo il print_timing di llama.cpp"""
    for line in stderr_text.split('\n'):
        # Cerca "eval time =" ma ignora la riga "prompt eval time ="
        if "eval time =" in line and "prompt" not in line and "tokens" in line:
            match = re.search(r'/\s*(\d+)\s*tokens', line)
            if match:
                return int(match.group(1))
    return 3 # Fallback minimo

def clean_llama_output(stdout_text, prompt_text):
    """Estrae solo la risposta generata da llama-cli, rimuovendo timing e banner."""
    # Rimuove le info finali sui timing
    stdout_text = re.sub(r'\[ Prompt:.*?Exiting\.\.\.', '', stdout_text, flags=re.DOTALL)
    
    # Rimuove tutto fino all'ultimo pezzo del prompt
    prompt_snippet = prompt_text[-20:]
    if prompt_snippet in stdout_text:
        parts = stdout_text.split(prompt_snippet)
        return parts[-1].strip()
    return stdout_text.strip()

def save_run(out_dir, stdout, stderr, run_meta):
    os.makedirs(out_dir, exist_ok=True)
    with open(out_dir / "stdout.txt", "w") as f: f.write(stdout)
    with open(out_dir / "stderr.txt", "w") as f: f.write(stderr)
    with open(out_dir / "meta.json", "w") as f: json.dump(run_meta, f, indent=4)
    
    import shutil
    if os.path.exists("guest_kernel.log"):
        shutil.copy("guest_kernel.log", out_dir / "guest_kernel.log")
    if os.path.exists("qemu_host.log"):
        shutil.copy("qemu_host.log", out_dir / "qemu_host.log")
    
    if run_meta.get("status") != "HANG":
        rc, dmesg_out, _ = run_ssh_cmd("sudo dmesg | grep -E 'fault_hook|\\[FI\\]|drm.*virtio'", timeout=5)
        with open(out_dir / "dmesg.txt", "w") as f: 
            f.write(dmesg_out if rc == 0 else "[Nessun log kernel registrato o errore]")
    else:
        with open(out_dir / "dmesg.txt", "w") as f:
            f.write("[VM IN HANG - dmesg irraggiungibile via SSH]\n")

# ================= LOOP PRINCIPALE =================

def main():
    print("[*] Inizializzazione Campagna FI...")
    
    # SALVATAGGIO STATO PULITO INIZIALE --- non serve più, usiamo snapshot qemu
    #print("[*] Creo snapshot pulito: savevm baseline_clean")
    #send_qemu_cmd("savevm baseline_clean")
    #time.sleep(2)
    #send_qemu_cmd("c") # Forza la ripresa

    # GENERAZIONE AUTOMATICA BASELINE
    print("\n" + "="*55)
    print("[*] FASE 1: GENERAZIONE BASELINE AUTOMATICA")
    print("="*55)
    
    for model_name, model_path in MODELS.items():
        BASELINE_DATA[model_name] = {}
        for prompt_name, prompt_text in PROMPTS.items():
            # Meccanismo per Resume così da non ripetere esecuzione baseline se già presente
            baseline_dir = RESULTS_DIR / model_name / prompt_name / "baseline"
            meta_file = baseline_dir / "meta.json"
            
            # Se la baseline esiste già su disco, la riusiamo senza rieseguire
            if meta_file.exists():
                # consideriamo esistente la baseline se già troviamo il corrispondente file meta.json, da cui recuperiamo il numero di token
                with open(meta_file, "r") as f: meta = json.load(f)
                with open(baseline_dir / "stdout.txt", "r") as f: stdout_content = f.read()
                tokens = meta.get("tokens", 3)

                decode_targets = sorted(list(set([2, max(2, tokens // 2), tokens]))) # calcolo anche i token per la decode sui quali fare fault injection

                BASELINE_DATA[model_name][prompt_name] = {
                    "decode_tokens": tokens,
                    "decode_targets": decode_targets, 
                    "expected": stdout_content.strip()
                }
                print(f" -> Baseline {model_name} | {prompt_name} [GIA' PRESENTE - Token Decode: {tokens}]")
                continue

            print(f" -> Genero baseline per {model_name} | {prompt_name}...")
            

            qemu_process = start_vm()
            if not qemu_process:
                print("    [!] Impossibile avviare la VM. Salto baseline.")
                continue 

           # send_qemu_cmd("loadvm baseline_clean")
            #time.sleep(2)
            #send_qemu_cmd("c") # Forza la ripresa
            #time.sleep(1)
            
            try:
                # Reset fault injector, assicuriamoci che il modulo non sia attivato
                run_ssh_cmd("sudo rmmod fault_injection 2>/dev/null || true")
                
                # Pulizia buffer dmesg per non avere log sporchi
                run_ssh_cmd("sudo dmesg -c > /dev/null")

                full_cmd = f"{LLAMA_CMD_BASE} -lv 3 -m {model_path} -p \"{prompt_text}\"" # con -lv 3 stampo il livello info di verbosità
                # rc = return code
                # bout = Baseline OUTput (testo generato stdout)
                # berr = Baseline ERRor (log e timing stderr)
                rc, bout, berr = run_ssh_cmd(full_cmd, timeout=60)
                
                if rc != 0:
                    print(f" [!] Errore critico durante la baseline ({rc}). Salto.")
                    continue
                    
                tokens = extract_decode_tokens(berr)

                decode_targets = sorted(list(set([2, max(2, tokens // 2), tokens]))) # calcolo anche i token per la decode sui quali fare fault injection

                BASELINE_DATA[model_name][prompt_name] = {
                    "decode_tokens": tokens,
                    "decode_targets": decode_targets,
                    "expected": bout.strip()
                }
                
                save_run(baseline_dir, bout, berr, {
                    "status": "SUCCESS",
                    "tokens": tokens,
                    "is_baseline": True,
                    "model": model_name,
                    "prompt": prompt_name,
                    "expected": bout.strip()
                })
                
                print(f"    [+] Fatto! Token Decode: {tokens}")
            finally:
                stop_vm(qemu_process)
        


    # CAMPAGNA FAULT INJECTION
    print("\n" + "="*55)
    print("[*] FASE 2: INIEZIONE GUASTI (FI)")
    print("="*55)

    # --- CALCOLO TOTALE ESPERIMENTI ---
    total_experiments = 0
    for model_name in MODELS:
        for prompt_name in PROMPTS:
            if prompt_name not in BASELINE_DATA.get(model_name, {}): continue
                        
            #Leggiamo direttamente i target già calcolati
            decode_targets = BASELINE_DATA[model_name][prompt_name]["decode_targets"]
            
            # (2 fasi fisse con 1 token) + (token variabili per il DECODE)
            num_tokens_per_fault = 2 + len(decode_targets)
            for func in TARGET_FUNCTIONS:
                total_experiments += len(FAULTS_PER_FUNCTION[func]) * num_tokens_per_fault

    print(f"[*] Totale esperimenti calcolati: {total_experiments}\n")
    current_experiment = 0


    # --- LOOP PRINCIPALE ---
    for model_name, model_path in MODELS.items():
        for prompt_name, prompt_text in PROMPTS.items():
            
            if prompt_name not in BASELINE_DATA[model_name]: continue
            
            decode_targets = BASELINE_DATA[model_name][prompt_name]["decode_targets"]
            baseline_expected = BASELINE_DATA[model_name][prompt_name]["expected"]

            for func in TARGET_FUNCTIONS:
                
                # Applichiamo i soli guasti validi per la funzione target corrente
                for f_conf in FAULTS_PER_FUNCTION[func]:
                    config_folder_name = f"{f_conf['type']}_{f_conf['val']}"

                    for phase in PHASES:
                        tokens_to_test = [1] if phase != "DECODE" else decode_targets
                        
                        for token in tokens_to_test:
                            current_experiment += 1

                            run_dir = RESULTS_DIR / model_name / prompt_name / func / config_folder_name / phase / f"token_{token}"

                            # Se la run esiste già, la salta 
                            if (run_dir / "meta.json").exists():
                                print(f"[{current_experiment}/{total_experiments}] [SALTATO - GIA' PRESENTE] FI: {model_name[:8]}|{prompt_name[:8]} -> {func} | {config_folder_name} | {phase} | Token {token}")
                                continue

                            print(f"[{current_experiment}/{total_experiments}] FI: {model_name[:8]}|{prompt_name[:8]} -> {func} | {config_folder_name} | {phase} | Token {token}")

                            qemu_process = start_vm()
                            if not qemu_process:
                                print("    [!] Impossibile avviare VM per FI. Salto run.")
                                continue 
                            
                            try: 
                                # insmod pulito e chmod
                                setup_cmd = (
                                    "sudo rmmod fault_injection 2>/dev/null || true ; "
                                    f"sudo insmod /home/mp/fault_injection.ko func_name={func} target_comm=llama-cli ; "
                                    "sleep 0.2 ; "
                                    "sudo chown -R mp:mp /sys/module/fault_injection/parameters/ ; "
                                    "sudo chmod 666 /sys/module/fault_injection/parameters/* ; "
                                    "sudo dmesg -c > /dev/null"
                                )
                                rc_setup, _, err_setup = run_ssh_cmd(setup_cmd, timeout=10)
                                if rc_setup != 0:
                                    print(f"    [!] Avviso: Risultato setup modulo ({rc_setup}): {err_setup.strip()}")
                                

                                fc, delay, corrupt = 0, 0, 0
                                if f_conf["type"] == "error": fc = f_conf["val"]
                                elif f_conf["type"] == "delay": delay = f_conf["val"]
                                elif f_conf["type"] == "corruption": corrupt = f_conf["val"]
                                
                                env_vars = (
                                    f"FI_TARGET_PHASE={phase} "
                                    f"FI_TARGET_TOKEN={token} "
                                    f"FI_TARGET_OCCURRENCE=1 "
                                    f"FI_BURST_LENGTH=1 "
                                    f"FI_FAULT_CODE={fc} "
                                    f"FI_DELAY_MS={delay} "
                                    f"FI_DESCRIPTOR_CORRUPTION={corrupt}"
                                )
                                
                                full_cmd = f"{env_vars} {LLAMA_CMD_BASE} -m {model_path} -p \"{prompt_text}\""
                                # rc = return code
                                # tout = Test OUTput (stdout generato sotto guasto)
                                # terr = Test ERRor (stderr sotto guasto)
                                rc, tout, terr = run_ssh_cmd(full_cmd, timeout=45)
                                
                                if rc == 124:
                                    status = "HANG"
                                elif rc != 0:
                                    status = f"CRASH (Codice {rc})"
                                else:
                                    # Llama-cli esce con codice 0 anche in caso di errori gestiti internamente (es. Vulkan OOM)
                                    if "Error:" in tout or "failed:" in tout or "Error:" in terr or "failed:" in terr:
                                        status = "APP_ERROR"
                                    else:
                                        status = "SUCCESS"
                                
                                # Use cleaned outputs purely for the match validation
                                clean_tout = clean_llama_output(tout, prompt_text)
                                clean_base = clean_llama_output(baseline_expected, prompt_text)
                                is_match = (clean_tout == clean_base) if status == "SUCCESS" else False

                                meta = {
                                    "status": status,
                                    "exit_code": rc,
                                    "phase": phase,
                                    "token": token,
                                    "fault_type": f_conf["type"],
                                    "fault_val": f_conf["val"],
                                    "target_function": func,
                                    "model": model_name,
                                    "prompt": prompt_name,
                                    "output_match": is_match,
                                    "baseline_output": baseline_expected,
                                }
                                save_run(run_dir, tout, terr, meta)
                            finally:
                                stop_vm(qemu_process)

    print("\n[*] Campagna completata")

if __name__ == "__main__":
    main()
