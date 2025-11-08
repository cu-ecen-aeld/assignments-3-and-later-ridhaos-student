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

#define PORT "9000"
#define BACKLOG 1
#define FILE_NAME "/var/tmp/aesdsocketdata"

volatile bool stop_program = false;

void sigin_handler(int s)
{
    if(s == SIGINT || s == SIGTERM){
        printf("CTRL+C signal capture\n");
        syslog(LOG_INFO, "Caught signal, exiting");
        stop_program = true;
    }
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int child_handler_recv(int sock_fd, int buffer_size)
{
    char buffer[buffer_size];
    int total_received = 0;
    char ch;
    ssize_t bytes_received;

    FILE * file = fopen(FILE_NAME, "a");
    if(file == NULL){
        perror("Open File:");
        return -1;
    }


    while (total_received < buffer_size - 1) {
        // Receive one character at a time
        bytes_received = recv(sock_fd, &ch, 1, 0);
        
        if (bytes_received <= 0) {
            // Error or connection closed
            return bytes_received;
        }
        
        // Store the character
        buffer[total_received++] = ch;
        
        // Check for newline character Client finish
        if (ch == '\n') {
            break;
        }
    }

    buffer[total_received++] = '\0'; // Null-terminate the string
    fwrite(buffer, 1, total_received-1, file);
    fclose(file);

    return total_received-1;

}

void child_handler_send(int sock_fd)
{
    FILE * file = fopen(FILE_NAME, "r");
    char ch;
    printf("Read file\n");
    while ((ch = fgetc(file)) != EOF) {
        send(sock_fd, &ch, 1, 0);
    }
    fclose(file);
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemon_mode = 1;
        }
    }
    

    struct addrinfo hints, *res, *p;
    struct sockaddr_storage their_addr;
    int rv, sfd, new_fd, yes=1;
    char s[INET6_ADDRSTRLEN];
    socklen_t sin_size;

    struct sigaction sa;

    sa.sa_handler = sigin_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    

    if((rv = getaddrinfo(NULL, PORT, &hints, &res)) != 0){
        syslog(LOG_ERR, "GetAddrInfo: %s", gai_strerror(rv));
        return 1;
    }

    for (p = res; p != NULL; p=p->ai_next)
    {
        if((sfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1 ){
            syslog(LOG_ERR, "Socket problem: %s", strerror(errno));
            perror("Socket: ");
            continue;
        }

        if(setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
            syslog(LOG_ERR, "setsockopt : %s", strerror(errno));
            perror("Setsockopt");
            continue;
        }

        if(bind(sfd, p->ai_addr, p->ai_addrlen) == -1){
            close(sfd);
            syslog(LOG_ERR, "bind Problem %s", strerror(errno));
            perror("bind");
            continue;
        }

        // Out from loop if one find
        break;
    }
    
    freeaddrinfo(res);

    if(p == NULL){
        syslog(LOG_ERR, "Server Failed to bind\n");
        exit(1);
    }

    if((listen(sfd, BACKLOG)) == -1){
        syslog(LOG_ERR, "listen Problem %s", strerror(errno));
        perror("bind");
        exit(1);
    }

    // DAEMON SETUP - MOVED AFTER SOCKET SETUP
    if(daemon_mode == 1){
        pid_t pid = fork();
        
        if (pid < 0) {
            perror("Fork failed");
            exit(EXIT_FAILURE);
        }
        
        if (pid > 0) {
            // Parent process exits
            exit(EXIT_SUCCESS);
        }
        
        // Child process continues
        
        // Create new session
        if (setsid() < 0) {
            perror("setsid failed");
            exit(EXIT_FAILURE);
        }
        
        // Second fork to ensure we're not a session leader
        pid = fork();
        if (pid < 0) {
            perror("Second fork failed");
            exit(EXIT_FAILURE);
        }
        
        if (pid > 0) {
            exit(EXIT_SUCCESS);
        }
        
        // Change working directory to root
        chdir("/");
        
        // Close standard file descriptors
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        // Redirect stdin, stdout, stderr to /dev/null
        open("/dev/null", O_RDONLY); // stdin
        open("/dev/null", O_WRONLY); // stdout  
        open("/dev/null", O_WRONLY); // stderr
        
        syslog(LOG_INFO, "Daemon started successfully");
    }

    int new_size = 131072; // 128 KB
    setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &new_size, sizeof(new_size));
    
    if (!daemon_mode) {
        printf("Server : Waiting for Connexion.....\n");
    }

    while(!stop_program)
    {
        sin_size = sizeof their_addr;
        new_fd = accept(sfd, (struct sockaddr *)&their_addr, &sin_size);

        if(new_fd == -1){
            if (errno == EINTR && stop_program) {
                break; // Signal received, break the loop
            }
            continue;
        }

        inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);

        if (!daemon_mode) {
            printf("Server Getting Cnx From %s\n", s);
        }
        syslog(LOG_INFO, "Accepted connection from %s", s);

        pid_t child_pid = fork();
        if(child_pid == 0){
            // Child process
            close(sfd); // Close listening socket in child
            
            child_handler_recv(new_fd, new_size);
            child_handler_send(new_fd);
            
            close(new_fd);
            
            if (!daemon_mode) {
                printf("Close Cnx From %s\n", s);
            }
            syslog(LOG_INFO, "Closed connection from %s", s);
            exit(0);
        } else if (child_pid > 0) {
            // Parent process
            close(new_fd); // Close connected socket in parent
        } else {
            // Fork failed
            syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
            close(new_fd);
        }
    }

    // Cleanup
    close(sfd);
    remove(FILE_NAME);
        
    closelog();
    return 0;
}