/*
 * Session-based Cache Proxy Server using pthread.h
 * Work by jungle_kai on 2023-09-20
 */

#include "csapp.h"

// Define the maximum cache and object sizes.
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

typedef struct CachedItem {
    char *uri;               // URI of the requested object.
    char *response;          // HTTP response.
    int size;                // Size of the response.
    struct CachedItem *next; // Pointer to the next cached item.
} CachedItem;

typedef struct Cache {
    CachedItem *head;     // Head of the linked list.
    int total_size;       // Total size of objects in the cache.
    pthread_mutex_t lock; // Mutex for accessing the cache.
} Cache;

// Function declarations.
void doit(int fd);
void parse_uri(char *uri, char *hostname, char *pathname, char *port);
void relay_response(int clientfd, int serverfd, char **response_buffer, ssize_t *response_size);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void *thread_function(void *arg);
void cache_init(Cache *cache);
void cache_add(Cache *cache, char *uri, char *response, int size);
CachedItem *cache_search(Cache *cache, char *uri);

// Global cache variable.
Cache cache;

int main(int argc, char **argv) {
    int listenfd, connfd;                  // sockets to listen and to connect (accept).
    char hostname[MAXLINE], port[MAXLINE]; // buffers for hostnames and ports.

    // Unique datatypes for sockets (Protocol-agnostic, Efficient/Safe)
    socklen_t clientlen;                // represents sizes (more specifically, size of socket addresses).
    struct sockaddr_storage clientaddr; // stores socket addresses for IPv4 and v6 (large enough).

    // Initialize cache.
    cache_init(&cache);

    // Check for valid command line arguments.
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // Start listening for incoming connections.
    listenfd = Open_listenfd(argv[1]); // argv[1] = port number input at shell.
    while (1) {
        /*
        clientaddr is an empty sockaddr_storage type that is designed to be large enough to handle both IPv4 and v6.
        Because accept() expects a pointer to size of clientaddr, we initialize it with the current empty size.
        accept() waits for a client to connect to listenfd, and once connected, fills clientaddr structure with whatever format (+ fills clientlen).

        (SA) is a generic socket address type structure (struct sockaddr; a tool) that can point to any of the IPv4 or IPv6 types.
        Functions in the server are designed to expect this structure when accepting IP addresses and relevant information.
        (SA*) casting is done to make sure that clientaddr structure is compatiable with functions that expect this generic type.
        */
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s); reminder: this is a proxy server.\n", hostname, port);

        /*
        This section creates a new thread for each connection.
        connfd is a local variable on the stack, and threads may race to use it (and change it).
        By allocating memory in this loop and saving the connfd value to it, each thread gets its own copy.

        pthread_t will declare the thread identifier variable (stores the ID).
        pthread_create() returns 0 if successful. The if conditional statement checks for irregularities.
        Each new thread after pthread_create() will start executing in the thread_function using the connfd_ptr.
        */
        int *connfd_ptr = malloc(sizeof(int));
        *connfd_ptr = connfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, thread_function, connfd_ptr) != 0) {
            fprintf(stderr, "Error creating thread\n");
            continue;
        }
    }
}

/*
 * Handles one HTTP request/response transaction.
 * Read the client request, check cache, if not available forward the request to target server.
 * Get response from the target server, send it to the client, add to cache.
 */
void doit(int clientfd) {
    // Declare buffers and other local variables.
    char request_buf[MAXLINE], line_buf[MAXLINE];             // Buffers to store the HTTP request and individual lines of the request.
    int total_bytes = 0;                                      // Track the total bytes read from the client's request.
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];     // Variables to hold the parsed HTTP method, URI, and version.
    char hostname[MAXLINE], pathname[MAXLINE], port[MAXLINE]; // Variables to store the parsed URI components.
    char *response_buffer = NULL;                             // Buffer to store the server's response.
    ssize_t response_size = 0;                                // Size of the server's response.
    rio_t rio;                                                // RIO structure to facilitate robust I/O operations.

    // Initialize the robust I/O (RIO) structure.
    // This structure makes it easier to handle I/O operations in a way that's resilient to some common issues (like partial reads/writes).
    Rio_readinitb(&rio, clientfd); // destination, source.

    // Read the client's request line (always the first line in the request).
    // size_t is an unsigned integral data type used to represent the size of objects in bytes (guaranteed to be big enough; protocol-agnostic).
    // ssize_t is a signed size_t that would allow functions to return -1 upon error.
    ssize_t bytes1 = Rio_readlineb(&rio, request_buf, MAXLINE);

    // If no data was read, send a 400 Bad Request error.
    if (!bytes1) {
        printf("No data to read in Request Line");
        clienterror(clientfd, "No request data", "400", "Bad Request", "Please submit a valid request");
        return;
    }

    // Parse the client's request line to extract the method, URI, and HTTP version.
    // sscanf() reads from the buffer and saves to variables. %s reads upto the whitespace in the buffer.
    sscanf(request_buf, "%s %s %s", method, uri, version);

    // Mutex lock-perform-unlock is a common practice to ensure thread safety.
    pthread_mutex_lock(&cache.lock);              // Lock the cache mutex to ensure thread-safe operations.
    CachedItem *item = cache_search(&cache, uri); // Search the cache for the requested URI. Return pointer if found (else: NULL).
    pthread_mutex_unlock(&cache.lock);            // Unlock the cache mutex after the search.

    // If the requested URI is found in the cache, write that response to the client (pass its size to rio_written for it to function).
    if (item) {
        printf("Served from cache: %s\n", uri);           // Debugging statement.
        Rio_writen(clientfd, item->response, item->size); // Send the cached response to the client.
        return;
    } else {
        printf("Fetched from server: %s\n", uri); // Debugging statement.
    }

    // If not found in the cache, parse the URI to get the hostname, pathname, and port.
    parse_uri(uri, hostname, pathname, port);

    // Modify the request for the target server. This is to ensure that it's properly formatted.
    snprintf(request_buf, MAXLINE, "%s %s %s\r\n", method, pathname, version); // destination, size, format.
    total_bytes += bytes1;

    // Debugging
    printf("\n\n@@@@ HOST, PATH, PORT = %s ++ %s ++ %s \n", hostname, pathname, port);
    printf("@@@@ LENGTH OF EACH HPP = %d ++ %d ++ %d \n", strlen(hostname), strlen(pathname), strlen(port));
    printf("@@@@ REQUEST LINE = %s ++ %s ++ %s", method, pathname, version);

    // Loop to read and store the client's request headers.
    while (1) {
        if (strcmp(line_buf, "\r\n") == 0) {
            break;
        }

        ssize_t bytes2 = Rio_readlineb(&rio, line_buf, MAXLINE - 1);

        if (bytes2 <= 0) {
            printf("Error or end-of-file while reading request.");
            clienterror(clientfd, "Failed reading request", "400", "Bad Request", "Error reading your request");
            return;
        }

        // Ensure the total bytes of the request do not exceed the maximum allowed.
        if (total_bytes + bytes2 < MAXLINE) {
            strcat(request_buf, line_buf); // Append the line to the overall request buffer.
            total_bytes += bytes2;
        } else {
            // If the request is too large, send a 413 Request Entity Too Large error.
            printf("Request headers too large to handle.");
            clienterror(clientfd, "Request too large", "413", "Request Entity Too Large", "Your request headers are too long");
            return;
        }
    }

    // Debugging
    printf("\n\n@@@@ FULL HEADER = \n%s\n", request_buf);

    // Try to establish a connection to the target server.
    int targetfd = open_clientfd(hostname, port);
    if (targetfd < 0) {
        printf("Error connecting to target server.\n");
        clienterror(clientfd, "Cannot connect", "500", "Internal Server Error", "Could not connect to target server");
        return;
    }

    Rio_writen(targetfd, request_buf, total_bytes);                       // Forward the client's request to the target server.
    relay_response(clientfd, targetfd, &response_buffer, &response_size); // Forward response body

    // Receive from the target and relay it back to the client.
    Close(targetfd); // Close the connection to the target server.

    if (response_size > 0) {                                    // If a valid response is received, add it to the cache.
        pthread_mutex_lock(&cache.lock);                        // Ensure thread-safe addition to the cache by locking the mutex.
        cache_add(&cache, uri, response_buffer, response_size); // Add the response to the cache.
        pthread_mutex_unlock(&cache.lock);                      // Unlock the cache mutex after adding (note that lock-unlock is as quick as possible).
    }

    free(response_buffer); // Free the allocated memory for the response buffer.
}

/*
 * Parses a URI into its hostname, pathname, and port components.
 * It expects the URI that it can parse, plus hostname/pathname/port pointers that it can fill.
 */
void parse_uri(char *uri, char *hostname, char *pathname, char *port) {
    /*
    When the client has a Proxy server setup, the OS knows to send its HTTP requests in the following format:
    - Example URI: "http://www.example.com:8080/index.html"

    But when the OS doesn't have this setup, it sends the URI as the pathname alone (HOST header contains the hostname and optional port number).
    Generally, proxy servers check for both the URI and the HOST header to make sure the request finds its destination.
    */

    char *protocol_ptr; // Pointer to the "http://"
    char *host_ptr;     // Pointer to the "www.example.com"
    char *port_ptr;     // Pointer to the "8080"
    char *path_ptr;     // Pointer to the "/index.html"

    // Initialize default values.
    strcpy(port, "80"); // Default HTTP port if not specified.

    // Initialize the host_ptr by checking if the URI has "http:// or https://"
    if ((protocol_ptr = strstr(uri, "://")) != NULL) {
        host_ptr = protocol_ptr + 3; // Move past "://"
    } else {
        host_ptr = uri; // If "http://" isn't present, the URI probably starts with the hostname.
    }

    // Initialize the path_ptr by checking if a pathname is specified.
    if ((path_ptr = strchr(host_ptr, '/')) != NULL) {
        strcpy(pathname, path_ptr); // Copy the pathname.
        *path_ptr = '\0';           // Null-terminate the hostname.
    } else {
        strcpy(pathname, "/"); // Default to root if no pathname is provided.
    }

    // Initialize the port_ptr by checking if a port number is specified.
    if ((port_ptr = strchr(host_ptr, ':')) != NULL) {
        *port_ptr = '\0';           // Null-terminate the hostname.
        strcpy(port, port_ptr + 1); // Copy the port number.
    }

    // Since port and pathname used host_ptr for discovery, we can finally initialize the hostname.
    strcpy(hostname, host_ptr); // Copy the hostname.
}

/*
 * Relays the server's response back to the client and stores it in a buffer.
 * Read from serverfd into buf, and send that buf-sized chunk to the clientfd (stream by chunks for efficiency).
 * Save each reading into temp_buf (which is malloc'd because we don't know the exact size of the response).
 * Make response_buffer pointer point to a realloc of temp_buf (use the double pointer in the parameter to carry it out).
 */
void relay_response(int clientfd, int serverfd, char **response_buffer, ssize_t *response_size) {
    char buf[MAXLINE];
    ssize_t n;

    // Initialize response buffer and size (already done in main, but double checking and defending against misuse).
    *response_buffer = NULL;
    *response_size = 0;

    /*
    Readings from the serverfd are saved into buf, which is then copied over to tmp_buffer (a collection of full readings).
    By using malloc to allocate heap memory, we not only save space (stack is limited) but also put a cap on maximum size (which is unknown).
    Response buffer is a double pointer that points to tmp_buffer so that it can be carried out to doit().
    */
    char *tmp_buffer = malloc(MAX_OBJECT_SIZE);
    if (!tmp_buffer) {
        perror("malloc"); // print the last error that occured in the "" : error format.
        return;
    }

    // initialized here because there's no need to allocate memory if malloc fails.
    ssize_t total_bytes = 0;

    // Read data from server, store in temp buffer, and write to client until no more data to read.
    while ((n = Rio_readn(serverfd, buf, sizeof(buf))) > 0) {
        // Ensure the data size does not exceed the maximum allowable object size.
        if (total_bytes + n <= MAX_OBJECT_SIZE) {
            // tmp_buffer points to beginning, so this line does memcpy() to last written location.
            memcpy(tmp_buffer + total_bytes, buf, n);
            total_bytes += n;
        }

        // Relay the server's response to the client
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

/*
 * Returns an error message to the client in an HTML format.
 * To the target fd, send an error message describing cause, errnum, and text messages.
 * Note: Uses snprintf instead of sprintf to safely format strings and prevent buffer overflows.
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];
    int body_length = 0; // Track the current length of the body content

    // Build the HTTP response body using snprintf for safe formatting.
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "<html><title>Proxy Error</title>");
    body_length += snprintf(body + body_length, sizeof(body) - body_length,
                            "<body bgcolor="
                            "ffffff"
                            ">\r\n");
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "%s: %s\r\n", errnum, shortmsg);
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "<p>%s: %s\r\n", longmsg, cause);
    body_length += snprintf(body + body_length, sizeof(body) - body_length, "<hr><em>The Cached, Session-based Proxy Server</em>\r\n");

    // Print the HTTP response headers followed by the body to the client.
    snprintf(buf, sizeof(buf), "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Content-length: %d\r\n\r\n", body_length);
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, body_length);
}

/*
 * Thread function to handle client communication.
 * Each thread will start executing in this function.
 */
void *thread_function(void *arg) {
    /*
    Remember that from main(): pthread_create(&tid, NULL, thread_function, connfd_ptr).
    thread_function is a function pointer, whereby the name points to a specific memory location that it starts.
    pthread_create() expects a "void *(*routine)(void*)"; a function pointer that takes void arguemnts and returns void values.
    pthread_create() not only executes its own task, but sends the connfd_ptr as a parameter to (*routine).

    Note that thread_function is responsible for not only executing the code, but ALSO FOR freeing the allocated memory.
    It also closes the connection and terminates the thread at the very end.
    */
    int connfd = *((int *)arg);     // Retrieve the client's connection file descriptor from the argument.
    free(arg);                      // Free the dynamically allocated memory used to pass the file descriptor.
    pthread_detach(pthread_self()); // Detach the thread to ensure resources are reclaimed upon completion.
    doit(connfd);                   // Handle the client's request.
    Close(connfd);                  // Close the client's connection.
    return NULL;                    // Return NULL since the function's return type expects a pointer.
}

/*
 * Initializes a Cache structure.
 * Expects a pointer to the cache structure as parameters.
 */
void cache_init(Cache *cache) {
    cache->head = NULL;                     // Set the starting point of the cache (head) to NULL.
    cache->total_size = 0;                  // Initialize the total size of the cache to 0.
    pthread_mutex_init(&cache->lock, NULL); // Initialize the mutex associated with the cache for thread safety. NULL for a pointer to an object that specifies mutex attributes.
}

/*
 * Searches the cache for a URI and returns its corresponding CachedItem if found.
 * Expects a pointer to the cache structure and the uri to search for.
 * Returns a pointer to the cached item if found, and NULL if nothing is found.
 */
CachedItem *cache_search(Cache *cache, char *uri) {
    // Start from the beginning of the cache list.
    CachedItem *current = cache->head;

    // Traverse the cache list.
    while (current) {
        // If the current item's URI matches the given URI, return it.
        if (strcmp(uri, current->uri) == 0) {
            return current;
        }
        // Move on to the next item in the cache list.
        current = current->next;
    }

    // If no match is found after traversing the entire list, return NULL.
    return NULL;
}

/*
 * Adds a new URI and its corresponding response to the cache.
 * Parameters are target cache structure (pointer), URI, server response, and size of that response.
 * If the size exceeds the maximum cache size, its ignored. If cache lacks space, it removes the oldest item(s).
 */
void cache_add(Cache *cache, char *uri, char *response, int size) {
    // If the object size exceeds the cache's max size, ignore the object.
    if (size > MAX_CACHE_SIZE) {
        return;
    }

    // Keep removing the oldest cache items until there's enough space for the new item.
    while (cache->total_size + size > MAX_CACHE_SIZE) {

        CachedItem *oldest = cache->head;  // The oldest item is always at the head of the list.
        cache->head = oldest->next;        // Update the head to point to the next item, effectively removing the oldest item from the list.
        cache->total_size -= oldest->size; // Deduct the size of the oldest item from the cache's total size.

        // Free the memory associated with the oldest cache item.
        free(oldest->uri);
        free(oldest->response);
        free(oldest);
    }

    // Allocate memory for the new cache item.
    CachedItem *new_item = malloc(sizeof(CachedItem));

    // Allocate memory and duplicate the URI to the response pointer (strdup() does the malloc).
    new_item->uri = strdup(uri);

    // Allocate memory for the response and copy it to the new cache item.
    new_item->response = malloc(size);
    memcpy(new_item->response, response, size);

    // Set the size of the new cache item.
    new_item->size = size;

    // Insert the new item at the beginning of the list.
    new_item->next = cache->head;
    cache->head = new_item;

    // Update the cache's total size to include the size of the new item.
    cache->total_size += size;
}
