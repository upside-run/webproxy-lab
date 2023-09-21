/* $begin tinymain */
// tiny.c - Simple, iterative HTTP/1.0 Web Server with GET, HEAD
// Updated serve_static() and clienterror() to use sprintf() (C99 Standard)
// Updated to support mp4, use malloc in serve_static(), and support HTTP HEAD

#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, char *method, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *method, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv) {
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

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
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        Close(connfd);
    }
}
/* $end tinymain */

/* $begin doit */
// handle one HTTP request/response transaction
void doit(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {
        clienterror(fd, method, "501", "Not Implemented", "Tiny does not implement this method");
        return;
    }

    read_requesthdrs(&rio);

    /* Parse URI from GET request */
    is_static = parse_uri(uri, filename, cgiargs);
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
        return;
    }

    if (is_static) { /* Serve static content */
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, method, sbuf.st_size);
    } else { /* Serve dynamic content */
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, method, cgiargs);
    }
}
/* $end doit */

/* $begin read_requesthdrs */
// read HTTP request headers
void read_requesthdrs(rio_t *rp) {
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
    while (strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return;
}
/* $end read_requesthdrs */

/* $begin parse_uri */
// parse URI into filename and CGI args
// return 0 if dynamic content, 1 if static
int parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;

    if (!strstr(uri, "cgi-bin")) { /* Static content */
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        if (uri[strlen(uri) - 1] == '/')
            strcat(filename, "home.html");
        return 1;
    } else { /* Dynamic content */
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr + 1);
            *ptr = '\0';
        } else
            strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);
        return 0;
    }
}
/* $end parse_uri */

/* $begin serve_static */
// copy a file back to the client
void serve_static(int fd, char *filename, char *method, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response header updated to not use sprintf repeatedly (violation of C99) */
    int buf_length = 0; // Track the current length of the buf content
    get_filetype(filename, filetype);
    buf_length += snprintf(buf + buf_length, sizeof(buf) - buf_length, "HTTP/1.0 200 OK\r\n");
    buf_length += snprintf(buf + buf_length, sizeof(buf) - buf_length, "Server: Tiny Web Server\r\n");
    buf_length += snprintf(buf + buf_length, sizeof(buf) - buf_length, "Connection: close\r\n");
    buf_length += snprintf(buf + buf_length, sizeof(buf) - buf_length, "Content-length: %d\r\n", filesize);
    buf_length += snprintf(buf + buf_length, sizeof(buf) - buf_length, "Content-type: %s\r\n\r\n", filetype);
    Rio_writen(fd, buf, buf_length); // Changed strlen(buf) to buf_length

    printf("%s", buf);

    /* Send response body to client (edited below) */
    // srcfd = Open(filename, O_RDONLY, 0);
    // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    // Close(srcfd);
    // Rio_writen(fd, srcp, filesize);
    // Munmap(srcp, filesize);

    /* Send response but with malloc, rio_readn, rio_written instead of mmap (Homework #9) */
    // use srcp as buffer instead of a pointer to source for Mmap()
    if (strcasecmp(method, "HEAD") != 0) {
        srcp = (char *)malloc(filesize);
        if (!srcp) {
            return;
        }
        srcfd = Open(filename, O_RDONLY, 0);
        Rio_readn(srcfd, srcp, filesize); // read from srcfd into srcp
        Close(srcfd);                     // close srcfd
        Rio_writen(fd, srcp, filesize);   // write to client from srcp
        free(srcp);                       // free srcp
    }
    /* That said, there is really no need to use malloc if its serving static content (no modifications) */
    // srcfd = Open(filename, O_RDONLY, 0);
    // while ((n = Rio_readn(srcfd, buf, sizeof(buf))) > 0) {
    //     Rio_writen(fd, buf, n);
    // }
    // Close(srcfd);
}

// derive file type from file name
void get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".mp4"))
        strcpy(filetype, "video/mp4");
    else
        strcpy(filetype, "text/plain");
}
/* $end serve_static */

/* $begin serve_dynamic */
// run a CGI program on behalf of the client
void serve_dynamic(int fd, char *filename, char *method, char *cgiargs) {
    char buf[MAXLINE], *emptylist[] = {NULL};

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (strcasecmp(method, "HEAD") != 0) {
        if (Fork() == 0) { /* Child */
            /* Real server would set all CGI vars here */
            setenv("QUERY_STRING", cgiargs, 1);
            Dup2(fd, STDOUT_FILENO);              /* Redirect stdout to client */
            Execve(filename, emptylist, environ); /* Run CGI program */
        }
    }
    Wait(NULL); /* Parent waits for and reaps child */
}
/* $end serve_dynamic */

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