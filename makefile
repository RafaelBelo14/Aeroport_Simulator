make:
	gcc -c -g main.c -Wall
	gcc -o main main.c -lpthread
