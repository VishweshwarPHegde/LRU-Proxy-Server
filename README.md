# LRU-Proxy-Server
# High-Performance HTTP Proxy Server

A multi-threaded, high-performance HTTP proxy server implemented in C with advanced caching, connection pooling, and request optimization features. Designed to handle 1000+ concurrent connections efficiently.

---

## 🚀 Features

### ✅ Core Functionality
- **HTTP/1.1 Proxy Server** – Full HTTP proxy implementation with GET request support
- **Multi-threaded Architecture** – Fixed thread pool with 50 worker threads
- **High Concurrency** – Handles up to 1200 concurrent client connections
- **Request Queue** – Buffered request handling with queue size of 2000

### ⚙️ Performance Optimizations
- **LRU Cache System** – 200MB intelligent caching with 10MB max element size
- **Connection Pooling** – Reusable upstream server connections for better performance
- **Non-blocking I/O** – Timeout-controlled socket operations
- **Memory Management** – Optimized buffer allocation and deallocation
- **Keep-Alive Support** – HTTP connection reuse for reduced latency

### 🔧 Advanced Features
- **Real-time Statistics** – Performance monitoring with cache hit/miss ratios
- **Graceful Shutdown** – Signal handling for clean server termination
- **Error Handling** – Comprehensive HTTP status code responses (400, 403, 404, 500, 501, 505)
- **Thread Safety** – Read-write locks and mutex synchronization
- **Resource Management** – Automatic cleanup and memory leak prevention

---

## 🛠 Technical Specifications

| Component                 | Specification         |
|---------------------------|-----------------------|
| Max Concurrent Connections | 1,200                |
| Thread Pool Size           | 50 workers           |
| Cache Size                 | 200 MB               |
| Max Cache Element          | 10 MB                |
| Request Queue Size         | 2,000                |
| Connection Timeout         | 30 seconds           |
| Buffer Size                | 8,192 bytes          |

---

## 📦 Prerequisites

- **OS**: Linux/Unix-based system
- **Compiler**: GCC with C99 support
- **Libraries**: 
  - POSIX threads (`pthread`)
  - Standard C libraries
  - Socket libraries
- **Dependency**: `proxy_parse.h` (HTTP request parser)

---

## 🔧 Installation

```bash
# Clone the Repository
git clone https://github.com/yourusername/high-performance-proxy.git
cd high-performance-proxy

# Compile the Server
gcc -o proxy_server lru_proxy_with_cache.c proxy_parse.c -lpthread -std=c99 -O2

# Make Executable
chmod +x proxy_server
