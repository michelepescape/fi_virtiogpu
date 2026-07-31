# Setup Ambiente

## 

Sull'host creiamo una directory dedicata per l'ambiente e scarichiamo l'immagine di 
Ubuntu Server 26.04.

```sh
mkdir -p ~/virtio-gpu-test
cd ~/virtio-gpu-test

# Download dell'immagine Ubuntu Server
wget https://releases.ubuntu.com/26.04/ubuntu-26.04-live-server-amd64.iso

# Creazione del disco virtuale (formato qcow2)
qemu-img create -f qcow2 ubuntu-test.qcow2 30G
```

## Installazione e configurazione del Sistema Operativo Guest

Avviamo QEMU per l'installazione. Durante il processo ci assicuriamo di installare anche il server OpenSSH e creiamo 
un utente standard `mp`.

```sh
qemu-system-x86_64 \
  -enable-kvm \
  -m 4G \
  -smp 4 \
  -cpu host \
  -machine q35 \
  -drive file=ubuntu-test.qcow2,if=virtio,format=qcow2 \
  -cdrom ubuntu-24.04-live-server-amd64.iso \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-vga \
  -display gtk,gl=off
```

A questo punto possiamo riavviare la VM omettendo il parametro -cdrom ed eseguendo così il boot dal 
disco virtuale. 

Per permettere l'esecuzione di comandi con `sudo` e il collegamento ssh tramite l'harness senza dover 
inserire la password manualmente, modifichiamo `sudoers` e poi creiamo una chiave per ssh.

```sh
# Apriamo sudoers
sudo visudo
```

Al suo interno inseriamo la stringa `NOME_UTENTE ALL=(ALL) NOPASSWD: ALL`

<img width="535" height="386" alt="Screenshot From 2026-07-27 13-05-28" src="https://github.com/user-attachments/assets/bf3e5915-01ec-45c3-871b-15b33c8fb8fc" />


Per la creazione della chaive ssh invece:

```sh
ssh-keygen -t rsa -N ""

ssh-copy-id -p 2222 mp@localhost
```

A questo punto installiamo le dipendenze per Vulkan e compiliamo llama.cpp

```sh
sudo apt-get update
sudo apt-get install libvulkan-dev glslc spirv-headers vulkan-tools build-essential cmake

# Clonazione e build
git clone https://github.com/ggml-org/llama.cpp.git
cd llama.cpp
cmake -B build -DGGML_VULKAN=1
cmake --build build --config Release
```

### Compilazione kernel

Per iniettare i fault, serve un kernel compilato con `CONFIG_FAULT_INJECTION` abilitato. Sull'host quindi scarichiamo l'ultimo kernel stabile
e creiamo una configurazione di base:

```sh
# Download del kernel stabile desiderato
git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git --branch v7.1.2
cd linux

# Configurazione base per x86 e virtualizzazione KVM
make x86_64_defconfig
make kvm_guest.config
```

Tramite make xconfig (o menuconfig), abilitiamo le seguenti flag:
```sh
CONFIG_FAULT_INJECTION=y
CONFIG_FAULT_INJECTION_DEBUG_FS=y
```


A questo punto compiliamo il kernel e d'ora in poi lo passiamo come parametro all'avvio della VM
aggiungendo i flag `-kernel`, `-initrd` e tramite `-append` ci assicuriamo di eseguire la partizione
corretta aggirando GRUB.

```sh
make -j$(nproc)
```

```sh
qemu-system-x86_64 \
  -enable-kvm \
  -m 8G \
  -smp 4 \
  -cpu host \
  -machine q35 \
  -drive file=ubuntu-test.qcow2,if=virtio,format=qcow2 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-gpu-gl,venus=on,blob=on,hostmem=4G \
  -display gtk,gl=on,show-cursor=on \
  -kernel /home/mp/Downloads/linux-7.1.2/linux/arch/x86/boot/bzImage \
  -initrd ./initrd_guest.img \
  -append "root=/dev/mapper/ubuntu--vg-ubuntu--lv console=tty0"
```
### Autologin e verifica kprobe 
Considerando che il driver per la scheda video viene caricato successivamente al login dell'utente,
per semplicità impostiamo anche il login automatico per l'utente `mp`

```sh
sudo systemctl edit getty@tty1
```

Inseriamo il seguente blocco:

```sh
[Service]
ExecStart=
ExecStart=-/sbin/agetty -o '-p -f -- \\u' --noclear --autologin mp %I $TERM
```


All'interno del guest, possiamo verificare il funzionamento base di kprobe usando tracefs, prima ancora di scrivere un modulo dedicato.

```sh

cd /sys/kernel/debug/tracing

# Creazione di una kprobe all'ingresso della ioctl
sudo bash -c "echo 'p:my_virtio_probe virtio_gpu_execbuffer_ioctl' > kprobe_events"

# Abilitazione della sonda
sudo bash -c "echo 1 > events/kprobes/my_virtio_probe/enable"
```
Eseguendo un carico sulla GPU (es. vulkaninfo --summary), il file /sys/kernel/debug/tracing/trace si popolerà con gli eventi catturati.

<img width="792" height="538" alt="Screenshot From 2026-07-05 16-19-56" src="https://github.com/user-attachments/assets/6f7faff2-405a-486d-b4d2-4a2a65263d37" />



Per la creazione del modulo: [fault_injection.md](fault_injection.md)
