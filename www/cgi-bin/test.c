#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    setbuf(stdout, NULL);  /* 禁用缓冲，确保 CGI 输出立即写入管道 */

    const char *method = getenv("REQUEST_METHOD");
    const char *query  = getenv("QUERY_STRING");
    const char *script = getenv("SCRIPT_NAME");
    time_t now = time(NULL);

    printf("Content-Type: text/html; charset=utf-8\r\n\r\n");

    printf("<html><head><title>CGI C Test</title></head><body>\n");
    printf("<h1>Hello from C CGI!</h1>\n");
    printf("<p>Method: %s</p>\n", method ? method : "unknown");
    printf("<p>Script: %s</p>\n", script ? script : "unknown");
    printf("<p>Time: %s</p>\n", ctime(&now));
    printf("<p>Query: %s</p>\n", query ? query : "(none)");

    if (query && strlen(query) > 0) {
        printf("<h2>Parameters:</h2><ul>\n");
        char buf[1024];
        strncpy(buf, query, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *saveptr;
        char *pair = strtok_r(buf, "&", &saveptr);
        while (pair) {
            char *eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                printf("<li>%s = %s</li>\n", pair, eq + 1);
            } else {
                printf("<li>%s</li>\n", pair);
            }
            pair = strtok_r(NULL, "&", &saveptr);
        }
        printf("</ul>\n");
    }

    printf("</body></html>\n");
    return 0;
}
