#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>

#define MAX_BYTES 8192              // Increased buffer size for better performance
#define MAX_CLIENTS 1200            // Increased to handle 1000+ concurrent requests
#define THREAD_POOL_SIZE 50         // Fixed thread pool size
#define MAX_SIZE 200*(1<<20)        // Size of the cache (200MB)
#define MAX_ELEMENT_SIZE 10*(1<<20) // Max size of cache element (10MB)
#define QUEUE_SIZE 2000             // Request queue size
#define CONNECTION_TIMEOUT 30       // Connection timeout in seconds

// Enhanced cache element structure
typedef struct cache_element {
    char* data;                     // Response data
    int len;                        // Length of data
    char* url;                      // Request URL (key)
    time_t lru_time_track;          // LRU timestamp
    time_t creation_time;           // Cache creation time
    int access_count;               // Access frequency counter
    struct cache_element* next;     // Next element pointer
    struct cache_element* prev;     // Previous element pointer (for O(1) removal)
} cache_element;

// Work queue structure for thread pool
typedef struct work_item {
    int client_socket;
    struct sockaddr_in client_addr;
    struct work_item* next;
} work_item;

typedef struct work_queue {
    work_item* head;
    work_item* tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} work_queue;

// Connection pool for upstream servers
typedef struct connection_pool {
    int* sockets;
    char** hosts;
    int* ports;
    time_t* last_used;
    int size;
    int capacity;
    pthread_mutex_t mutex;
} connection_pool;

// Global variables
int port_number = 8080;
int proxy_socketId;
pthread_t worker_threads[THREAD_POOL_SIZE];
work_queue request_queue;
pthread_mutex_t connection_limit_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t connection_available = PTHREAD_COND_INITIALIZER;
int active_connection_count = 0;
volatile int server_running = 1;

// Cache globals with enhanced thread safety
cache_element* cache_head;
cache_element* cache_tail;
int cache_size;
pthread_rwlock_t cache_rwlock;  // Read-write lock for better performance
pthread_mutex_t cache_stats_mutex;

// Performance statistics
struct {
    long total_requests;
    long cache_hits;
    long cache_misses;
    long bytes_served;
    double avg_response_time;
    pthread_mutex_t mutex;
} stats = {0, 0, 0, 0, 0.0, PTHREAD_MUTEX_INITIALIZER};

// Connection pool
connection_pool conn_pool = {NULL, NULL, NULL, NULL, 0, 100, PTHREAD_MUTEX_INITIALIZER};

// Function prototypes
void* worker_thread(void* arg);
void enqueue_request(int client_socket, struct sockaddr_in client_addr);
work_item* dequeue_request();
void init_connection_pool();
int get_pooled_connection(char* host, int port);
void return_pooled_connection(int socket, char* host, int port);
cache_element* find_in_cache(char* url);
int add_to_cache(char* data, int size, char* url);
void remove_lru_element();
void update_cache_stats();
int handle_request_optimized(int client_socket, ParsedRequest *request, char *temp_req);
int setup_nonblocking_socket(int socket);
void cleanup_resources();
void signal_handler(int sig);
void print_stats();

// Enhanced error message sending with keep-alive support
int sendErrorMessage(int socket, int status_code) {
    char str[2048];
    char currentTime[50];
    time_t now = time(0);
    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S GMT", &data);

    const char* status_text;
    const char* html_content;
    
    switch(status_code) {
        case 400:
            status_text = "Bad Request";
            html_content = "<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>";
            break;
        case 403:
            status_text = "Forbidden";
            html_content = "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>";
            break;
        case 404:
            status_text = "Not Found";
            html_content = "<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>";
            break;
        case 500:
            status_text = "Internal Server Error";
            html_content = "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>";
            break;
        case 501:
            status_text = "Not Implemented";
            html_content = "<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>";
            break;
        case 505:
            status_text = "HTTP Version Not Supported";
            html_content = "<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>";
            break;
        default:
            return -1;
    }

    int content_len = strlen(html_content);
    snprintf(str, sizeof(str), 
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: text/html\r\n"
        "Connection: keep-alive\r\n"
        "Date: %s\r\n"
        "Server: HighPerformanceProxy/2.0\r\n"
        "\r\n%s", 
        status_code, status_text, content_len, currentTime, html_content);
    
    return send(socket, str, strlen(str), MSG_NOSIGNAL);
}

// Enhanced connection establishment with timeout and connection pooling
int connectRemoteServer(char* host_addr, int port_num) {
    // Try to get connection from pool first
    int remoteSocket = get_pooled_connection(host_addr, port_num);
    if (remoteSocket > 0) {
        return remoteSocket;
    }

    // Create new connection
    remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (remoteSocket < 0) {
        perror("Error creating socket");
        return -1;
    }

    // Set socket to non-blocking for timeout control
    setup_nonblocking_socket(remoteSocket);

    // Set socket options for performance
    int opt = 1;
    setsockopt(remoteSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(remoteSocket, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    struct hostent *host = gethostbyname(host_addr);
    if (host == NULL) {
        fprintf(stderr, "Host resolution failed for %s\n", host_addr);
        close(remoteSocket);
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);

    // Connect with timeout
    int result = connect(remoteSocket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result < 0 && errno != EINPROGRESS) {
        close(remoteSocket);
        return -1;
    }

    // Wait for connection to complete with timeout
    if (errno == EINPROGRESS) {
        fd_set write_fds;
        struct timeval timeout;
        FD_ZERO(&write_fds);
        FD_SET(remoteSocket, &write_fds);
        timeout.tv_sec = CONNECTION_TIMEOUT;
        timeout.tv_usec = 0;

        result = select(remoteSocket + 1, NULL, &write_fds, NULL, &timeout);
        if (result <= 0) {
            close(remoteSocket);
            return -1;
        }

        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(remoteSocket, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            close(remoteSocket);
            return -1;
        }
    }

    // Set back to blocking mode for data transfer
    int flags = fcntl(remoteSocket, F_GETFL, 0);
    fcntl(remoteSocket, F_SETFL, flags & ~O_NONBLOCK);

    return remoteSocket;
}

// Optimized request handling with better memory management and performance
int handle_request_optimized(int client_socket, ParsedRequest *request, char *temp_req)  {
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    char *send_buffer = (char*)malloc(MAX_BYTES);
    if (!send_buffer) {
        return -1;
    }

    // Build HTTP request more efficiently
    int len = snprintf(send_buffer, MAX_BYTES, 
        "GET %s %s\r\n"
        "Host: %s\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: HighPerformanceProxy/2.0\r\n",
        request->path, request->version, request->host);
       int server_port = (request->port != NULL) ? atoi(request->port) : 80;
    if (ParsedRequest_unparse_headers(request, send_buffer + len, MAX_BYTES - len) < 0) {
        printf("Warning: Header unparsing failed, proceeding anyway\n");
    }

   
    int remoteSocket = connectRemoteServer(request->host, server_port);

    if (remoteSocket < 0) {
        free(send_buffer);
        return -1;
    }

    // Send request to upstream server
    ssize_t bytes_sent = send(remoteSocket, send_buffer, strlen(send_buffer), MSG_NOSIGNAL);
    if (bytes_sent < 0) {
        close(remoteSocket);
        free(send_buffer);
        return -1;
    }

    // Receive and forward response with dynamic buffer
    char *response_buffer = (char*)malloc(MAX_ELEMENT_SIZE);
    if (!response_buffer) {
        close(remoteSocket);
        free(send_buffer);
        return -1;
    }

    int total_received = 0;
    int buffer_size = MAX_ELEMENT_SIZE;
    ssize_t bytes_received;

    while ((bytes_received = recv(remoteSocket, send_buffer, MAX_BYTES - 1, 0)) > 0) {
        // Forward to client immediately for better latency
        ssize_t bytes_forwarded = send(client_socket, send_buffer, bytes_received, MSG_NOSIGNAL);
        if (bytes_forwarded < 0) {
            break;
        }

        // Store in response buffer for caching
        if (total_received + bytes_received < buffer_size) {
            memcpy(response_buffer + total_received, send_buffer, bytes_received);
            total_received += bytes_received;
        } else if (total_received < MAX_ELEMENT_SIZE) {
            // Expand buffer if needed
            buffer_size = total_received + bytes_received + MAX_BYTES;
            if (buffer_size <= MAX_ELEMENT_SIZE) {
                response_buffer = (char*)realloc(response_buffer, buffer_size);
                if (response_buffer) {
                    memcpy(response_buffer + total_received, send_buffer, bytes_received);
                    total_received += bytes_received;
                }
            }
        }
    }

    if (total_received > 0) {
        response_buffer[total_received] = '\0';
        add_to_cache(response_buffer, total_received, temp_req);
        
        // Update statistics
        pthread_mutex_lock(&stats.mutex);
        stats.bytes_served += total_received;
        gettimeofday(&end_time, NULL);
        double response_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + 
                              (end_time.tv_usec - start_time.tv_usec) / 1000.0;
        stats.avg_response_time = (stats.avg_response_time * stats.total_requests + response_time) / 
                                 (stats.total_requests + 1);
        stats.total_requests++;
        pthread_mutex_unlock(&stats.mutex);
    }

    // Try to return connection to pool instead of closing
    return_pooled_connection(remoteSocket, request->host, server_port);
    
    free(send_buffer);
    free(response_buffer);
    return 0;
}

// Optimized cache lookup with read-write locks
cache_element* find_in_cache(char* url) {
    pthread_rwlock_rdlock(&cache_rwlock);
    
    cache_element* current = cache_head;
    while (current != NULL) {
        if (strcmp(current->url, url) == 0) {
            // Move to front (most recently used)
            pthread_rwlock_unlock(&cache_rwlock);
            pthread_rwlock_wrlock(&cache_rwlock);
            
            // Double-check the element still exists
            if (current && strcmp(current->url, url) == 0) {
                current->lru_time_track = time(NULL);
                current->access_count++;
                
                // Move to front if not already there
                if (current != cache_head) {
                    // Remove from current position
                    if (current->prev) current->prev->next = current->next;
                    if (current->next) current->next->prev = current->prev;
                    if (current == cache_tail) cache_tail = current->prev;
                    
                    // Move to front
                    current->prev = NULL;
                    current->next = cache_head;
                    if (cache_head) cache_head->prev = current;
                    cache_head = current;
                    if (!cache_tail) cache_tail = current;
                }
                
                pthread_rwlock_unlock(&cache_rwlock);
                
                // Update statistics
                pthread_mutex_lock(&stats.mutex);
                stats.cache_hits++;
                pthread_mutex_unlock(&stats.mutex);
                
                return current;
            }
            pthread_rwlock_unlock(&cache_rwlock);
            return NULL;
        }
        current = current->next;
    }
    
    pthread_rwlock_unlock(&cache_rwlock);
    
    // Update statistics
    pthread_mutex_lock(&stats.mutex);
    stats.cache_misses++;
    pthread_mutex_unlock(&stats.mutex);
    
    return NULL;
}

// Enhanced LRU removal with better algorithm
void remove_lru_element() {
    pthread_rwlock_wrlock(&cache_rwlock);
    
    if (!cache_tail) {
        pthread_rwlock_unlock(&cache_rwlock);
        return;
    }
    
    cache_element* lru = cache_tail;
    time_t oldest_time = lru->lru_time_track;
    
    // Find least recently used element
    cache_element* current = cache_tail;
    while (current != NULL) {
        if (current->lru_time_track < oldest_time) {
            oldest_time = current->lru_time_track;
            lru = current;
        }
        current = current->prev;
    }
    
    // Remove the LRU element
    if (lru->prev) lru->prev->next = lru->next;
    if (lru->next) lru->next->prev = lru->prev;
    if (lru == cache_head) cache_head = lru->next;
    if (lru == cache_tail) cache_tail = lru->prev;
    
    cache_size -= (lru->len + strlen(lru->url) + sizeof(cache_element));
    
    free(lru->data);
    free(lru->url);
    free(lru);
    
    pthread_rwlock_unlock(&cache_rwlock);
}

// Optimized cache addition
int add_to_cache(char* data, int size, char* url) {
    int element_size = size + strlen(url) + sizeof(cache_element);
    
    if (element_size > MAX_ELEMENT_SIZE) {
        return 0;
    }
    
    pthread_rwlock_wrlock(&cache_rwlock);
    
    // Make space if needed
    while (cache_size + element_size > MAX_SIZE && cache_tail) {
        pthread_rwlock_unlock(&cache_rwlock);
        remove_lru_element();
        pthread_rwlock_wrlock(&cache_rwlock);
    }
    
    cache_element* element = (cache_element*)malloc(sizeof(cache_element));
    if (!element) {
        pthread_rwlock_unlock(&cache_rwlock);
        return 0;
    }
    
    element->data = (char*)malloc(size + 1);
    element->url = (char*)malloc(strlen(url) + 1);
    
    if (!element->data || !element->url) {
        free(element->data);
        free(element->url);
        free(element);
        pthread_rwlock_unlock(&cache_rwlock);
        return 0;
    }
    
    memcpy(element->data, data, size);
    element->data[size] = '\0';
    strcpy(element->url, url);
    element->len = size;
    element->lru_time_track = time(NULL);
    element->creation_time = time(NULL);
    element->access_count = 1;
    element->prev = NULL;
    element->next = cache_head;
    
    if (cache_head) cache_head->prev = element;
    cache_head = element;
    if (!cache_tail) cache_tail = element;
    
    cache_size += element_size;
    
    pthread_rwlock_unlock(&cache_rwlock);
    return 1;
}

// Worker thread function for thread pool
void* worker_thread(void* arg) {
    while (server_running) {
        work_item* item = dequeue_request();
        if (!item) continue;
        
        int client_socket = item->client_socket;
        free(item);
        
        // Process the client request
        char *buffer = (char*)calloc(MAX_BYTES, sizeof(char));
        if (!buffer) {
            close(client_socket);
            pthread_mutex_lock(&connection_limit_mutex);
active_connection_count--;
pthread_cond_signal(&connection_available);
pthread_mutex_unlock(&connection_limit_mutex);
            continue;
        }
        
        ssize_t bytes_received = recv(client_socket, buffer, MAX_BYTES - 1, 0);
        
        // Ensure we receive the complete HTTP request
        while (bytes_received > 0 && !strstr(buffer, "\r\n\r\n")) {
            ssize_t additional = recv(client_socket, buffer + strlen(buffer), 
                                    MAX_BYTES - strlen(buffer) - 1, 0);
            if (additional <= 0) break;
            bytes_received += additional;
        }
        
        if (bytes_received > 0) {
            char *temp_req = strdup(buffer);
            if (temp_req) {
                // Check cache first
                cache_element* cached = find_in_cache(temp_req);
                
                if (cached != NULL) {
                    // Serve from cache - send in optimal chunks
                    size_t sent = 0;
                    while (sent < cached->len) {
                        size_t chunk_size = (cached->len - sent > MAX_BYTES) ? 
                                          MAX_BYTES : (cached->len - sent);
                        ssize_t bytes_sent = send(client_socket, cached->data + sent, 
                                                chunk_size, MSG_NOSIGNAL);
                        if (bytes_sent <= 0) break;
                        sent += bytes_sent;
                    }
                    printf("Cache hit: %s\n", temp_req);
                } else {
                    // Parse and handle request
                    ParsedRequest* request = ParsedRequest_create();
                    if (ParsedRequest_parse(request, buffer, strlen(buffer)) == 0) {
                        if (strcmp(request->method, "GET") == 0 && 
                            request->host && request->path) {
                            if (handle_request_optimized(client_socket, request, temp_req) < 0) {
                                sendErrorMessage(client_socket, 500);
                            }
                        } else {
                            sendErrorMessage(client_socket, 501);
                        }
                    } else {
                        sendErrorMessage(client_socket, 400);
                    }
                    ParsedRequest_destroy(request);
                }
                free(temp_req);
            }
        }
        
        free(buffer);
        shutdown(client_socket, SHUT_RDWR);
        close(client_socket);
        pthread_mutex_lock(&connection_limit_mutex);
active_connection_count--;
pthread_cond_signal(&connection_available);
pthread_mutex_unlock(&connection_limit_mutex);
    }
    return NULL;
}

// Request queue management
void enqueue_request(int client_socket, struct sockaddr_in client_addr) {
    work_item* item = (work_item*)malloc(sizeof(work_item));
    if (!item) {
        close(client_socket);
        return;
    }
    
    item->client_socket = client_socket;
    item->client_addr = client_addr;
    item->next = NULL;
    
    pthread_mutex_lock(&request_queue.mutex);
    
    while (request_queue.count >= QUEUE_SIZE) {
        pthread_cond_wait(&request_queue.not_full, &request_queue.mutex);
    }
    
    if (request_queue.tail) {
        request_queue.tail->next = item;
    } else {
        request_queue.head = item;
    }
    request_queue.tail = item;
    request_queue.count++;
    
    pthread_cond_signal(&request_queue.not_empty);
    pthread_mutex_unlock(&request_queue.mutex);
}

work_item* dequeue_request() {
    pthread_mutex_lock(&request_queue.mutex);
    
    while (request_queue.count == 0 && server_running) {
        pthread_cond_wait(&request_queue.not_empty, &request_queue.mutex);
    }
    
    if (!server_running) {
        pthread_mutex_unlock(&request_queue.mutex);
        return NULL;
    }
    
    work_item* item = request_queue.head;
    request_queue.head = item->next;
    if (!request_queue.head) {
        request_queue.tail = NULL;
    }
    request_queue.count--;
    
    pthread_cond_signal(&request_queue.not_full);
    pthread_mutex_unlock(&request_queue.mutex);
    
    return item;
}

// Connection pool implementation
void init_connection_pool() {
    pthread_mutex_lock(&conn_pool.mutex);
    conn_pool.sockets = (int*)malloc(conn_pool.capacity * sizeof(int));
    conn_pool.hosts = (char**)malloc(conn_pool.capacity * sizeof(char*));
    conn_pool.ports = (int*)malloc(conn_pool.capacity * sizeof(int));
    conn_pool.last_used = (time_t*)malloc(conn_pool.capacity * sizeof(time_t));
    
    for (int i = 0; i < conn_pool.capacity; i++) {
        conn_pool.sockets[i] = -1;
        conn_pool.hosts[i] = NULL;
    }
    pthread_mutex_unlock(&conn_pool.mutex);
}

int get_pooled_connection(char* host, int port) {
    pthread_mutex_lock(&conn_pool.mutex);
    
    for (int i = 0; i < conn_pool.size; i++) {
        if (conn_pool.sockets[i] > 0 && conn_pool.hosts[i] && 
            strcmp(conn_pool.hosts[i], host) == 0 && conn_pool.ports[i] == port) {
            
            // Check if connection is still valid and not too old
            time_t now = time(NULL);
            if (now - conn_pool.last_used[i] < 60) { // 60 second timeout
                int socket = conn_pool.sockets[i];
                conn_pool.sockets[i] = -1;
                free(conn_pool.hosts[i]);
                conn_pool.hosts[i] = NULL;
                
                pthread_mutex_unlock(&conn_pool.mutex);
                return socket;
            } else {
                // Connection too old, close it
                close(conn_pool.sockets[i]);
                conn_pool.sockets[i] = -1;
                free(conn_pool.hosts[i]);
                conn_pool.hosts[i] = NULL;
            }
        }
    }
    
    pthread_mutex_unlock(&conn_pool.mutex);
    return -1;
}

void return_pooled_connection(int socket, char* host, int port) {
    pthread_mutex_lock(&conn_pool.mutex);
    
    // Find empty slot
    for (int i = 0; i < conn_pool.capacity; i++) {
        if (conn_pool.sockets[i] == -1) {
            conn_pool.sockets[i] = socket;
            conn_pool.hosts[i] = strdup(host);
            conn_pool.ports[i] = port;
            conn_pool.last_used[i] = time(NULL);
            if (i >= conn_pool.size) conn_pool.size = i + 1;
            
            pthread_mutex_unlock(&conn_pool.mutex);
            return;
        }
    }
    
    // Pool full, just close the connection
    pthread_mutex_unlock(&conn_pool.mutex);
    close(socket);
}

int setup_nonblocking_socket(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down gracefully...\n", sig);
    server_running = 0;
    
    // Wake up all waiting threads
    pthread_mutex_lock(&request_queue.mutex);
    pthread_cond_broadcast(&request_queue.not_empty);
    pthread_mutex_unlock(&request_queue.mutex);
    
    print_stats();
    cleanup_resources();
    close(proxy_socketId);
    exit(0);
}

// Cleanup function
void cleanup_resources() {
    // Cleanup connection pool
    pthread_mutex_lock(&conn_pool.mutex);
    for (int i = 0; i < conn_pool.size; i++) {
        if (conn_pool.sockets[i] > 0) {
            close(conn_pool.sockets[i]);
        }
        free(conn_pool.hosts[i]);
    }
    free(conn_pool.sockets);
    free(conn_pool.hosts);
    free(conn_pool.ports);
    free(conn_pool.last_used);
    pthread_mutex_unlock(&conn_pool.mutex);
    
    // Cleanup cache
    pthread_rwlock_wrlock(&cache_rwlock);
    cache_element* current = cache_head;
    while (current) {
        cache_element* next = current->next;
        free(current->data);
        free(current->url);
        free(current);
        current = next;
    }
    pthread_rwlock_unlock(&cache_rwlock);
    
    // Destroy synchronization primitives
    pthread_rwlock_destroy(&cache_rwlock);
    pthread_mutex_destroy(&request_queue.mutex);
    pthread_cond_destroy(&request_queue.not_empty);
    pthread_cond_destroy(&request_queue.not_full);
   pthread_mutex_destroy(&connection_limit_mutex);
pthread_cond_destroy(&connection_available);
}

// Print performance statistics
void print_stats() {
    pthread_mutex_lock(&stats.mutex);
    printf("\n=== Performance Statistics ===\n");
    printf("Total Requests: %ld\n", stats.total_requests);
    printf("Cache Hits: %ld (%.2f%%)\n", stats.cache_hits, 
           stats.total_requests > 0 ? (stats.cache_hits * 100.0 / stats.total_requests) : 0.0);
    printf("Cache Misses: %ld (%.2f%%)\n", stats.cache_misses,
           stats.total_requests > 0 ? (stats.cache_misses * 100.0 / stats.total_requests) : 0.0);
    printf("Bytes Served: %ld MB\n", stats.bytes_served / (1024 * 1024));
    printf("Average Response Time: %.2f ms\n", stats.avg_response_time);
    printf("Cache Size: %d bytes (%.2f MB)\n", cache_size, cache_size / (1024.0 * 1024.0));
    pthread_mutex_unlock(&stats.mutex);
}

// Main function
int main(int argc, char *argv[]) {
    if (argc == 2) {
        port_number = atoi(argv[1]);
    } else {
        printf("Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    printf("Starting High-Performance Proxy Server on port %d\n", port_number);
    printf("Thread Pool Size: %d\n", THREAD_POOL_SIZE);
    printf("Max Concurrent Connections: %d\n", MAX_CLIENTS);
        printf("Cache Size: %d MB\n", MAX_SIZE / (1024 * 1024));
    printf("Max Element Size: %d MB\n", MAX_ELEMENT_SIZE / (1024 * 1024));
    printf("Queue Size: %d\n", QUEUE_SIZE);


    // Initialize cache lock
    if (pthread_rwlock_init(&cache_rwlock, NULL) != 0) {
        perror("pthread_rwlock_init failed");
        exit(1);
    }

    // Initialize request queue
    request_queue.head = NULL;
    request_queue.tail = NULL;
    request_queue.count = 0;
    pthread_mutex_init(&request_queue.mutex, NULL);
    pthread_cond_init(&request_queue.not_empty, NULL);
    pthread_cond_init(&request_queue.not_full, NULL);

    // Initialize connection pool
    init_connection_pool();

    // Create worker threads
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&worker_threads[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create failed");
            exit(1);
        }
    }

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create proxy socket
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0) {
        perror("Error opening socket");
        exit(1);
    }

    // Set socket options
    int opt = 1;
    setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(proxy_socketId, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    // Bind socket
    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof(proxy_addr));
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(port_number);

    if (bind(proxy_socketId, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) < 0) {
        perror("Error binding socket");
        exit(1);
    }

    // Listen for connections
    if (listen(proxy_socketId, QUEUE_SIZE) < 0) {
        perror("Error listening on socket");
        exit(1);
    }

    printf("Proxy server ready and listening on port %d...\n", port_number);

    // Main server loop
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Wait for connection with timeout to allow periodic stats printing
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(proxy_socketId, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int ready = select(proxy_socketId + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready > 0 && FD_ISSET(proxy_socketId, &read_fds)) {
            int client_socket = accept(proxy_socketId, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket < 0) {
                perror("Error accepting connection");
                continue;
            }
            
            // Check if we've reached max connections
            pthread_mutex_lock(&connection_limit_mutex);
if (active_connection_count >= MAX_CLIENTS) {
    pthread_mutex_unlock(&connection_limit_mutex);
    sendErrorMessage(client_socket, 503);
    close(client_socket);
    continue;
}
active_connection_count++;
pthread_mutex_unlock(&connection_limit_mutex);
            
            // Set socket to non-blocking
            setup_nonblocking_socket(client_socket);
            
            // Enqueue the request
            enqueue_request(client_socket, client_addr);
        }
        
        // Print stats periodically
        static time_t last_stats_time = 0;
        time_t now = time(NULL);
        if (now - last_stats_time >= 60) { // Every minute
            print_stats();
            last_stats_time = now;
        }
    }

    // Cleanup and shutdown
    printf("Shutting down proxy server...\n");
    
    // Wait for worker threads to finish
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(worker_threads[i], NULL);
    }
    
    cleanup_resources();
    close(proxy_socketId);
    
    printf("Proxy server shutdown complete.\n");
    return 0;
}