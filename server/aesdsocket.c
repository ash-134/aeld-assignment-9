#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include "../aesd-char-driver/aesd_ioctl.h"

#define PORT "9000"
#define BUFFER_SIZE 1024
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif
#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"
#else
#define DATA_FILE "/var/tmp/aesdsocketdata"
#endif
#define IOCSEEKTO_PREFIX "AESDCHAR_IOCSEEKTO:"

volatile sig_atomic_t exit_requested = 0;
int server_fd = -1;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_node {
    pthread_t thread_id;
    int client_fd;
    bool complete;
    struct thread_node *next;
};

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        exit_requested = 1;
        if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
    }
}

void *client_handler(void *arg) {
    struct thread_node *node = (struct thread_node *)arg;
    int client_fd = node->client_fd;
    char *recv_buf = malloc(BUFFER_SIZE);
    if (!recv_buf) { close(client_fd); node->complete = true; return NULL; }
    size_t total_len = 0, capacity = BUFFER_SIZE;
    ssize_t bytes_received;

    while ((bytes_received = recv(client_fd, recv_buf + total_len, capacity - total_len - 1, 0)) > 0) {
        total_len += bytes_received;
        recv_buf[total_len] = '\0';
        char *newline_ptr;
        while ((newline_ptr = strchr(recv_buf, '\n')) != NULL) {
            size_t packet_len = newline_ptr - recv_buf + 1;
            pthread_mutex_lock(&file_mutex);
            int fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
            if (fd != -1) {
                unsigned int x = 0, y = 0;
                if (strncmp(recv_buf, IOCSEEKTO_PREFIX, strlen(IOCSEEKTO_PREFIX)) == 0 &&
                    sscanf(recv_buf + strlen(IOCSEEKTO_PREFIX), "%u,%u", &x, &y) == 2) {
                    struct aesd_seekto seekto = { .write_cmd = x, .write_cmd_offset = y };
                    ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto);
                } else {
                    write(fd, recv_buf, packet_len);
                    lseek(fd, 0, SEEK_SET);
                }
                char send_buf[BUFFER_SIZE];
                ssize_t bytes_read;
                while ((bytes_read = read(fd, send_buf, sizeof(send_buf))) > 0) {
                    send(client_fd, send_buf, bytes_read, 0);
                }
                close(fd);
            }
            pthread_mutex_unlock(&file_mutex);
            size_t remaining = total_len - packet_len;
            memmove(recv_buf, newline_ptr + 1, remaining);
            total_len = remaining;
            recv_buf[total_len] = '\0';
        }
        if (total_len + BUFFER_SIZE > capacity) {
            capacity *= 2;
            char *new_buf = realloc(recv_buf, capacity);
            if (!new_buf) break;
            recv_buf = new_buf;
        }
    }
    free(recv_buf);
    close(client_fd);
    node->complete = true;
    return NULL;
}

int main(int argc, char *argv[]) {
    bool daemon_mode = (argc > 1 && strcmp(argv[1], "-d") == 0);
    openlog("aesdsocket", LOG_PID, LOG_USER);
    struct sigaction sa = { .sa_handler = signal_handler };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *res;
    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) return -1;
    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(server_fd, res->ai_addr, res->ai_addrlen) == -1) { close(server_fd); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);

    if (daemon_mode) {
        if (fork() > 0) exit(0);
        setsid(); chdir("/"); close(0); close(1); close(2);
    }
    listen(server_fd, 10);
    struct thread_node *head = NULL;

    while (!exit_requested) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd == -1) continue;
        struct thread_node *node = calloc(1, sizeof(struct thread_node));
        node->client_fd = cfd;
        node->next = head;
        head = node;
        pthread_create(&node->thread_id, NULL, client_handler, node);
    }
    struct thread_node *curr = head;
    while (curr) {
        pthread_join(curr->thread_id, NULL);
        struct thread_node *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
    close(server_fd);
    closelog();
    return 0;
}
