obj-m += fault_injection.o

# Sostituisci questo percorso con la cartella dei tuoi sorgenti kernel compilati
KDIR := /home/mp/Downloads/linux-7.1.2/linux
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

