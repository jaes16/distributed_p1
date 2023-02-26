#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>

extern int errno;

#define PORT 8080
#define BUF_SIZE 4096 // Max line length but also max packet length
#define MAX_FILE_SIZE 999999999
#define MAX_TIMEOUT 100

#define RESPONSE400 "HTTP/1.1 400 Bad Request\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 224\r\n\r\n"
#define RESPONSE403 "HTTP/1.1 403 Forbidden\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 211\r\n\r\n"
#define RESPONSE404 "HTTP/1.1 404 File Not Found\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 201\r\n\r\n"
#define RESPONSE408 "HTTP/1.1 408 Connection Timed Out\r\n\r\n"
//#define RESPONSE408 "HTTP/1.1 408 Connection Timed Out\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 0\r\n"

int send_on_socket(int socket, void *buffer, size_t buf_len) {
    int sent = 0;
    while (sent < buf_len){
        sent += send(socket, buffer+sent, buf_len-sent, 0);
    }
    // printf("%s\n", (char *) buffer);
    return sent;
}


void send_error(int socket, int error) {
    int fd, send_str_len;
    char buffer[BUF_SIZE];

    if (error == 400) {
        send_on_socket(socket, RESPONSE400, sizeof(RESPONSE400));
        fd = open("files/400.html", O_RDONLY);
    } else if (error == 403) {
        send_on_socket(socket, RESPONSE403, sizeof(RESPONSE403));
        fd = open("files/403.html", O_RDONLY);
    } else if (error == 404) {
        send_on_socket(socket, RESPONSE404, sizeof(RESPONSE404));
        fd = open("files/404.html", O_RDONLY);
    } else if (error == 408) {
        send_on_socket(socket, RESPONSE408, sizeof(RESPONSE408));
        return;
    }

    while ((send_str_len = read(fd, buffer, sizeof(buffer))) > 0) {
        send_on_socket(socket, buffer, send_str_len); // send only what was read (send_str_len)
    }
    close(fd);
}


void get_header(char *file_path, char* header_str, off_t file_len) {
    if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 5, ".html", 5) == 0) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".jpg", 4) == 0) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".gif", 4) == 0) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: image/gif\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
    return;
}


int read_file_and_send(char *file_path, int socket) {
    int fd, send_str_len;
    off_t file_len;
    char header_str[BUF_SIZE] = {0};
    char send_str[BUF_SIZE];

    fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES) { return EACCES; }
        else if (errno == ENOENT) {  return ENOENT; }
    }

    // find length of file so we can tell the content-length
    file_len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (file_len > MAX_FILE_SIZE) { return -2; }
    
    // send header
    get_header(file_path, header_str, file_len);
    send_on_socket(socket, header_str, strnlen(header_str, BUF_SIZE));

    // send file
    while ((send_str_len = read(fd, send_str, BUF_SIZE)) > 0) {
        send_on_socket(socket, send_str, send_str_len); // send only what was read (send_str_len)
    }

    close(fd);
    return 0;
}


int interpret_get(char *buffer, char *get_path) {
    // interpret
    if (strncmp(buffer, "GET ", 4) == 0){
        memset(get_path, 0, BUF_SIZE);
        memcpy(get_path, "files", 5);
        for (int i = 4; buffer[i] != ' '; i++){
            get_path[i+1] = buffer[i];
        }
        
        if (get_path[5] == '/' && get_path[6] == 0) { memcpy(get_path+5, "/index.html", 11); }
        printf("Requested Dest: %s\n\n", get_path);
        
        return 0;
    }
    else {
        return 400;
    }
}


int client_service(int new_socket) {
    printf("process %d with socket %d\n", getpid(), new_socket);

    // listen
    char buffer[BUF_SIZE];
    char get_path[BUF_SIZE];
    int ret;
    struct timeval tv;

    while(1){

        // read from new socket
        memset(buffer, 0, BUF_SIZE);
        ret = recv(new_socket, buffer, BUF_SIZE, 0);
        if (ret < 0) { // timeout!
            if (errno == ETIMEDOUT || errno == EAGAIN) {
                send_error(new_socket, 408);
                break;
            }
            perror("recv failed"); continue; 
        }
        if (ret == 0) { break; } // client left

        // reset timeout
        tv.tv_sec = MAX_TIMEOUT;
        if (tv.tv_sec <= 0) { tv.tv_sec = 10; }
        tv.tv_usec = 0;
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const void *) &tv, (socklen_t) sizeof(tv));

        printf("Read in request:\n\'\'\'\n%s\n\'\'\'\nEnd of Request.\n\n", buffer);

        // interpret what file they want
        ret = interpret_get(buffer, get_path);
        if (ret == 400) { send_error(new_socket, 400); continue; }
        
        // read file
        ret = read_file_and_send(get_path, new_socket);
        if (ret < 0) printf("ret %d", ret);
        if (ret == EACCES) { send_error(new_socket, 403); }
        if (ret == ENOENT) { send_error(new_socket, 404); }
        
    }
    close(new_socket);

    return 0;
}


int setup_initial_sock(struct sockaddr_in *address, socklen_t *addrlen) {
    int sock_desc, ret;

    // get appropriate protocal num
    struct protoent *protocal_entry;
    protocal_entry = getprotobyname("IP");

    // create end point
    sock_desc = socket(AF_INET, SOCK_STREAM, protocal_entry->p_proto);
    if (sock_desc < 0) { perror("socket failed."); return -1; }

    // bind
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = INADDR_ANY;
    address->sin_port = htons(PORT);
    ret = bind(sock_desc, (struct sockaddr*) address, *addrlen);
    if (ret < 0) { perror("bind failed."); return -1; }

    // set to be passive
    ret = listen(sock_desc, 20);
    if (ret < 0) { perror("listen failed."); return -1; }

    return sock_desc;
}


int main() {
    printf("-------------STARTING SERVER-------------\n");
    int timeout = MAX_TIMEOUT; // max 5 minute timeout
    int sock_desc, ret, new_socket;
    int *args;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    struct timeval tv;

    sock_desc = setup_initial_sock(&address, &addrlen);
    if (sock_desc < 0) { return -1; }

    while(1){
        // accept
        new_socket = accept(sock_desc, (struct sockaddr *) &address, (socklen_t *) &addrlen);
        if (new_socket < 0) { perror("accept failed"); continue; }

        // set timeout
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const void *) &tv, (socklen_t) sizeof(tv));

        client_service(new_socket);
    }


    // close listening socket
    shutdown(sock_desc, SHUT_RDWR);

    return 0;
}