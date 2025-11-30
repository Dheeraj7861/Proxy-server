#include "proxy_parse.h"
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <mutex>
#include <algorithm> // For std::transform
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

using namespace std;

#define MAX_BYTES 4096    
#define MAX_CLIENTS 400     
#define MAX_CACHE_SIZE 200 * (1 << 20) // 200MB size limit

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct SemGuard {
    sem_t* sem;
    SemGuard(sem_t* s) : sem(s) {}
    ~SemGuard() { sem_post(sem); }
};


class LRUCache {
private:
    struct CacheEntry {
        string url;
        string data; 
    };

    size_t capacity_bytes;
    size_t current_size;
    list<CacheEntry> lru_list; 
    unordered_map<string, list<CacheEntry>::iterator> cache_map; 
    mutex cache_lock; 

public:
    LRUCache(size_t cap) : capacity_bytes(cap), current_size(0) {}

    string get(string url) {
        lock_guard<mutex> lock(cache_lock); 
        auto it = cache_map.find(url);
        if (it == cache_map.end()) return ""; 
        lru_list.splice(lru_list.begin(), lru_list, it->second);
        return it->second->data;
    }

    void put(string url, const string& data) {
        lock_guard<mutex> lock(cache_lock);
        size_t entry_size = url.size() + data.size();
        if (entry_size > capacity_bytes) return;

        if (cache_map.find(url) != cache_map.end()) {
            auto it = cache_map[url];
            current_size -= (it->url.size() + it->data.size());
            lru_list.erase(it);
            cache_map.erase(url);
        }

        while (current_size + entry_size > capacity_bytes && !lru_list.empty()) {
            auto last = lru_list.back();
            current_size -= (last.url.size() + last.data.size());
            cache_map.erase(last.url);
            lru_list.pop_back();
        }

        lru_list.push_front({url, data});
        cache_map[url] = lru_list.begin();
        current_size += entry_size;
    }
};

// Global State
int port_number = 8080;
int proxy_socketId;
sem_t seamaphore;
LRUCache cache(MAX_CACHE_SIZE);


int send_all(int socket, const char* buffer, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent = send(socket, buffer + total_sent, length - total_sent, MSG_NOSIGNAL);
        if (sent == -1) {
            if (errno == EINTR) continue; // Interrupted by signal, retry
            return -1; // Real error
        }
        total_sent += sent;
    }
    return 0;
}


int connectRemoteServer(char* host_addr, int port_num) {
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (remoteSocket < 0) return -1;

    struct hostent *host = gethostbyname(host_addr);
    if (host == NULL) {
        close(remoteSocket); // FIX: Close FD on failure
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length);

    if (connect(remoteSocket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(remoteSocket); // Ensure close on connect fail too
        return -1;
    }
    return remoteSocket;
}

int sendErrorMessage(int socket, int status_code) {
    char str[1024];
    string currentTime;
    time_t now = time(0);
    struct tm data = *gmtime(&now);
    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S %Z", &data);
    
    string msg;
    switch(status_code) {
        case 400: msg = "400 Bad Request"; break;
        case 500: msg = "500 Internal Server Error"; break;
        case 501: msg = "501 Not Implemented"; break;
        default: msg = "500 Internal Server Error"; break;
    }
    snprintf(str, sizeof(str), "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\nDate: %s\r\n\r\n", status_code, msg.c_str(), timebuf);
    send_all(socket, str, strlen(str));
    return 1;
}

// ----------------------------------------------------------
//  Request Handler
// ----------------------------------------------------------
int handle_request(int clientSocket, ParsedRequest *request, string url) {
    // 1. Reconstruct Request (Robust Header Forwarding)
    string req_str = string(request->method) + " " + string(request->path) + " " + string(request->version) + "\r\n";

    for (size_t i = 0; i < request->headersused; i++) {
        string key(request->headers[i].key);
        string value(request->headers[i].value);
        
        // Fix F: Case-insensitive comparison
        string key_lower = key;
        transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);

        if (key_lower == "host" || key_lower == "connection") continue;
        
        req_str += key + ": " + value + "\r\n";
    }
    
    req_str += "Host: " + string(request->host) + "\r\n";
    req_str += "Connection: close\r\n\r\n";

    // 2. Connect to Remote
    int server_port = 80;
    if (request->port != NULL) server_port = atoi(request->port);
    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if (remoteSocketID < 0) return -1;

    // 3. Send Request
    if (send_all(remoteSocketID, req_str.c_str(), req_str.size()) < 0) {
        close(remoteSocketID);
        return -1;
    }

    // 4. Relay Response & Capture for Cache
    string temp_cache_data;
    vector<char> buffer(MAX_BYTES);
    ssize_t bytes_received; // Use ssize_t for recv return

    while ((bytes_received = recv(remoteSocketID, buffer.data(), MAX_BYTES, 0)) > 0) {
        if (send_all(clientSocket, buffer.data(), (size_t)bytes_received) < 0) break;
        temp_cache_data.append(buffer.data(), bytes_received);
    }

    // 5. Store in Cache
    cache.put(url, temp_cache_data);
    
    // Graceful shutdown logic (optional but recommended)
    shutdown(remoteSocketID, SHUT_RDWR);
    close(remoteSocketID);
    return 0;
}

// ----------------------------------------------------------
//  Thread Function
// ----------------------------------------------------------
void* thread_fn(void* socketNew) {
    // 1. Semaphore Admission
    sem_wait(&seamaphore);
    
    SemGuard guard(&seamaphore); 

    int socket = *(int*)socketNew;
    delete (int*)socketNew;

    // Fix D: Detach thread to prevent resource leaks
    pthread_detach(pthread_self()); 

    // Fix C: Robust Header Accumulation
    string raw_req;
    vector<char> buffer(MAX_BYTES);
    size_t total_bytes = 0;
    bool header_complete = false;
    const size_t MAX_HEADER_SIZE = 64 * 1024; // 64KB Safety Limit

    while(total_bytes < MAX_HEADER_SIZE) {
        ssize_t recved = recv(socket, buffer.data(), MAX_BYTES, 0);
        if (recved <= 0) break; 

        raw_req.append(buffer.data(), recved);
        total_bytes += recved; // Increment total bytes

        // Check for End of Headers
        if (raw_req.find("\r\n\r\n") != string::npos) {
            header_complete = true;
            break;
        }
    }

    if (header_complete) {
        string cached_resp = cache.get(raw_req); 

        if (!cached_resp.empty()) {
            // HIT
            send_all(socket, cached_resp.c_str(), cached_resp.size());
            cout << "Data retrieved from the Cache" << endl;
        } else {
            // MISS
            ParsedRequest* request = ParsedRequest_create();
            // Use total_bytes/raw_req size
            if (ParsedRequest_parse(request, raw_req.c_str(), raw_req.size()) < 0) {
                sendErrorMessage(socket, 400);
            } else {
                if (string(request->method) == "GET") {
                     if (handle_request(socket, request, raw_req) == -1) {
                         sendErrorMessage(socket, 500);
                     }
                } else {
                    sendErrorMessage(socket, 501);
                }
            }
            ParsedRequest_destroy(request);
        }
    } else {
        // Did not receive full headers or connection closed early
        if (total_bytes > 0) sendErrorMessage(socket, 400);
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    // guard destructor calls sem_post() here
    return NULL;
}

// ----------------------------------------------------------
//  Main
// ----------------------------------------------------------
int main(int argc, char * argv[]) {
    if (argc == 2) port_number = atoi(argv[1]);
    printf("Setting Proxy Server Port : %d\n", port_number);

    // Ignore SIGPIPE globally to prevent process crash on write to closed socket
    // (Backup to MSG_NOSIGNAL)
    signal(SIGPIPE, SIG_IGN); 

    sem_init(&seamaphore, 0, MAX_CLIENTS);
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0) exit(1);

    int reuse = 1;
    setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Port bind failed");
        exit(1);
    }

    if (listen(proxy_socketId, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(1);
    }
    printf("Server Listening...\n");

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socketId;
    
    while (1) {
        client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr, &client_len);
        if (client_socketId < 0) {
            if (errno == EINTR) continue; // Handle signal interrupt
            perror("Error in Accepting connection");
            continue;
        }

        int* client_sock_ptr = new int(client_socketId); 
        pthread_t tid;
        if (pthread_create(&tid, NULL, thread_fn, (void*)client_sock_ptr) != 0) {
            perror("Failed to create thread");
            delete client_sock_ptr;
            close(client_socketId);
        }
    }

    close(proxy_socketId);
    return 0;
}
