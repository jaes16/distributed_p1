all: server server_thread server_process server_edriven

server: server.c
	gcc -g -o server server.c

server_nothread: server_thread.c
	gcc -g -o server_thread server_thread.c

server_process: server_process.c
	gcc -g -o server_process server_process.c

server_edriven: server_edriven.c
	gcc -g -o server_edriven server_edriven.c