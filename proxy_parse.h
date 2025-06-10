#ifndef PROXY_PARSE_H
#define PROXY_PARSE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

/*
 * Structures and functions for parsing HTTP requests
 */

#define MAX_ELEMENT_SIZE 2048
#define MAX_REQ_LEN 65536

struct ParsedHeader {
    char *name;
    char *value;
    struct ParsedHeader *next;
};

typedef struct ParsedRequest {
    char *method;
    char *protocol;
    char *host;
    char *port;
    char *path;
    char *version;
    char *buf;
    int buf_len;
    struct ParsedHeader *headers;
    int header_count;
} ParsedRequest;

/*
 * debug() prints out debugging info if DEBUG is set to 1
 * usage: debug("this is a debug message");
 * or:    debug("the number is %d", 55);
 */
#define DEBUG 0
void debug(const char* format, ...);

/*
 * ParsedRequest_create() allocates and returns a new ParsedRequest structure
 */
ParsedRequest* ParsedRequest_create();

/*
 * ParsedRequest_parse() takes a ParsedRequest structure and fills it in
 * based on the given HTTP request string
 * returns 0 on success, -1 on failure
 */
int ParsedRequest_parse(ParsedRequest *pr, const char *buf, int buflen);

/*
 * ParsedRequest_unparse() prints the given ParsedRequest structure to the
 * given file descriptor
 */
void ParsedRequest_unparse(ParsedRequest *pr, char *buf, size_t buflen);

/*
 * ParsedRequest_unparse_headers() prints the headers from the given
 * ParsedRequest structure to the given buffer
 * returns the number of bytes written, -1 on error
 */
int ParsedRequest_unparse_headers(ParsedRequest *pr, char *buf, size_t buflen);

/*
 * ParsedRequest_destroy() frees all memory associated with the given
 * ParsedRequest structure
 */
void ParsedRequest_destroy(ParsedRequest *pr);

/*
 * ParsedRequest_printRequestLine() prints the request line from the given
 * ParsedRequest structure to the given file descriptor
 */
void ParsedRequest_printRequestLine(ParsedRequest *pr, char *buf, size_t buflen, size_t *tmp);

/*
 * ParsedHeader_create() allocates and returns a new ParsedHeader structure
 */
struct ParsedHeader* ParsedHeader_create(char *name, char *value);

/*
 * ParsedHeader_destroyOne() frees all memory associated with the given
 * ParsedHeader structure (but not the next field)
 */
void ParsedHeader_destroyOne(struct ParsedHeader *ph);

/*
 * ParsedHeader_destroy() frees all memory associated with the given
 * ParsedHeader structure and all subsequent headers
 */
void ParsedHeader_destroy(struct ParsedHeader *ph);

/*
 * ParsedRequest_getHeader() returns the value of the header with the given
 * name, or NULL if no such header exists
 */
char* ParsedRequest_getHeader(ParsedRequest *pr, const char *name);

/*
 * ParsedRequest_setHeader() sets the value of the header with the given name
 * to the given value. If no such header exists, it is created.
 * returns 0 on success, -1 on failure
 */
int ParsedRequest_setHeader(ParsedRequest *pr, const char *name, const char *value);

#endif /* PROXY_PARSE_H */

