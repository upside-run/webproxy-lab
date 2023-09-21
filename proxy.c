// #include <stdio.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
// static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
//                                     "Firefox/10.0.3\r\n";
// int main() {
//     printf("%s", user_agent_hdr);
//     return 0;
// }

#include "csapp.h"
// #include <pthread.h> // already included in csapp.h

typedef struct CachedItem {
    char *uri;               // The URI of the requested object.
    char *response;          // The HTTP response.
    int size;                // Size of the response.
    struct CachedItem *next; // Pointer to the next cached object.
} CachedItem;

typedef struct Cache {
    CachedItem *head;     // Head of the linked list.
    int total_size;       // Total size of objects in cache.
    pthread_mutex_t lock; // Mutex for this cache.
} Cache;

void doit(int fd);
int parse_uri(char *uri, char *hostname, char *pathname, char *port);
void relay_response(int clientfd, int serverfd, char **response_buffer, ssize_t *response_size);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void *thread_function(void *arg);
void cache_add(Cache *cache, char *uri, char *response, int size);
CachedItem *cache_search(Cache *cache, char *uri);
Cache cache;

int main(int argc, char **argv) {
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    cache.head = NULL;
    cache.total_size = 0;
    pthread_mutex_init(&cache.lock, NULL);

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s); a reminder that this is a proxy server.\n", hostname, port);

        /* Dynamically allocate memory for each thread */
        int *connfd_ptr = malloc(sizeof(int));
        *connfd_ptr = connfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, thread_function, connfd_ptr) != 0) {
            fprintf(stderr, "Error creating thread\n"); // Handle error
            continue;
        }
    }
}
/* $end tinymain */

/* $begin doit */
// handle one HTTP request/response transaction
void doit(int clientfd) {
    char request_buf[MAXLINE], line_buf[MAXLINE];
    int total_bytes = 0;
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], pathname[MAXLINE], port[MAXLINE];
    char *response_buffer = NULL;
    ssize_t response_size = 0;
    rio_t rio;

    /* Initialize rio */
    Rio_readinitb(&rio, clientfd);

    /* Read request line and parse them into compartments */
    ssize_t bytes1 = Rio_readlineb(&rio, request_buf, MAXLINE);
    if (!bytes1) {
        printf("No data to read in Request Line");
        clienterror(clientfd, "No request data", "400", "Bad Request", "Please submit a valid request");
        return;
    }
    sscanf(request_buf, "%s %s %s", method, uri, version);

    /* Cache lookup */
    pthread_mutex_lock(&cache.lock);
    CachedItem *item = cache_search(&cache, uri);
    pthread_mutex_unlock(&cache.lock);

    if (item) {
        printf("Served from cache: %s\n", uri);
        // Serve the cached content to the client.
        Rio_writen(clientfd, item->response, item->size);
        return;
    } else {
        printf("Fetched from server: %s\n", uri);
    }

    parse_uri(uri, hostname, pathname, port);
    snprintf(request_buf, MAXLINE, "%s %s %s\r\n", method, pathname, version);
    total_bytes += bytes1;

    /* Read request headers and append(strcat) them to request_buf */
    while (1) {
        /* Check if we've reached the end of the HTTP headers */
        if (strcmp(line_buf, "\r\n") == 0) {
            break;
        }

        /* Read a line from the client into line_buf */
        ssize_t bytes2 = Rio_readlineb(&rio, line_buf, MAXLINE - 1);

        /* Check for read errors or end of file */
        if (bytes2 <= 0) {
            printf("Error or end-of-file while reading request.");
            clienterror(clientfd, "Failed reading request", "400", "Bad Request", "Error reading your request");
            return;
        }

        /* Ensure we don't overflow request_buf */
        if (total_bytes + bytes2 < MAXLINE) {
            strcat(request_buf, line_buf); // Append the line to request_buf
            total_bytes += bytes2;
        } else {
            printf("Request headers too large to handle.");
            clienterror(clientfd, "Request too large", "413", "Request Entity Too Large", "Your request headers are too long");
            return;
        }
    }

    /* Open connection to end server and relay the response */
    int targetfd = open_clientfd(hostname, port);
    if (targetfd < 0) {
        printf("Error connecting to target server.\n");
        clienterror(clientfd, "Cannot connect", "500", "Internal Server Error", "Could not connect to target server");
        return;
    }

    /* Forward the request line and header to the end server */
    Rio_writen(targetfd, request_buf, total_bytes);
    // forward_requesthdrs(&rio, targetfd);

    /* Relay the target server's response to the client */
    relay_response(clientfd, targetfd, &response_buffer, &response_size);
    Close(targetfd);

    /* Add to Cache */
    pthread_mutex_lock(&cache.lock);
    cache_add(&cache, uri, response_buffer, response_size);
    free(response_buffer);
    pthread_mutex_unlock(&cache.lock);
}
/* $end doit */

/* $begin parse_uri */
// parse URI into hostname, pathname, and port number
int parse_uri(char *uri, char *hostname, char *pathname, char *port) {
    char *hostbegin, *hostend, *pathbegin;

    // check for http:// or https:// at the beginning of the URI
    if (strstr(uri, "http://")) {
        hostbegin = uri + 7;
        strcpy(port, "80");
    } else if (strstr(uri, "https://")) {
        hostbegin = uri + 8;
        strcpy(port, "443"); // default HTTPS port
    } else {
        hostbegin = uri;
        strcpy(port, "80");
    }

    // find the end of the hostname (could be a '/' or a ':')
    hostend = strpbrk(hostbegin, "/:");
    if (hostend == NULL) {
        strcpy(hostname, hostbegin);
        strcpy(pathname, "/");
        return 0;
    }

    if (*hostend == ':') { // if there's a port number
        strncpy(hostname, hostbegin, hostend - hostbegin);
        hostname[hostend - hostbegin] = '\0';
        sscanf(hostend + 1, "%[0-9]", port); // read until the next '/' (a format specifier, saying expect anything but not(^) '/')
    } else {
        strncpy(hostname, hostbegin, hostend - hostbegin);
        hostname[hostend - hostbegin] = '\0';
    }

    // find the beginning of the path
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        strcpy(pathname, "/");
    } else {
        strcpy(pathname, pathbegin);
    }

    return 0;
}
/* $end parse_uri */

void relay_response(int clientfd, int serverfd, char **response_buffer, ssize_t *response_size) {
    char buf[MAXLINE];
    ssize_t n;

    *response_buffer = NULL;
    *response_size = 0;

    char *tmp_buffer = malloc(MAX_OBJECT_SIZE); // Allocate memory for max possible response
    if (!tmp_buffer) {
        perror("malloc");
        return;
    }
    ssize_t total_bytes = 0;

    // Read data from server and write to client until no more data to read.
    while ((n = Rio_readn(serverfd, buf, sizeof(buf))) > 0) {
        // Check if the data size exceeds max object size
        if (total_bytes + n <= MAX_OBJECT_SIZE) {
            memcpy(tmp_buffer + total_bytes, buf, n); // Copy data to temp buffer
            total_bytes += n;
        }

        Rio_writen(clientfd, buf, n);
    }

    // Resize buffer to actual response size and assign to response_buffer
    if (total_bytes > 0) {
        *response_buffer = realloc(tmp_buffer, total_bytes);
        if (!*response_buffer) {
            free(tmp_buffer);
            perror("realloc");
            return;
        }
        *response_size = total_bytes;
    } else {
        free(tmp_buffer);
    }
}
/* $end relay_response */

/* $begin clienterror */
// returns an error message to the client
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];
    int body_length = 0; // Track the current length of the body content

    /* Build the HTTP response body updated to not use sprintf repeatedly (violation of C99) */
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "<html><title>Tiny Error</title>");
    body_length += snprintf(body + body_length, sizeof(body) - body_length,
                            "<body bgcolor="
                            "ffffff"
                            ">\r\n");
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "%s: %s\r\n", errnum, shortmsg);
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "<p>%s: %s\r\n", longmsg, cause);
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "<hr><em>The Tiny Web server</em>\r\n");

    /* Print the HTTP response updated to not use sprintf repeatedly (violation of C99) */
    snprintf(buf, sizeof(buf), "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));

    snprintf(buf, sizeof(buf), "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));

    snprintf(buf, sizeof(buf), "Content-length: %d\r\n\r\n", body_length);
    Rio_writen(fd, buf, strlen(buf));

    Rio_writen(fd, body, body_length);
}
/* $end clienterror */

/* $start thread_function */
// handles clinet communication
void *thread_function(void *arg) {
    int connfd = *((int *)arg);
    free(arg); // Free the dynamically allocated memory for the file descriptor.

    pthread_detach(pthread_self()); // Detach the thread to ensure resources are reclaimed when the thread finishes.
    doit(connfd);
    Close(connfd);
    return NULL;
}
/* $end thread_function */

/* $start cache_init */
void cache_init(Cache *cache) {
    cache->head = NULL;
    cache->total_size = 0;
    pthread_mutex_init(&cache->lock, NULL);
}
/* $end cache_init */

/* $start cache_search */
CachedItem *cache_search(Cache *cache, char *uri) {
    CachedItem *current = cache->head;

    while (current) {
        if (strcmp(uri, current->uri) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}
/* $end cache_search */

/* $start cache_add */
void cache_add(Cache *cache, char *uri, char *response, int size) {
    // If object is too big for the cache, ignore it.
    if (size > MAX_CACHE_SIZE) {
        return;
    }

    // Remove oldest items from cache until we have enough space.
    while (cache->total_size + size > MAX_CACHE_SIZE) {
        CachedItem *oldest = cache->head;
        cache->head = oldest->next;
        cache->total_size -= oldest->size;
        free(oldest->uri);
        free(oldest->response);
        free(oldest);
    }

    // Create new cache item.
    CachedItem *new_item = malloc(sizeof(CachedItem));
    new_item->uri = strdup(uri);
    new_item->response = malloc(size);
    memcpy(new_item->response, response, size);
    new_item->size = size;
    new_item->next = cache->head;
    cache->head = new_item;
    cache->total_size += size;
}
/* $end cache_add */