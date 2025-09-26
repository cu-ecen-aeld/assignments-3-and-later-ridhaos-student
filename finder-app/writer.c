#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char const *argv[])
{
    // Initialize syslog connection
    openlog("myapp", LOG_PID | LOG_CONS, LOG_USER);
    if (argc != 3)
    {
        // Check number of parameter
        printf("Usage: %s <file_path> <string_to_writer>\n", argv[0]);
        syslog(LOG_ERR, "Error: Usage wrong of application");
        closelog();
        return 1;
    }
    
    // Open file and if not exist created.
    int fd = open(argv[1], O_CREAT | O_WRONLY, 0664);
    if (fd == -1)
    {
        printf("Error : %s\n", strerror(errno));
        syslog(LOG_ERR,     "Error: %s\n", strerror(errno)); 
        closelog();
        return 1;
    }

    // Wrtie string to file and catch error if exist.
    __ssize_t nr = write(fd, argv[2], strlen(argv[2]));
    if (nr == -1)
    {
        printf("Error : %s\n", strerror(errno));
        syslog(LOG_ERR,     "Error: %s\n", strerror(errno)); 
        closelog();
        return 1;
    }
    
    printf("Writing <%s> to <%s>\n" ,argv[2] ,argv[1]);
    syslog(LOG_DEBUG, "Writing <%s> to <%s>\n" ,argv[2] ,argv[1]);
   
    // Close syslog connection
    closelog();
    int cf = close(fd);

    if (cf == -1)
    {
        printf("Error : %s\n", strerror(errno));
        syslog(LOG_ERR,     "Error: %s\n", strerror(errno)); 
        closelog();
        return 1;
    }

    return 0;
}
