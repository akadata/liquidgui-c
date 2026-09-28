savedcmd_nct6687.mod := printf '%s\n'   nct6687.o | awk '!x[$$0]++ { print("./"$$0) }' > nct6687.mod
