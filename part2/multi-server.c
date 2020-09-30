/*
 * multi-server.c
 */

#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <sys/stat.h>   /* for stat() */
#include <sys/wait.h>   /* for waitpid() */
#include <sys/shm.h>    /* for shmget() */
#include <semaphore.h>  /* for sem_open() */
#include <fcntl.h>      /* for O_CREAT */


#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096

#define SHM_SIZE 1000

#define SHM_MODE 0600

static void die(const char *message)
{
    perror(message);
    exit(1); 
}

/*
 * Create a listening socket bound to the given port.
 */
static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");
      
    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr));       /* Zero out structure */
    servAddr.sin_family = AF_INET;                /* Internet address family */
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    servAddr.sin_port = htons(port);              /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

/*
 * A wrapper around send() that does error checking and logging.
 * Returns -1 on failure.
 * 
 * This function assumes that buf is a null-terminated string, so
 * don't use this function to send binary data.
 */
ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != len) {
        perror("send() failed");
        return -1;
    }
    else 
        return res;
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}


/*
 * Send HTTP status line followed by a blank line.
 */
static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reasonPhrase = getReasonPhrase(statusCode);

    // print the status line into the buffer
    sprintf(buf, "HTTP/1.0 %d ", statusCode);
    strcat(buf, reasonPhrase);
    strcat(buf, "\r\n");

    // We don't send any HTTP header in this simple server.
    // We need to send a blank line to signal the end of headers.
    strcat(buf, "\r\n");

    // For non-200 status, format the status line as an HTML content
    // so that browers can display it.
    if (statusCode != 200) {
        char body[1000];
        sprintf(body, 
                "<html><body>\n"
                "<h1>%d %s</h1>\n"
                "</body></html>\n",
                statusCode, reasonPhrase);
        strcat(buf, body);
    }

    // send the buffer to the browser
    Send(clntSock, buf);
}

/*
 * Handle static file requests.
 * Returns the HTTP status code that was sent to the browser.
 */
static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock, sem_t *semptr, int *shmptr)
{
    printf("url: %s", requestURI);
    int statusCode;
    FILE *fp = NULL;

    // Compose the file path from webRoot and requestURI.
    // If requestURI ends with '/', append "index.html".
    
    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (file[strlen(file)-1] == '/') {
        strcat(file, "index.html");
    }

    // See if the requested file is a directory.
    // Our server does not support directory listing.
    // enable the statistics page

    if (strcmp(requestURI, "/statistics") == 0) {
        sem_wait(semptr);
        shmptr[1]++;
        sem_post(semptr);
        char stats[1000];
        sprintf(stats, 
                "<html><body>\n"
                "<h1>Server Statistics</h1>\n"
                "<h4>Requests: %d</h4>\n"
                "<h4>2xx: %d</h4>\n"
                "<h4>3xx: %d</h4>\n"
                "<h4>4xx: %d</h4>\n"
                "<h4>5xx: %d</h4>\n"
                "</body></html>\n", shmptr[0], shmptr[1], shmptr[2], shmptr[3], shmptr[4]);
        sendStatusLine(clntSock, 200);
        Send(clntSock, stats);
        goto func_end;
    }

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        statusCode = 403; // "Forbidden"
        sem_wait(semptr);
        shmptr[3]++;
        sem_post(semptr);
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sem_wait(semptr);
        shmptr[3]++;
        sem_post(semptr);
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
    sem_wait(semptr);
    shmptr[1]++;
    sem_post(semptr);
    sendStatusLine(clntSock, statusCode);

    // send the file 
    size_t n;
    char buf[DISK_IO_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) {
            // send() failed.
            // We log the failure, break out of the loop,
            // and let the server continue on with the next request.
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 both on EOF and on error.
    // Let's check if there was an error.
    if (ferror(fp))
        perror("fread failed");

func_end:

    // clean up
    free(file);
    if (fp)
        fclose(fp);

    return statusCode;
}



int main(int argc, char *argv[])
{
    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    if (argc != 3) {
        fprintf(stderr, "usage: %s <server_port> <web_root>\n", argv[0]);
        exit(1);
    }

    unsigned short servPort = atoi(argv[1]);
    const char *webRoot = argv[2];

    int servSock = createServerSocket(servPort);

    char line[1000];
    char requestLine[1000];
    int statusCode;
    struct sockaddr_in clntAddr;
    int shmid;
    sem_t *semptr;

    if ((shmid = shmget(0, SHM_SIZE, SHM_MODE)) < 0)
        die("shmget() failed");

    if ((semptr = sem_open("/lock", O_CREAT)) == SEM_FAILED)
        die("sem_open() failed");

    for (;;) {

        /*
         * wait for a client to connect
         */

        // initialize the in-out parameter
        unsigned int clntLen = sizeof(clntAddr); 
        int clntSock = accept(servSock, (struct sockaddr *)&clntAddr, &clntLen);
        if (clntSock < 0)
            die("accept() failed");

        /* 
         * create a child process to handle the connection
         */
        pid_t pid;
        if ((pid = fork()) < 0) {
            die("fork() failed");
        } else if (pid == 0) {
            // child
            close(servSock);

            // shared memory
            int *shmptr;
            if ((shmptr = shmat(shmid, 0, 0)) == (void*) -1)
                die("shmat() failed");
            
            // increase request stat
            sem_wait(semptr);
            shmptr[0]++;
            sem_post(semptr);

            FILE *clntFp = fdopen(clntSock, "r");
            if (clntFp == NULL)
                die("fdopen failed");

            /*
            * Let's parse the request line.
            */

            char *method      = "";
            char *requestURI  = "";
            char *httpVersion = "";

            if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
                // socket closed - there isn't much we can do
                statusCode = 400; // "Bad Request"
                sem_wait(semptr);
                shmptr[3]++;
                sem_post(semptr);
                goto loop_end;
            }

            char *token_separators = "\t \r\n"; // tab, space, new line
            method = strtok(requestLine, token_separators);
            requestURI = strtok(NULL, token_separators);
            httpVersion = strtok(NULL, token_separators);
            char *extraThingsOnRequestLine = strtok(NULL, token_separators);

            // check if we have 3 (and only 3) things in the request line
            if (!method || !requestURI || !httpVersion || 
                    extraThingsOnRequestLine) {
                statusCode = 501; // "Not Implemented"
                sem_wait(semptr);
                shmptr[4]++;
                sem_post(semptr);
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }

            // we only support GET method 
            if (strcmp(method, "GET") != 0) {
                statusCode = 501; // "Not Implemented"
                sem_wait(semptr);
                shmptr[4]++;
                sem_post(semptr);
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }

            // we only support HTTP/1.0 and HTTP/1.1
            if (strcmp(httpVersion, "HTTP/1.0") != 0 && 
                strcmp(httpVersion, "HTTP/1.1") != 0) {
                statusCode = 501; // "Not Implemented"
                sem_wait(semptr);
                shmptr[4]++;
                sem_post(semptr);
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }
            
            // requestURI must begin with "/"
            if (!requestURI || *requestURI != '/') {
                statusCode = 400; // "Bad Request"
                sem_wait(semptr);
                shmptr[3]++;
                sem_post(semptr);
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }

            // make sure that the requestURI does not contain "/../" and 
            // does not end with "/..", which would be a big security hole!
            int len = strlen(requestURI);
            if (len >= 3) {
                char *tail = requestURI + (len - 3);
                if (strcmp(tail, "/..") == 0 || 
                        strstr(requestURI, "/../") != NULL)
                {
                    statusCode = 400; // "Bad Request"
                    sem_wait(semptr);
                    shmptr[3]++;
                    sem_post(semptr);
                    sendStatusLine(clntSock, statusCode);
                    goto loop_end;
                }
            }

            /*
            * Now let's skip all headers.
            */

            while (1) {
                if (fgets(line, sizeof(line), clntFp) == NULL) {
                    // socket closed prematurely - there isn't much we can do
                    statusCode = 400; // "Bad Request"
                    sem_wait(semptr);
                    shmptr[3]++;
                    sem_post(semptr);
                    goto loop_end;
                }
                if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) {
                    // This marks the end of headers.  
                    // Break out of the while loop.
                    break;
                }
            }

            /*
            * At this point, we have a well-formed HTTP GET request.
            * Let's handle it.
            */

            statusCode = handleFileRequest(webRoot, requestURI, clntSock, semptr, shmptr);

loop_end:

            /*
            * Done with client request.
            * Log it, close the client socket, and go back to accepting
            * connection.
            */

            fprintf(stderr, "%s \"%s %s %s\" %d %s cpid: %d\n",
                    inet_ntoa(clntAddr.sin_addr),
                    method,
                    requestURI,
                    httpVersion,
                    statusCode,
                    getReasonPhrase(statusCode),
                    getpid());

            // close the client socket 
            fclose(clntFp);
            close(clntSock);
            shmdt(shmptr);
            exit(1);
        } else {
            close(clntSock);
            pid_t cpid;
            while ((cpid = waitpid(-1, NULL, WNOHANG)) >= 0);
        }

    } // for (;;)
    sem_close(semptr);
    sem_unlink("/lock");
    return 0;
}

