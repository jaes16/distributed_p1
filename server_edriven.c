#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

extern int errno;

#define PORT 8080
#define BUF_SIZE 4096 // Max line length but also max packet length
#define MAX_FILE_SIZE 999999999
#define MAX_TIMEOUT 100000
#define MAX_TASKS 1000
#define MAX_CONNECTIONS 1000

#define EFILESIZE 1000

#define RESPONSE400 "HTTP/1.1 400 Bad Request\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 224\r\n\r\n"
#define RESPONSE403 "HTTP/1.1 403 Forbidden\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 211\r\n\r\n"
#define RESPONSE404 "HTTP/1.1 404 File Not Found\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 201\r\n\r\n"
#define RESPONSE408 "HTTP/1.1 408 Connection Timed Out\r\n\r\n"
//#define RESPONSE408 "HTTP/1.1 408 Connection Timed Out\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: 0\r\n"



struct task {
    int socket_fd;
    int len_sent;
    int len_total;
    int file_fd;
    int file_type; // 0 for html, 1 for jpg, 2 for gif
    int http_ver;
};


struct task task_queue[MAX_TASKS];
int queue_start;
int queue_end;
int num_tasks;

struct pollfd connections[MAX_CONNECTIONS];
int num_connections;

char task_buffer[BUF_SIZE];



int send_on_socket(int socket, void *buffer, size_t buf_len) {
    int sent = 0;
    while (sent < buf_len){
        sent += send(socket, buffer+sent, buf_len-sent, 0);
    }
    // printf("%s\n", (char *) buffer);
    return sent;
}

int custom_open(char *file_path, int *len) {
    int fd;

    fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES) { return -EACCES; }
        else if (errno == ENOENT) {  return -ENOENT; }
    }

    // find length of file so we can tell the content-length
    *len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (*len > MAX_FILE_SIZE) { close(fd); return -EFILESIZE; }

    return fd;
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


int get_file_type(char *file_path) {
    if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 5, ".html", 5) == 0) {
        return 0;
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".jpg", 4) == 0) {
        return 1;
    }
    else if (strncmp(file_path + strnlen(file_path, BUF_SIZE) - 4, ".gif", 4) == 0) {
        return 2;
    }
    return -1;
}

void get_header(int file_type, char* header_str, off_t file_len) {
    if (file_type == 0) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
    else if (file_type == 1) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
    else if (file_type == 2) {
        snprintf(header_str, BUF_SIZE, "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nAccept-Ranges: bytes\r\nContent-Type: image/gif\r\nContent-Length: %d\r\n\r\n", (u_int32_t) file_len);
    }
}


int interpret_get(char *buffer, char *get_path) {
    // interpret
    if (strncmp(buffer, "GET ", 4) == 0){
        memset(get_path, 0, BUF_SIZE);
        memcpy(get_path, "files", 5);
        int i;
        for (i = 4; buffer[i] != ' '; i++){
            get_path[i+1] = buffer[i];
        }
        
        if (get_path[5] == '/' && get_path[6] == 0) { memcpy(get_path+5, "/index.html", 11); }
        printf("Requested Dest: %s\n\n", get_path);
        
        for ( ; buffer[i] != '\n' && i < BUF_SIZE; i++) { }
        if (strncmp(buffer+i-10, " HTTP/1.0\n", 10)){ return 0; }
        else if (strncmp(buffer+i-10, " HTTP/1.1\n", 10)){ return 1; }
        else { return 400; }
    }
    else {
        return 400;
    }
}


void remove_connection(int socket) {
    // remove client from connection list
    printf("removing client %d!\n", socket);
    for (int i = 1; i < num_connections; i++) {
        if (connections[i].fd == socket) {
            memmove(connections + (sizeof(struct pollfd) * i),
                    connections + (sizeof(struct pollfd) * (i+1)),
                    (sizeof(struct pollfd) * (num_connections - i - 1)));
            num_connections--;
            close(socket);
            return;
        }
    }
}


int service_client(int socket) {
    printf("servicing client %d\n", socket);
    char buffer[BUF_SIZE];
    char get_path[BUF_SIZE];
    int ret, file_len, file_fd, conn_type;
    struct timeval tv;

    // read from new socket
    memset(buffer, 0, BUF_SIZE);
    ret = recv(socket, buffer, BUF_SIZE, 0);
    if (ret < 0) { // timeout!
        if (errno == ETIMEDOUT || errno == EAGAIN) {
            send_error(socket, 408);
            return 408;
        }
        perror("recv failed");
        return -1;
    }
    if (ret == 0) { // client left
        remove_connection(socket);
        return -1; 
    } 

    // reset timeout
    tv.tv_sec = MAX_TIMEOUT;
    if (tv.tv_sec <= 0) { tv.tv_sec = 10; }
    tv.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const void *) &tv, (socklen_t) sizeof(tv));

    printf("Read in request:\n\'\'\'\n%s\n\'\'\'\nEnd of Request.\n\n", buffer);

    // interpret what file they want
    conn_type = interpret_get(buffer, get_path);
    if (conn_type == 400) { send_error(socket, 400); return 400; }
    
    // get file descriptor and lenght
    file_fd = custom_open(get_path, &file_len);
    if (file_fd == -EACCES) { send_error(socket, 403); return -1; }
    if (file_fd == -ENOENT) { send_error(socket, 404); return -1; }
    if (file_fd == -EFILESIZE) { return -1; } // TO CHANGE


    // file is valid, add task
    if (num_tasks < MAX_TASKS) {
        task_queue[queue_end].socket_fd = socket;
        task_queue[queue_end].len_sent = 0;
        task_queue[queue_end].len_total = file_len;
        task_queue[queue_end].file_fd = file_fd;
        task_queue[queue_end].file_type = get_file_type(get_path);
        task_queue[queue_end].http_ver = conn_type;
        printf("Added Task:\nsocket%d\nlen_total%d\nfile%d\nfile_type%d\n",
            task_queue[queue_end].socket_fd,
            task_queue[queue_end].len_total,
            task_queue[queue_end].file_fd,
            task_queue[queue_end].file_type);
        queue_end++;
        if (queue_end >= MAX_TASKS) { queue_end = 0; }
        num_tasks++;
    } else {
        printf("too many tasks\n");
    }

    return 0;
}


int fulfill_task() {
    int header_len = 0;
    int write_len;
    struct task cur_task = task_queue[queue_start];

    memset(task_buffer, 0, BUF_SIZE);
    // fulfill task first in line
    if (cur_task.len_sent == 0) { // first time sending, send header too
        get_header(cur_task.file_type, task_buffer, cur_task.len_total);
        header_len = strnlen(task_buffer, BUF_SIZE);
    }

    // space left (whether after header or in total)
    write_len = read(cur_task.file_fd, task_buffer + header_len, BUF_SIZE - header_len);

    // write out
    printf("Sending packet:\n %s\n To socket %d\n", task_buffer, cur_task.socket_fd);
    send_on_socket(cur_task.socket_fd, task_buffer, write_len+header_len); // TODO: POSSIBLE ERRORS


    cur_task.len_sent += write_len;
    task_queue[queue_start].len_sent += write_len;
    if (cur_task.len_sent == cur_task.len_total) { // done sending the file
        // get this task outta here
        memset(&task_queue[queue_start], 0, sizeof(struct task));
        close(cur_task.file_fd);
        num_tasks--;

        if (cur_task.http_ver == 0) { // HTTP 1.0 get this socket outta here
            remove_connection(cur_task.socket_fd);
        }
    } else {
        // move this task to the back
        memmove(&task_queue[queue_end], &task_queue[queue_start], sizeof(struct task));
        memset(&task_queue[queue_start], 0, sizeof(struct task));
        queue_end++;
        if (queue_end >= MAX_TASKS) { queue_end = 0; }
    }
    
    // update queue start
    queue_start++;
    if (queue_start >= MAX_TASKS) { queue_start = 0; }
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

    queue_start = 0;
    queue_end = 0;
    num_tasks = 0;

    connections[0].fd = sock_desc;
    connections[0].events = POLLIN;
    num_connections = 1;

    while(1){
        
        if (num_tasks == 0) { ret = poll(connections, num_connections, -1); }
        else { ret = poll(connections, num_connections, 1); }

        if (ret == -1) { perror("poll failed"); }
        else if (ret) {
            printf("got here, ret with > 0\n");

            // check if we have a new connection
            if ((connections[0].revents & POLLIN) > 0) {
                // we have a new connection
                new_socket = accept(sock_desc, (struct sockaddr *) &address, (socklen_t *) &addrlen);
                if (new_socket < 0) { perror("accept failed"); continue; }
                printf("accepted new connection at %d\n", new_socket);
                // set timeout
                tv.tv_sec = MAX_TIMEOUT; // TO CHANGE!
                tv.tv_usec = 0;
                setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const void *) &tv, (socklen_t) sizeof(tv));
                
                // add to list of connections
                connections[num_connections].fd = new_socket;
                connections[num_connections].events = POLLIN;
                num_connections++;
            }
            // check if we have any requests
            for (int i = 1; i < num_connections; i++){
                if ((connections[i].revents & POLLIN) > 0){
                    // found a connection that has something for us
                    service_client(connections[i].fd);
                    break;
                }
            }
        }

        // fulfill a task
        if (num_tasks > 0) {
            fulfill_task();
        }
        printf("\nNUM_TASKS %d, NUM_CONNECTIONS %d\n", num_tasks, num_connections);
        printf("QUEUE_START %d, QUEUE_END %d\n\n", queue_start, queue_end);
    }


    // close listening socket
    shutdown(sock_desc, SHUT_RDWR);

    return 0;
}