#include "proxy_parse.h"

void debug(const char* format, ...) {
    if (DEBUG) {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}

ParsedRequest* ParsedRequest_create() {
    ParsedRequest *pr = (ParsedRequest*)malloc(sizeof(ParsedRequest));
    if (pr == NULL) {
        return NULL;
    }
    
    memset(pr, 0, sizeof(ParsedRequest));
    return pr;
}

void ParsedRequest_destroy(ParsedRequest *pr) {
    if (pr == NULL) return;
    
    if (pr->buf) free(pr->buf);
    if (pr->method) free(pr->method);
    if (pr->protocol) free(pr->protocol);
    if (pr->host) free(pr->host);
    if (pr->port) free(pr->port);
    if (pr->path) free(pr->path);
    if (pr->version) free(pr->version);
    
    ParsedHeader_destroy(pr->headers);
    free(pr);
}

struct ParsedHeader* ParsedHeader_create(char *name, char *value) {
    struct ParsedHeader *ph = (struct ParsedHeader*)malloc(sizeof(struct ParsedHeader));
    if (ph == NULL) return NULL;
    
    ph->name = strdup(name);
    ph->value = strdup(value);
    ph->next = NULL;
    
    if (ph->name == NULL || ph->value == NULL) {
        free(ph->name);
        free(ph->value);
        free(ph);
        return NULL;
    }
    
    return ph;
}

void ParsedHeader_destroyOne(struct ParsedHeader *ph) {
    if (ph == NULL) return;
    
    free(ph->name);
    free(ph->value);
    free(ph);
}

void ParsedHeader_destroy(struct ParsedHeader *ph) {
    while (ph != NULL) {
        struct ParsedHeader *next = ph->next;
        ParsedHeader_destroyOne(ph);
        ph = next;
    }
}

char* ParsedRequest_getHeader(ParsedRequest *pr, const char *name) {
    if (pr == NULL || name == NULL) return NULL;
    
    struct ParsedHeader *current = pr->headers;
    while (current != NULL) {
        if (strcasecmp(current->name, name) == 0) {
            return current->value;
        }
        current = current->next;
    }
    return NULL;
}

int ParsedRequest_setHeader(ParsedRequest *pr, const char *name, const char *value) {
    if (pr == NULL || name == NULL || value == NULL) return -1;
    
    // Check if header already exists
    struct ParsedHeader *current = pr->headers;
    while (current != NULL) {
        if (strcasecmp(current->name, name) == 0) {
            // Update existing header
            free(current->value);
            current->value = strdup(value);
            return (current->value != NULL) ? 0 : -1;
        }
        current = current->next;
    }
    
    // Create new header
    struct ParsedHeader *new_header = ParsedHeader_create((char*)name, (char*)value);
    if (new_header == NULL) return -1;
    
    // Add to front of list
    new_header->next = pr->headers;
    pr->headers = new_header;
    pr->header_count++;
    
    return 0;
}

static char* trim_whitespace(char *str) {
    char *end;
    
    // Trim leading space
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return str;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    end[1] = '\0';
    return str;
}

int ParsedRequest_parse(ParsedRequest *pr, const char *buf, int buflen) {
    if (pr == NULL || buf == NULL || buflen <= 0) {
        return -1;
    }
    
    // Make a copy of the buffer
    pr->buf = (char*)malloc(buflen + 1);
    if (pr->buf == NULL) {
        return -1;
    }
    memcpy(pr->buf, buf, buflen);
    pr->buf[buflen] = '\0';
    pr->buf_len = buflen;
    
    char *line_start = pr->buf;
    char *line_end;
    
    // Parse request line
    line_end = strstr(line_start, "\r\n");
    if (line_end == NULL) {
        line_end = strstr(line_start, "\n");
        if (line_end == NULL) {
            return -1;
        }
        *line_end = '\0';
        line_end += 1;
    } else {
        *line_end = '\0';
        line_end += 2;
    }
    
    // Parse method, path, and version
    char *method_end = strchr(line_start, ' ');
    if (method_end == NULL) return -1;
    *method_end = '\0';
    pr->method = strdup(line_start);
    
    char *path_start = method_end + 1;
    char *path_end = strchr(path_start, ' ');
    if (path_end == NULL) return -1;
    *path_end = '\0';
    
    // Parse URL to extract protocol, host, port, and path
    char *url = strdup(path_start);
    if (strncmp(url, "http://", 7) == 0) {
        pr->protocol = strdup("http");
        char *host_start = url + 7;
        char *host_end = strchr(host_start, '/');
        char *port_start = strchr(host_start, ':');
        
        if (port_start && (host_end == NULL || port_start < host_end)) {
            *port_start = '\0';
            pr->host = strdup(host_start);
            port_start++;
            if (host_end) {
                *host_end = '\0';
                pr->port = strdup(port_start);
                pr->path = strdup(host_end + 1);
            } else {
                pr->port = strdup(port_start);
                pr->path = strdup("/");
            }
        } else {
            if (host_end) {
                *host_end = '\0';
                pr->host = strdup(host_start);
                pr->path = strdup(host_end + 1);
            } else {
                pr->host = strdup(host_start);
                pr->path = strdup("/");
            }
            pr->port = strdup("80");
        }
    } else {
        // Relative URL
        pr->path = strdup(url);
    }
    free(url);
    
    char *version_start = path_end + 1;
    pr->version = strdup(version_start);
    
    // Parse headers
    line_start = line_end;
    while (line_start && *line_start != '\0') {
        line_end = strstr(line_start, "\r\n");
        if (line_end == NULL) {
            line_end = strstr(line_start, "\n");
            if (line_end == NULL) {
                break;
            }
            *line_end = '\0';
            line_end += 1;
        } else {
            *line_end = '\0';
            line_end += 2;
        }
        
        if (strlen(line_start) == 0) {
            // Empty line indicates end of headers
            break;
        }
        
        char *header_separator = strchr(line_start, ':');
        if (header_separator != NULL) {
            *header_separator = '\0';
            char *header_name = trim_whitespace(line_start);
            char *header_value = trim_whitespace(header_separator + 1);
            
            struct ParsedHeader *header = ParsedHeader_create(header_name, header_value);
            if (header != NULL) {
                header->next = pr->headers;
                pr->headers = header;
                pr->header_count++;
            }
        }
        
        line_start = line_end;
    }
    
    // If no host was found in URL, try to get it from Host header
    if (pr->host == NULL) {
        char *host_header = ParsedRequest_getHeader(pr, "Host");
        if (host_header != NULL) {
            char *port_separator = strchr(host_header, ':');
            if (port_separator != NULL) {
                *port_separator = '\0';
                pr->host = strdup(host_header);
                pr->port = strdup(port_separator + 1);
                *port_separator = ':'; // Restore original
            } else {
                pr->host = strdup(host_header);
                if (pr->port == NULL) {
                    pr->port = strdup("80");
                }
            }
        }
    }
    
    return 0;
}

void ParsedRequest_printRequestLine(ParsedRequest *pr, char *buf, size_t buflen, size_t *tmp) {
    if (pr == NULL || buf == NULL || tmp == NULL) return;
    
    *tmp = snprintf(buf, buflen, "%s %s %s\r\n",
                   pr->method ? pr->method : "",
                   pr->path ? pr->path : "/",
                   pr->version ? pr->version : "HTTP/1.1");
}

int ParsedRequest_unparse_headers(ParsedRequest *pr, char *buf, size_t buflen) {
    if (pr == NULL || buf == NULL || buflen == 0) {
        return -1;
    }
    
    size_t written = 0;
    struct ParsedHeader *current = pr->headers;
    
    while (current != NULL && written < buflen - 1) {
        int header_len = snprintf(buf + written, buflen - written,
                                 "%s: %s\r\n", current->name, current->value);
        
        if (header_len < 0 || written + header_len >= buflen) {
            return -1;
        }
        
        written += header_len;
        current = current->next;
    }
    
    // Add final CRLF to end headers
    if (written < buflen - 2) {
        buf[written] = '\r';
        buf[written + 1] = '\n';
        written += 2;
        buf[written] = '\0';
    } else {
        return -1;
    }
    
    return written;
}

void ParsedRequest_unparse(ParsedRequest *pr, char *buf, size_t buflen) {
    if (pr == NULL || buf == NULL || buflen == 0) return;
    
    size_t tmp;
    ParsedRequest_printRequestLine(pr, buf, buflen, &tmp);
    
    if (tmp < buflen) {
        ParsedRequest_unparse_headers(pr, buf + tmp, buflen - tmp);
    }
}