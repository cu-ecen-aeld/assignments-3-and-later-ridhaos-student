
#define _GNU_SOURCE // for pthread_tryjoin_np

#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#define PORT "9000"
#define BACKLOG 10
#define FILE_NAME "/var/tmp/aesdsocketdata"
#define RECV_BUF_SIZE 131072  // 128KB

volatile sig_atomic_t stop_program = 0;

typedef struct {
    int new_fd;
    char client_ip[INET6_ADDRSTRLEN];
    int daemon_mode;
} thread_variable_t;

typedef struct thread_node {
    pthread_t tid;
    struct thread_node *next;
    thread_variable_t *tdata;
} thread_node_t;

/* Global list head and mutex */
static thread_node_t *thread_list_head = NULL;
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Mutex for file access */
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Listening socket (global to be closed from signal handler) */
static int listen_fd = -1;

/* Signal handler sets stop flag and closes listening socket to wake accept() */
void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        stop_program = 1;
        if (listen_fd != -1) {
            /* Closing the listening socket will interrupt accept() */
            close(listen_fd);
            listen_fd = -1;
        }
    }
}

/* Helper to obtain sockaddr address (IPv4 or IPv6) */
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/* Append a packet (buffer containing up-to-and-including '\n') to file
 * but strip a preceding '\r' if present before '\n'.
 * Returns 0 on success, -1 on error.
 */
static int append_packet_to_file(const char *buf, size_t len)
{
    if (len == 0) return 0;

    /* Determine actual write length: strip preceding '\r' if present */
    size_t write_len = len;
    if (write_len >= 2 && buf[write_len - 2] == '\r' && buf[write_len - 1] == '\n') {
        /* replace CRLF with LF only -> write_len decreases by 1 */
        write_len = write_len - 1;
    }

    /* Acquire file mutex while appending */
    if (pthread_mutex_lock(&file_mutex) != 0) {
        syslog(LOG_ERR, "pthread_mutex_lock file_mutex failed");
        return -1;
    }

    int fd = open(FILE_NAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open append file failed: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    ssize_t w = write(fd, buf, write_len);
    if (w == -1 || (size_t)w != write_len) {
        syslog(LOG_ERR, "write to file failed: %s", strerror(errno));
        close(fd);
        pthread_mutex_unlock(&file_mutex);
        return -1;
    }

    close(fd);
    pthread_mutex_unlock(&file_mutex);
    return 0;
}

/* Read full file into heap buffer. On success returns pointer and sets size_out.
 * Caller must free() returned pointer. If file doesn't exist, returns NULL and size_out=0.
 */
static char *read_entire_file(size_t *size_out)
{
    *size_out = 0;
    struct stat st;
    if (stat(FILE_NAME, &st) == -1) {
        if (errno == ENOENT) {
            return NULL; // file doesn't exist
        }
        syslog(LOG_ERR, "stat failed: %s", strerror(errno));
        return NULL;
    }
    size_t fsize = (size_t)st.st_size;
    if (fsize == 0) {
        return NULL;
    }

    char *buf = malloc(fsize);
    if (!buf) {
        syslog(LOG_ERR, "malloc failed for read_entire_file");
        return NULL;
    }

    /* Ensure we read a consistent view: lock file mutex */
    if (pthread_mutex_lock(&file_mutex) != 0) {
        syslog(LOG_ERR, "pthread_mutex_lock file_mutex failed");
        free(buf);
        return NULL;
    }

    int fd = open(FILE_NAME, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open for read failed: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        free(buf);
        return NULL;
    }

    ssize_t r = read(fd, buf, fsize);
    if (r == -1 || (size_t)r != fsize) {
        syslog(LOG_ERR, "read file failed: %s", strerror(errno));
        close(fd);
        pthread_mutex_unlock(&file_mutex);
        free(buf);
        return NULL;
    }

    close(fd);
    pthread_mutex_unlock(&file_mutex);

    *size_out = fsize;
    return buf;
}

/* TimeStamp thread routine */
void *timestamp_thread(void *arg)
{
    (void)arg;
    while (!stop_program)
    {
        sleep(10);

        if(stop_program) break;

        time_t t = time(NULL);
        struct tm tm_info;
        localtime_r(&t, &tm_info);

        char timebuf[128];

        strftime(timebuf, sizeof(timebuf),
                 "%a, %d %b %Y %H:%M:%S %z",
                 &tm_info);

        char outbuf[256];
        int len = snprintf(outbuf, sizeof(outbuf),
                           "timestamp:%s\n", timebuf);

        if (len <= 0 || len >= (int)sizeof(outbuf)) {
            syslog(LOG_ERR, "timestamp snprintf error");
            continue;
        }

        pthread_mutex_lock(&file_mutex);

        int fd = open(FILE_NAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
            syslog(LOG_ERR, "open timestamp file: %s", strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            continue;
        }

        write(fd, outbuf, len);
        close(fd);

        pthread_mutex_unlock(&file_mutex);
    }
    pthread_exit(NULL);
}

/* Worker thread routine */
static void *connection_handler(void *arg)
{
    thread_variable_t *tdata = (thread_variable_t*)arg;
    int client_fd = tdata->new_fd;

    /* Receive until newline '\n' is found (packet completes). Read in chunks. */
    char *packet_buf = NULL;
    size_t packet_cap = 1024;
    size_t packet_len = 0;
    packet_buf = malloc(packet_cap);
    if (!packet_buf) {
        syslog(LOG_ERR, "malloc packet_buf failed");
        close(client_fd);
        free(tdata);
        pthread_exit(NULL);
    }

    bool packet_complete = false;
    while (!packet_complete && !stop_program) {
        char chunk[512];
        ssize_t recvd = recv(client_fd, chunk, sizeof(chunk), 0);
        if (recvd == 0) {
            /* Connection closed by client before newline */
            break;
        } else if (recvd < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "recv error from %s: %s", tdata->client_ip, strerror(errno));
            break;
        } else {
            /* append chunk to packet buffer */
            if (packet_len + (size_t)recvd > packet_cap) {
                size_t newcap = packet_cap * 2;
                while (newcap < packet_len + (size_t)recvd) newcap *= 2;
                char *tmp = realloc(packet_buf, newcap);
                if (!tmp) {
                    syslog(LOG_ERR, "realloc failed");
                    break;
                }
                packet_buf = tmp;
                packet_cap = newcap;
            }
            memcpy(packet_buf + packet_len, chunk, recvd);
            packet_len += recvd;

            /* check if newline present */
            for (size_t i = packet_len - recvd; i < packet_len; ++i) {
                if (packet_buf[i] == '\n') {
                    packet_complete = true;
                    /* truncate after newline (keep newline) */
                    packet_len = i + 1;
                    break;
                }
            }
            /* If packet length grows beyond RECV_BUF_SIZE, refuse further growth */
            if (packet_len >= (size_t)RECV_BUF_SIZE) {
                /* drop the rest of the packet */
                packet_len = RECV_BUF_SIZE - 1;
                packet_buf[packet_len] = '\n';
                packet_complete = true;
                break;
            }
        }
    }

    /* If we received at least 1 byte and found newline, append to file */
    if (packet_len > 0) {
        /* ensure packet_buf is not null-terminated in file; we write exact bytes */
        if (append_packet_to_file(packet_buf, packet_len) != 0) {
            syslog(LOG_ERR, "Failed to append packet from %s", tdata->client_ip);
        } else {
            /* success */
        }
    }

    free(packet_buf);

    /* Read full file (while holding lock inside read_entire_file) */
    size_t file_size = 0;
    char *file_contents = read_entire_file(&file_size);
    if (file_contents && file_size > 0) {
        /* send file in loop until all bytes sent */
        size_t sent = 0;
        while (sent < file_size && !stop_program) {
            ssize_t s = send(client_fd, file_contents + sent, file_size - sent, 0);
            if (s < 0) {
                if (errno == EINTR) continue;
                syslog(LOG_ERR, "send error to %s: %s", tdata->client_ip, strerror(errno));
                break;
            }
            sent += (size_t)s;
        }
        free(file_contents);
    } else {
        /* If file empty or missing, send nothing (valid per spec) */
    }

    /* Log and cleanup */
    syslog(LOG_INFO, "Closed connection from %s", tdata->client_ip);
    if (!tdata->daemon_mode) {
        printf("Close Cnx From %s\n", tdata->client_ip);
    }

    close(client_fd);

    /* Note: DO NOT free tdata here if main thread expects to free after join.
     * But we allocated tdata per connection and main thread will free it after join.
     * To avoid double-free, we won't free tdata here; main thread will free it when
     * performing the join/cleanup.
     */
    pthread_exit(NULL);
    return NULL;
}

/* Add a created thread and its data to the list (main thread does this after pthread_create) */
static void add_thread_node(pthread_t tid, thread_variable_t *tdata)
{
    
    thread_node_t *node = malloc(sizeof(thread_node_t));
    if (!node) {
        syslog(LOG_ERR, "malloc thread node failed");
        return;
    }
    node->tid = tid;
    node->tdata = tdata;
    node->next = NULL;

    pthread_mutex_lock(&thread_list_mutex);
    node->next = thread_list_head;
    thread_list_head = node;
    pthread_mutex_unlock(&thread_list_mutex);
}

/* Reap finished threads using pthread_tryjoin_np() (non-blocking join).
 * Frees thread node and its thread_variable_t after successful join.
 */
static void reap_finished_threads(void)
{
    pthread_mutex_lock(&thread_list_mutex);
    thread_node_t **pp = &thread_list_head;
    while (*pp) {
        thread_node_t *node = *pp;
        int jrv = pthread_tryjoin_np(node->tid, NULL);
        if (jrv == 0) {
            /* thread finished and joined */
            *pp = node->next;
            /* free thread data */
            if (node->tdata) free(node->tdata);
            free(node);
            /* pp remains the same (already updated) */
        } else if (jrv == EBUSY) {
            /* thread still running, move to next */
            pp = &node->next;
        } else {
            /* Some error occurred (unlikely), log and remove to avoid leak */
            syslog(LOG_ERR, "pthread_tryjoin_np returned %d for tid (removing): %s", jrv, strerror(jrv));
            *pp = node->next;
            if (node->tdata) free(node->tdata);
            free(node);
        }
    }
    pthread_mutex_unlock(&thread_list_mutex);
}

int main(int argc, char *argv[])
{
    struct addrinfo hints, *res, *p;
    struct sockaddr_storage their_addr;
    socklen_t sin_size;
    int rv;
    int yes = 1;
    struct sigaction sa;
    int daemon_mode = 0;
    pthread_t timestamp_tid;
    int timestamp_started = 0;

    openlog(NULL, LOG_PID, LOG_USER);

    /* parse args */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0) daemon_mode = 1;
    }

    /* Setup signal handling, no SA_RESTART so accept() returns EINTR */
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    /* Prepare address info */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rv));
        exit(EXIT_FAILURE);
    }

    /* Create and bind socket (first usable addr) */
    for (p = res; p != NULL; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd == -1) {
            continue;
        }
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(listen_fd);
            listen_fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);

    if (p == NULL) {
        syslog(LOG_ERR, "Failed to bind");
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    /* Daemonize if requested (after socket setup) */
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "fork failed");
            exit(EXIT_FAILURE);
        }
        if (pid > 0) {
            /* parent exits */
            exit(EXIT_SUCCESS);
        }
        /* child continues */
        if (setsid() < 0) exit(EXIT_FAILURE);
        pid = fork();
        if (pid < 0) exit(EXIT_FAILURE);
        if (pid > 0) exit(EXIT_SUCCESS);
        chdir("/");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        open("/dev/null", O_RDONLY);
        open("/dev/null", O_WRONLY);
        open("/dev/null", O_WRONLY);
        syslog(LOG_INFO, "Daemon started successfully");
    } else {
        printf("Server : Waiting for Connection on port %s ...\n", PORT);
    }

    pthread_create(&timestamp_tid, NULL, timestamp_thread, NULL);

    /* Adjust receive buffer (optional) */
    int rcvbuf = RECV_BUF_SIZE;
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    remove(FILE_NAME);

    /* Accept loop */
    while (!stop_program) {
        sin_size = sizeof their_addr;
        int new_fd = accept(listen_fd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            if (errno == EINTR && stop_program) {
                /* Interrupted by signal and stop requested */
                break;
            }
            /* other errors just continue */
            continue;
        }

        /* get client ip */
        char client_ip[INET6_ADDRSTRLEN];
        inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr),
                  client_ip, sizeof client_ip);

        if (!daemon_mode) {
            printf("Server Getting Cnx From %s\n", client_ip);
        }
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        /* Allocate per-connection thread data */
        thread_variable_t *tdata = malloc(sizeof(thread_variable_t));
        if (!tdata) {
            syslog(LOG_ERR, "malloc failed for thread data");
            close(new_fd);
            continue;
        }
        tdata->new_fd = new_fd;
        tdata->daemon_mode = daemon_mode;
        strncpy(tdata->client_ip, client_ip, sizeof(tdata->client_ip));
        tdata->client_ip[sizeof(tdata->client_ip)-1] = '\0';

        /* Create a joinable worker thread */
        pthread_t tid;
        int cr = pthread_create(&tid, NULL, connection_handler, (void*)tdata);
        if (cr != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(cr));
            close(new_fd);
            free(tdata);
            continue;
        }
        /* Add to linked list for future join/cleanup */
        add_thread_node(tid, tdata);

        /* Reap finished threads (non-blocking) to avoid leak */
        reap_finished_threads();
        
        if (!timestamp_started) {
            pthread_create(&timestamp_tid, NULL, timestamp_thread, NULL);
            timestamp_started = 1;
        }
    }

    /* Primary shutdown: stop accepting, join remaining threads */
    syslog(LOG_INFO, "Server shutting down, cleaning up threads");
    /* Close listen socket if still open */
    if (listen_fd != -1) {
        close(listen_fd);
        listen_fd = -1;
    }

    /* Join remaining worker threads */
    pthread_mutex_lock(&thread_list_mutex);
    thread_node_t *node = thread_list_head;
    thread_list_head = NULL;
    pthread_mutex_unlock(&thread_list_mutex);

    while (node) {
        thread_node_t *next = node->next;
        /* Wait for thread to finish (blocking join) */
        pthread_join(node->tid, NULL);
        if (node->tdata) free(node->tdata);
        free(node);
        node = next;
    }

    /* Remove data file */
    if (timestamp_started) {
        pthread_join(timestamp_tid, NULL);
    }

    remove(FILE_NAME);

    /* Destroy mutexes and close syslog */
    pthread_mutex_destroy(&thread_list_mutex);
    pthread_mutex_destroy(&file_mutex);
    closelog();

    return 0;
}
