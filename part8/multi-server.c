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
#include <pthread.h>    /* for pthread_create() */

#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096

#define N_THREADS 16

static pthread_t thread_pool[N_THREADS];

static void die(const char *message)
{
    perror(message);
    exit(1); 
}

/*
 * A message in a blocking queue
 */
struct message {
    int sock; // Payload, in our case a new client connection
    // int servSock;
    struct message *next; // Next message on the list
};

/*
 * This structure implements a blocking queue.
 * If a thread attempts to pop an item from an empty queue
 * it is blocked until another thread appends a new item.
 */
struct queue {
    pthread_mutex_t mutex; // mutex used to protect the queue
    pthread_cond_t cond;   // condition variable for threads to sleep on
    struct message *first; // first message in the queue
    struct message *last;  // last message in the queue
    unsigned int length;   // number of elements on the queue
};

struct queue *queue;

// initializes the members of struct queue
void queue_init(struct queue *q)
{
    if (pthread_mutex_init(&q->mutex, NULL) != 0)
        die("pthread_mutex_init() failed");
    if (pthread_cond_init(&q->cond, NULL) != 0)
        die("pthread_cond_init() failed");
    q->first = NULL;
    q->last = NULL;
    q->length = 0;
}

// deallocate and destroy everything in the queue
void queue_destroy(struct queue *q)
{
    pthread_mutex_lock(&q->mutex);
    while (q->length > 0) {
        q->length--;
        struct message *t = q->first;
        q->first = q->first->next;
        free(t);
    }
    free(q);
}

// put a message into the queue and wake up workers if necessary
void queue_put(struct queue *q, int sock)
{
    pthread_mutex_lock(&q->mutex);
    struct message *m = (struct message *) malloc(sizeof(struct message));
    m->sock = sock;
    // m->servSock = servSock;
    m->next = q->first;
    if (q->length == 0) 
        q->first = m;
    else
        q->last->next = m;
    q->last = m;
    q->length++;
    if (pthread_cond_broadcast(&q->cond) != 0)
        die("pthread_cond_broadcast() failed");
    pthread_mutex_unlock(&q->mutex);
}

// take a socket descriptor from the queue; block if necessary
struct message * queue_get(struct queue *q)
{
    pthread_mutex_lock(&q->mutex);
    while (q->length == 0)
        pthread_cond_wait(&q->cond, &q->mutex);
    struct message *m = q->first;
    q->first = q->first->next;
    if (q->length == 1)
        q->last = NULL;
    q->length--;
    pthread_mutex_unlock(&q->mutex);
    return m;
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
        const char *webRoot, const char *requestURI, int clntSock)
{
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

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        statusCode = 403; // "Forbidden"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
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

/*
 * param for worker thread 
 */
struct param {
    const char *webRoot;
    fd_set *fd;
};

/*  
 * Handle the request by worker thread
 */
void * worker(void *arg)
{   
    pthread_detach(pthread_self());
    struct param *param = (struct param *) arg;
    const char *webRoot = param->webRoot;
    char requestLine[1000];
    char line[1000];
    int statusCode;
    int clntSock;
    // int servSock;
    struct sockaddr_in clntAddr;
    unsigned int clntLen = sizeof(struct sockaddr_in);

    for (;;) {
        // wait for queue_put
        struct message *message = queue_get(queue);
        clntSock = message->sock;
        // servSock = message->servSock;
        free(message);

        if (getpeername(clntSock, (struct sockaddr *) &clntAddr, &clntLen) != 0)
            die("getpeername() failed");

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
            goto loop_end;
        }

        char *token_separators = "\t \r\n"; // tab, space, new line
        char *saveptr = requestLine;
        method = strtok_r(requestLine, token_separators, &saveptr);
        requestURI = strtok_r(NULL, token_separators, &saveptr);
        httpVersion = strtok_r(NULL, token_separators, &saveptr);
        char *extraThingsOnRequestLine = strtok_r(NULL, token_separators, &saveptr);

        // check if we have 3 (and only 3) things in the request line
        if (!method || !requestURI || !httpVersion || 
                extraThingsOnRequestLine) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support GET method 
        if (strcmp(method, "GET") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support HTTP/1.0 and HTTP/1.1
        if (strcmp(httpVersion, "HTTP/1.0") != 0 && 
            strcmp(httpVersion, "HTTP/1.1") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }
        
        // requestURI must begin with "/"
        if (!requestURI || *requestURI != '/') {
            statusCode = 400; // "Bad Request"
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

        statusCode = handleFileRequest(webRoot, requestURI, clntSock);

loop_end:
// avoiding declaration right begind handle_end
    ;
    char buffer[20];

    /*
    * Done with client request.
    * Log it, close the client socket, and go back to accepting
    * connection.
    */
    
    fprintf(stderr, "%s \"%s %s %s\" %d %s\n",
            inet_ntop(AF_INET, &(clntAddr.sin_addr), buffer, 20),
            method,
            requestURI,
            httpVersion,
            statusCode,
            getReasonPhrase(statusCode));

        // close the client socket 
        fclose(clntFp);
        // FD_SET(servSock, param->fd);
    }
    return (void *) 0;
}


int main(int argc, char *argv[])
{
    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    if (argc < 3) {
    fprintf(stderr,
        "usage: %s <server_port> [<server_port> ...] <web_root>\n",
        argv[0]);
    exit(1);
    }

    int i;
    int servSocks[32];
    memset(servSocks, -1, sizeof(servSocks));

    // Create server sockets for all ports we listen on
    for (i = 1; i < argc - 1; i++) {
        if (i >= (sizeof(servSocks) / sizeof(servSocks[0])))
            die("Too many listening sockets");
        servSocks[i - 1] = createServerSocket(atoi(argv[i]));
    }

    // initialize the socket queue
    queue = (struct queue *) malloc(sizeof(struct queue));
    queue_init(queue);

    // initialize the fd set
    pthread_t tid;
    fd_set all;
    FD_ZERO(&all);
    for (i = 1; i < argc - 1; i++) {
        FD_SET(servSocks[i - 1], &all);
    }

    // initialize the thread pool
    struct param *param = (struct param *) malloc(sizeof(struct param));
    param->webRoot = argv[argc - 1];
    param->fd = &all;
    for (i = 0; i < N_THREADS; i++) {
        if (pthread_create(&tid, NULL, worker, (void *) param) != 0)
            die("pthread_create() failed");
        thread_pool[i] = tid;
    }

    // struct sockaddr_in clntAddr;

    for (;;) {
        /*
         * wait for a client to connect
         */

        // initialize the in-out parameter
        // in for loop, we dont need to call pthread_join()
        // unsigned int clntLen = sizeof(clntAddr); 
        fd_set all1 = all;
        int label = select(FD_SETSIZE, &all1, NULL, NULL, NULL);
        if (label < 0) {
            die("select() failed");

        } else if (label == 0) {
            printf("adsfa");
            continue;
        } else {
            int servSock;
            for (servSock = 0; servSock < FD_SETSIZE; servSock++) {
                if (FD_ISSET(servSock, &all1)) {
                    // FD_CLR(servSock, &all);
                    int clntSock = accept(servSock, (struct sockaddr *) NULL, NULL);
                    if (clntSock < 0)
                        die("accept() failed");
                    // add clntSock to the queue
                    queue_put(queue, clntSock);
                }
            }
            
        }
    } // for (;;)
    queue_destroy(queue);
    free(param);
    return 0;
}





