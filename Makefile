CC=cc

dedit: main.c
	$(CC) main.c -o dedit -Wall -Wextra -pedantic -std=c99
