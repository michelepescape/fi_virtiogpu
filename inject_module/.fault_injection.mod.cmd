savedcmd_fault_injection.mod := printf '%s\n'   fault_injection.o | awk '!x[$$0]++ { print("./"$$0) }' > fault_injection.mod
