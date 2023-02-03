#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

extern int errno;

#define PORT 8080
#define BUF_SIZE 4096 // Max line length but also max packet length
#define MAX_FILE_SIZE 999999999

// int send_on_socket(int socket, void *buffer, size_t buf_len){
int send_on_socket(int socket, void *buffer, size_t buf_len, struct sockaddr *dest_addr, socklen_t dest_len){
    int sent = 0;
    while (sent < buf_len){
        sent += send(socket, buffer+sent, buf_len-sent, 0);
        // sent += sendto(socket, buffer+sent, buf_len-sent, 0, dest_addr, dest_len);
    }
    // printf("%s\n", (char *) buffer);
    return sent;
}

// int read_file_and_send(char *file_path, int socket){
int read_file_and_send(char *file_path, int socket, struct sockaddr *dest_addr, socklen_t dest_len){
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    fp = fopen(file_path, "r");
    if (fp == NULL){
        return -1;
    }

    // find length of file so we can tell the content-length
    size_t file_len = 0;
    fseek(fp, 0L, SEEK_END);
    file_len = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    if (file_len > MAX_FILE_SIZE) { return -2; }
    
    char header_str[BUF_SIZE];
    memset(header_str, 0, BUF_SIZE);


    // create string to send
    char *send_str = (char *) malloc(sizeof(char) * BUF_SIZE * 10);
    memset(send_str, 0, BUF_SIZE * 10);
    int send_str_len = 0;

    if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 5, ".html", 5) == 0) {
        sprintf(header_str, "HTTP/1.1 200 OK\r\nConnection: open\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
        // fread(send_str, 1, file_len, fp);
        while ((read = getline(&line, &len, fp)) != -1) {
            strcat(send_str, line);
            send_str_len += strnlen(line, len);
        }
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".jpg", 4) == 0) {
        sprintf(header_str, "HTTP/1.1 200 OK\r\nConnection: open\r\nAccept-Ranges: bytes\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
        // fread(send_str, 1, file_len, fp);
        char ch;
        for (send_str_len = 0; send_str_len < file_len; send_str_len++) {
            ch = fgetc(fp);
            // printf("%c", ch);
            send_str[send_str_len] = ch;
        }
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".gif", 4) == 0) {
        sprintf(header_str, "HTTP/1.1 200 OK\r\nConnection: open\r\nAccept-Ranges: bytes\r\nContent-Type: image/gif\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
        // fread(send_str, 1, file_len, fp);
        char ch;
        for (send_str_len = 0; send_str_len < file_len; send_str_len++) {
            ch = fgetc(fp);
            // printf("%c", ch);
            send_str[send_str_len] = ch;
        }
    }
    send_on_socket(socket, header_str, strnlen(header_str, BUF_SIZE), dest_addr, dest_len);
    

    printf("Sending:\n%s%s\nDone sending\n\n", header_str, send_str);
    // send_on_socket(socket, (void *) send_str, strlen(send_str));
    send_on_socket(socket, send_str, send_str_len, dest_addr, dest_len);

    

    fclose(fp);
    if (line){ free(line); }
    if (send_str){ free(send_str); }

    return 0;
}

int main() {
    printf("-------------STARTING SERVER-------------");
    const char index[] = "HTTP/1.1 200 OK\r\nContent-Length: 98\r\nConnection: open\r\n\r\n<html><head><title>sysnet</title></head><body>Hello. Welcome to the sysnet homepage.</body></html>";
    // const char index[] = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: close\r\n\r\nHelloWorld!";

    // get appropriate protocal num
    struct protoent *protocal_entry;
    protocal_entry = getprotobyname("IP");

    // create end point
    int sock_desc = 0;
    sock_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_desc < 0) { perror("socket failed."); return -1; }

    // bind
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    int ret = bind(sock_desc, (struct sockaddr*) &address, addrlen);
    if (ret < 0) { perror("bind failed."); return -1; }

    // listen
    char buffer[BUF_SIZE] = {0};
    // wait until someone connects
    ret = listen(sock_desc, 3);
    if (ret < 0) { perror("listen failed."); return -1; }

    while(1){
            
        // accept
        int new_socket = accept(sock_desc, (struct sockaddr *) &address, (socklen_t *) &addrlen);
        if (new_socket < 0) { perror("accept failed"); }

       // accepted new connection!
        int prev_was_newl = 0;
        // read from new socket
        memset(buffer, 0, BUF_SIZE);
        ret = read(new_socket, buffer, BUF_SIZE);
        if (ret < 0) { perror("read failed"); continue; }
        printf("Read in request:\n\'\'\'\n%s\n\'\'\'\nEnd of Request.\n\n", buffer);
        
        // interpret
        if (strncmp(buffer, "GET ", 4) == 0){
            char get_path[4096] = {0};
            memcpy(get_path, "files", 5);
            for (int i = 4; buffer[i] != ' '; i++){
                get_path[i+1] = buffer[i];
            }
            if (get_path[5] == '/' && get_path[6] == 0) { memcpy(get_path+5, "/index.html", 11); }
            // read_file_and_send(get_path, new_socket);
            printf("Requested Dest: %s\n\n", get_path);
            ret = read_file_and_send(get_path, new_socket, (struct sockaddr *) &address, (socklen_t) addrlen);
            if (ret == -1) { send_on_socket(new_socket, "HTTP/1.1 404 File Not Found", 27, (struct sockaddr *) &address, (socklen_t) addrlen); printf("404 File Not Found\n"); }
        }

        // close new connected socket
        close(new_socket);

    }

    // close listening socket
    shutdown(sock_desc, SHUT_RDWR);

    return 0;
}