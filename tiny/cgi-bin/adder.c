/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
/* $begin adder */

#include "../csapp.h"

int main(void) {
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE] = {0}; // Initialize content with zeroes
    int n1 = 0, n2 = 0;

    /* Extract the two arguments */
    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        if (p) {
            *p = '\0';
            strcpy(arg1, buf);
            strcpy(arg2, p + 1);
            n1 = atoi(arg1);
            n2 = atoi(arg2);
        }
    }

    /* Make the response body */
    snprintf(content, MAXLINE, "Welcome to add.com: THE Internet addition portal.\r\n<p>");

    // If arguments were provided, calculate and show the result
    if (buf && p) {
        snprintf(content + strlen(content), MAXLINE - strlen(content), "The answer is: %d + %d = %d\r\n</p>", n1, n2, n1 + n2);
    }

    // Add Javascript to the response
    snprintf(content + strlen(content), MAXLINE - strlen(content),
             "<script>\r\n"
             "function submitForm() {\r\n"
             "    var num1 = document.getElementById('num1').value;\r\n"
             "    var num2 = document.getElementById('num2').value;\r\n"
             "    var newPath = '/cgi-bin/adder?' + num1 + '&' + num2;\r\n"
             "    window.location.href = newPath;\r\n"
             "    return false;\r\n"
             "}\r\n"
             "</script>\r\n");

    // Add the form to the response
    snprintf(content + strlen(content), MAXLINE - strlen(content),
             "<form onsubmit=\"return submitForm()\">\r\n"
             "Number 1: <input type=\"text\" id=\"num1\" name=\"num1\"><br>\r\n"
             "Number 2: <input type=\"text\" id=\"num2\" name=\"num2\"><br>\r\n"
             "<br>\r\n"
             "<input type=\"submit\" value=\"Add Numbers\">\r\n"
             "</form>\r\n"
             "<a href=\"../home.html\">Go Back Home</a>\r\n");

    snprintf(content + strlen(content), MAXLINE - strlen(content), "<p>Thanks for visiting!\r\n");

    /* Generate the HTTP response */
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);

    exit(0);
}

/* $end adder */