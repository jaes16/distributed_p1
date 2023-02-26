#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

extern int errno;

#define PORT 8008
#define BUF_SIZE 4096 // Max line length but also max packet length
#define MAX_FILE_SIZE 999999999
#define MAX_TIMEOUT 100000

#define RESPONSE400 "HTTP/1.1 400 Bad Request\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 224\r\n\r\n"
#define RESPONSE403 "HTTP/1.1 403 Forbidden\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 211\r\n\r\n"
#define RESPONSE404 "HTTP/1.1 404 File Not Found\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 201\r\n\r\n"
#define RESPONSE408 "HTTP/1.1 408 Connection Timed Out\r\n\r\n"

#define MAX_THREADS 1000

// In this block, must lock to access these
int thread_sockets[MAX_THREADS];
int thread_count;
pthread_mutex_t thread_mutex;

// just in case send doesn't send everything
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
    char *buffer = (char *) malloc(BUF_SIZE);

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

    if (buffer) free(buffer);

    close(fd);
}

void get_header(char *file_path, char* header_str, off_t file_len) {
    if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 5, ".html", 5) == 0) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".txt", 4) == 0) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
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
    char *header_str = (char *) malloc(BUF_SIZE);
    char *send_str = (char *) malloc(BUF_SIZE);
    memset(header_str, 0, BUF_SIZE);

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

    if (header_str) free(header_str);
    if (send_str) free(send_str);
    close(fd);
    return 0;
}

// returns 0 for http/1.0, 1 for http/1.1, and 400 for 400error
int interpret_get(char *buffer, char *get_path) {
    // interpret
    if (strncmp(buffer, "GET ", 4) == 0){
        memset(get_path, 0, BUF_SIZE);
        memcpy(get_path, "files", 5);
        int i;
        for (i = 4; buffer[i] != ' ' && i < BUF_SIZE; i++){
            get_path[i+1] = buffer[i];
        }
        
        if (get_path[5] == '/' && get_path[6] == 0) { memcpy(get_path+5, "/index.html", 11); }
        printf("Requested Dest: %s\n\n", get_path);
        
        for ( ; buffer[i] != '\n' && i < BUF_SIZE; i++) { }
        if (strncmp(buffer+i-10, " HTTP/1.0\n", 10)){
            return 0;
        }
        else if (strncmp(buffer+i-10, " HTTP/1.1\n", 10)){
            return 1;
        }
        else {
            return 400;
        }
    }
    else {
        return 400;
    }
}


void *client_service(void *args) {
    // listen
    char *buffer = (char *) malloc(BUF_SIZE);
    char *get_path = (char *) malloc(BUF_SIZE);
    int ret, conn_type;
    int new_socket = *((int *) args);

    while(1){
        // accepted new connection!
        // read from new socket
        memset(buffer, 0, BUF_SIZE);
        ret = recv(new_socket, buffer, BUF_SIZE, 0);
        if (ret < 0) {
            if (errno == ETIMEDOUT || errno == EAGAIN) {
                send_on_socket(new_socket, RESPONSE408, sizeof(RESPONSE408));
                break;
            }
            perror("recv failed"); continue; 
        }
        if (ret == 0) { break; }
        printf("Read in request:\n\'\'\'\n%s\n\'\'\'\nEnd of Request.\n\n", buffer);
        
        // interpret
        conn_type = interpret_get(buffer, get_path);
        if (conn_type == 400) { send_error(new_socket, 400); continue; }

        // read file
        ret = read_file_and_send(get_path, new_socket);
        if (ret < 0) printf("ret %d", ret);
        if (ret == EACCES) { send_error(new_socket, 403); }
        if (ret == ENOENT) { send_error(new_socket, 404); }

        // if http1.0, break out
        if (conn_type == 0) { break; }

    }
    // shutdown(new_socket, 0);
    close(new_socket);
    if (args) free(args);
    if (buffer) free(buffer);
    if (get_path) free(get_path);

    pthread_mutex_lock(&thread_mutex);
    thread_count--;
    for (int i = 0; i < MAX_THREADS; i++){
        if (thread_sockets[i] == new_socket){
            thread_sockets[i] = -1;
        }
    }
    pthread_mutex_unlock(&thread_mutex);

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
    pthread_t thread_id;
    struct timeval tv;

    int addrlen = sizeof(address);

    sock_desc = setup_initial_sock(&address, (socklen_t *) &addrlen);
    if (sock_desc < 0) { return -1; }

    pthread_mutex_lock(&thread_mutex);
    thread_count = 0;
    for (int i = 0; i < MAX_THREADS; i++) { thread_sockets[i] = -1; }
    pthread_mutex_unlock(&thread_mutex);

    while(1){
        pthread_mutex_lock(&thread_mutex);
        if (thread_count < MAX_THREADS){
            pthread_mutex_unlock(&thread_mutex); // because the other threads will only lower the count

            // accept
            new_socket = accept(sock_desc, (struct sockaddr *) &address, (socklen_t *) &addrlen);
            if (new_socket < 0) { perror("accept failed"); continue; }

            pthread_mutex_lock(&thread_mutex);
            tv.tv_sec = timeout - (thread_count * 10);
            if (tv.tv_sec <= 0) { tv.tv_sec = 10; }
            tv.tv_usec = 0;
            setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const void *) &tv, (socklen_t) sizeof(tv));
            printf("thread_count: %d, timeout: %ld\n", thread_count, tv.tv_sec);
            thread_count++;

            // update new timeout on all threads
            int socket_added = 0;
            for (int i = 0; i < MAX_THREADS; i++){
                if (thread_sockets[i] != -1) {
                    setsockopt(thread_sockets[i], SOL_SOCKET, SO_RCVTIMEO, (const void *) &tv, (socklen_t) sizeof(tv));
                }
                else if(socket_added == 0){ // empty spot ([i]==-1), add new socket
                    thread_sockets[i] = new_socket;
                    socket_added = 1;
                }
            }
            pthread_mutex_unlock(&thread_mutex);
            
            args = malloc(sizeof(int));
            *args = new_socket;
            pthread_create(&thread_id, NULL, client_service, (void *) args);
        } else {
            pthread_mutex_unlock(&thread_mutex);
            printf("Max number of threads reached!\n");
        }
    }

    // close listening socket
    shutdown(sock_desc, SHUT_RDWR);

    return 0;
}