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
#include <time.h>
#include <zlib.h>

#include "headers/proxy_parse.h"

#define MAX_CLIENTS 400
#define MAX_BYTES 4096

#define MAX_SIZE 200*(1<<20) //200MB
#define MAX_ELEMENT_SIZE 10*(1<<20) //10MB

#define MAX_BLOCKED_WEBSITES 10

const char* blocked_websites[MAX_BLOCKED_WEBSITES] = {
    "www.blockedwebsite.com"
};

typedef struct cache_element cache_element;

struct cache_element{
    char* url;
    char* data; 
    int len;
    time_t time;
    cache_element* next;
};

cache_element* head;
int cache_element_size;

cache_element* find(char* url);
int add_cache_element(char* data, int len, char* url, ParsedRequest* request);
void remove_cache_element();

int port = 8080;
int proxy_socketId; //server socket descriptor
pthread_t threadId[MAX_CLIENTS]; //thread ID of clients
sem_t semaphore; //Lock for creation of threads
pthread_mutex_t mutex; //Lock for cache_element access

int is_website_blocked(const char* host) {
    for (int i = 0; i < MAX_BLOCKED_WEBSITES; i++) {
        if (blocked_websites[i] != NULL && strcmp(host, blocked_websites[i]) == 0) {
            return 1;  // Website is blocked
        }
    }
    return 0;  // Website is not blocked
}

int decompress_data(const char* input_data, int input_len, char** output_data, int* output_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) { // 16 + MAX_WBITS for automatic gzip detection
        return -1; // Error initializing inflation
    }

    strm.avail_in = input_len;
    strm.next_in = (unsigned char*)input_data;

    *output_len = input_len * 2; // Start with a buffer twice the input size
    *output_data = (char*)malloc(*output_len);
    if (*output_data == NULL) {
        inflateEnd(&strm);
        return -1; // Memory allocation failure
    }

    strm.avail_out = *output_len;
    strm.next_out = (unsigned char*)*output_data;

    int ret = inflate(&strm, Z_NO_FLUSH);
    while (ret == Z_OK) {
        if (strm.avail_out == 0) { // If output buffer is full, increase its size
            *output_len *= 2;
            *output_data = (char*)realloc(*output_data, *output_len);
            if (*output_data == NULL) {
                inflateEnd(&strm);
                return -1; // Memory allocation failure
            }
            strm.avail_out = *output_len - strm.avail_out;
            strm.next_out = (unsigned char*)*output_data + strm.avail_out;
        }
        ret = inflate(&strm, Z_NO_FLUSH);
    }

    if (ret != Z_STREAM_END) {
        free(*output_data);
        inflateEnd(&strm);
        return -1; // Decompression failed
    }

    *output_len -= strm.avail_out;
    inflateEnd(&strm); // Cleanup
    return 0; // Success
}

int send_error(int socket, int status_code){
    char str[1024];
    char currentTime[50];
    time_t now=time(0);
    struct tm data=*gmtime(&now);

    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    const char* status_message;
    const char* title;
    const char* body;

    switch(status_code){
        case 400:
            status_message="400 Bad Request";
            title="400 Bad Request";
            body="<BODY><H1>400 Bad Request</H1>\n</BODY>";
            break;
        case 403:
            status_message="403 Forbidden";
            title="403 Forbidden";
            body="<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY>";
            break;
        case 404:
            status_message="404 Not Found";
            title="404 Not Found";
            body="<BODY><H1>404 Not Found</H1>\n</BODY>";
            break;
        case 500:
            status_message="500 Internal Server Error";
            title="500 Internal Server Error";
            body="<BODY><H1>500 Internal Server Error</H1>\n</BODY>";
            break;
        case 501:
            status_message="501 Not Implemented";
            title="501 Not Implemented";
            body="<BODY><H1>501 Not Implemented</H1>\n</BODY>";
            break;
        case 505:
            status_message="505 HTTP Version Not Supported";
            title="505 HTTP Version Not Supported";
            body="<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY>";
            break;
        default:
            return -1;
    }

    snprintf(str, sizeof(str),
        "HTTP/1.1 %s\r\n"
        "Content-Length: %ld\r\n"
        "Content-Type: text/html\r\n"
        "Connection: keep-alive\r\n"
        "Date: %s\r\n"
        "Server: TheOklama\r\n"
        "\r\n"
        "<HTML><HEAD><TITLE>%s</TITLE></HEAD>\n%s</HTML>",
        status_message, strlen(body) + strlen(title) + 100, currentTime, title, body);

    printf("%s\n", status_message);

    if(send(socket, str, strlen(str), 0)==-1){
        printf("Error sending failed\n");
        return -1;
    }

    return 1;
}

int connectRemoteServer(char* host_addr, int port){
    int remote_socket=socket(AF_INET, SOCK_STREAM, 0);
    if(remote_socket<0){
        printf("Error creating remote socket\n");
        return -1;
    }

    //converts domain name to IP address and returns a structure
    struct hostent *server=gethostbyname(host_addr);
    if (server == NULL) {
        fprintf(stderr, "Error, no such host exists\n");
        return -1;
    }

    //stores differently when connecting and when being connected to
    struct sockaddr_in server_address;
    bzero((char *)&server_address, sizeof(server_address));
    server_address.sin_family=AF_INET; //address family
    server_address.sin_port=htons(port); //port for socket connection
    //s_addr hold the IP address

    //copying from hostent object to sockaddr_in
    bcopy((char *)server->h_addr, (char *)&server_address.sin_addr.s_addr, server->h_length);
    if(connect(remote_socket, (struct sockaddr*)&server_address, (socklen_t)sizeof(server_address))<0){
        fprintf(stderr, "Error connecting to remote server\n");
        return -1;
    }

    //RPC, local socket which is connected to remote socket
    return remote_socket;
}

int handle_request(int client_socketId, ParsedRequest *request, char* temp){
    /*request body example:
    GET /index.html HTTP/1.1\r\n
    Host: example.com || www.example.com:8080\r\n
    User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) | Browser and system details
    Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp;q=0.8  | List of accepted content types
    Accept-Language: en-US,en;q=0.5 | Preffered language
    Connection: keep-alive | or close (TCP)
    */
    char* host = request->host;
    
    if (is_website_blocked(host)) {
        // Block the request by sending a 403 Forbidden response
        send_error(client_socketId, 403);
        return -1;  // Return early to avoid processing further
    }

    char* buffer=(char*)malloc(sizeof(char)*MAX_BYTES);
    strcpy(buffer, "GET ");
    strcat(buffer, request->path);
    strcat(buffer, " ");
    strcat(buffer, request->version);
    strcat(buffer, "\r\n"); 

    if(ParsedHeader_set(request, "Connection", "close")<0){
        printf("Error\n");
    }
    
    //double check for host
    if(ParsedHeader_get(request, "Host")==NULL){
        if(ParsedHeader_set(request, "Host", request->host)<0){
            printf("Error setting host\n");
        }
    }

    //used to handle large string as it is unsigned int type 
    size_t len=strlen(buffer);
    if(ParsedRequest_unparse_headers(request, buffer+len, (size_t)MAX_BYTES-len)<0){
        printf("Error unparse headers\n");
    }

    int server_port=80; //not our server, end server. Default GET goes to 80 port
    if(request->port!=NULL){
        server_port=atoi(request->port);
    }

    //socket in destination server
    int remote_socketId=connectRemoteServer(request->host, server_port);

    if(remote_socketId<0)
        return -1;

    //flag like wait for all data, dont determine route etc
    int bytes_send=send(remote_socketId, buffer, strlen(buffer), 0);
    bzero(buffer, MAX_BYTES);

    //-1 for terminator "\0"
    bytes_send=recv(remote_socketId, buffer, MAX_BYTES-1, 0);
    char* temp_buffer=(char*)malloc(sizeof(char)*MAX_BYTES);
    int temp_buffer_size=MAX_BYTES;
    int temp_buffer_index=0;

    while(bytes_send>0){
        //send what we recieved to requested socket
        bytes_send=send(client_socketId, buffer, bytes_send, 0);
        
        //copy from buffer to temp, as needed to store in cache_element since buffer will be set to zero
        for(int i=0;i<bytes_send/sizeof(char);i++){
            temp_buffer[temp_buffer_index]=buffer[i];
            temp_buffer_index++;
        }

        //increase temp buffer size for good measure
        temp_buffer_size+=MAX_BYTES;
        temp_buffer=(char*)realloc(temp_buffer, temp_buffer_size);

        if(bytes_send<0){
            printf("Error sending data to client\n");
            break;
        }

        bzero(buffer, MAX_BYTES);
        bytes_send=recv(remote_socketId, buffer, MAX_BYTES-1, 0);
    }

    temp_buffer[temp_buffer_index]='\0';
    free(buffer);
    add_cache_element(temp_buffer, strlen(temp_buffer), temp,request);
    printf("Done\n");
    free(temp_buffer);
    close(remote_socketId);

    return 0;
}

int checkHTTPversion(char* msg){
    int v=-1;

    if(strncmp(msg, "HTTP/1.1", 8)==0)
        v=1;
    else if(strncmp(msg, "HTTP/1.0", 8)==0)
        v=1;
    else   
        v=-1;

    return v;
}   

void* thread_fn(void* client_socketId){
    //obtain semaphore lock
    sem_wait(&semaphore);
    int p;
    sem_getvalue(&semaphore, &p);
    printf("Number of clients (Semaphore value): %d\n", p);

    int* t= (int*) (client_socketId);
    int socket = *t; //typecasting void
    int client_bytes, len;  

    char *buffer=(char*)calloc(MAX_BYTES, sizeof(char));
    bzero(buffer, MAX_BYTES);
    //recieve data from socket, >0 recieving, 0 done, -1 error
    //0 default, can be peek, wait fully before returning
    client_bytes=recv(socket, buffer, MAX_BYTES, 0);

    while(client_bytes>0){
        len = strlen(buffer);

        //strstr find substring in string
        //"\r" used to move cursor to next line, carriage return 
        if(strstr(buffer, "\r\n\r\n")==NULL)
            client_bytes=recv(socket, buffer+len, MAX_BYTES-len, 0); 
        else
            break;
    }

    printf("\n---------------Request----------------------------\n");
	printf("%s",buffer);
	printf("\n--------------------------------------------\n");

    //copy of buffer, good coding practice
    char* reqCopy=(char*)malloc(strlen(buffer)*sizeof(char)+1);
    for(int i=0;i<strlen(buffer);i++){
        reqCopy[i]=buffer[i];
    }
        
    struct cache_element* temp=find(reqCopy);
    if(temp!=NULL){
        int size=temp->len/sizeof(char);
        int pos=0;
        char response[MAX_BYTES];
        while(pos<size){
            bzero(response, MAX_BYTES);
            for(int i=0;i<MAX_BYTES;i++){
                response[i]=temp->data[pos];
                pos++;
            }
            //send(socket, response, MAX_BYTES, 0);
        }
        printf("Data retrived from cache_element\n\n");
        printf("%s\n\n", response);

        ParsedRequest* request = ParsedRequest_create();
        if (ParsedRequest_parse(request, buffer, len) < 0) {
            printf("Error parsing request\n");
        } else {
            // Forward request to actual server and handle the response
            if (!strcmp(request->method, "GET")) {
                if (request->host && request->path && checkHTTPversion(request->version) == 1) {
                    client_bytes = handle_request(socket, request, reqCopy);
                    if (client_bytes == -1) {
                        send_error(socket, 500);
                    }
                } else {
                    send_error(socket, 500);
                }
            }else {
                printf("Only GET for HTTP 1.0 is implemented till now\n"); 
            }
        }
        ParsedRequest_destroy(request);
    }else if(client_bytes>0){
        len=strlen(buffer);
        //has struct where we can store request header 
        ParsedRequest* request= ParsedRequest_create();

        //parsing, breaking it down and storing in request 
        if(ParsedRequest_parse(request, buffer, len)<0){
            printf("Error parsing request\n");
        }else{
            bzero(buffer, MAX_BYTES);
            //if true strcmp returns 0
            if(!strcmp(request->method, "GET")){
                if(request->host && request->path && checkHTTPversion(request->version)==1){
                    client_bytes=handle_request(socket, request, reqCopy);
                    if(client_bytes==-1)
                        send_error(socket, 500);
                }else{
                    send_error(socket, 500);
                }
            }else {
                printf("Only GET for HTTP 1.0 is implemented till now\n"); 
            }
        }
        ParsedRequest_destroy(request);
    }else if(client_bytes<0){
        perror("Error in reciving from client\n");
    }else if(client_bytes==0){
        printf("Client disconnected\n");
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &p);
    printf("Number of clients (Semaphore value): %d\n", p);
    free(reqCopy);

    return NULL;
} 


int main(int argc, char* argv[]){
    if(argc == 2) 
        port = atoi(argv[1]);
    else{
        perror("Too few arguments");
        exit(1);
    }
    //printf("Starting proxy server on port %d\n", port);

    if(sem_init(&semaphore, 0, MAX_CLIENTS)!=0){
        perror("Semaphore initialisation failed");
        exit(1);
    }

    if(pthread_mutex_init(&mutex, NULL)!=0){
        perror("Mutex initialisation failed");
        exit(1);
    }

    printf("Semaphore and mutex initialised\n");

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0); //creating the server socket

    if(proxy_socketId < 0) {
        printf("Error creating socket\n");
        exit(1);
    }

    int reuse=1;
    //setting the socket option
    //where to set, at which level to set (socket, tcp, ip), reuse dont block, reuse
    if(setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt failed\n");
    }else{
        printf("Server Socket set up succesfully!\n");
    }

    //Store info about our server socket
    struct sockaddr_in server_address;

    bzero((char*) &server_address, sizeof(server_address)); //change garbage data to 0
    //sin -> socket address
    server_address.sin_family = AF_INET; 
    server_address.sin_addr.s_addr = INADDR_ANY; //accept any incoming messages 
    server_address.sin_port = htons(port); //convert to network byte order big endian 

    if(bind(proxy_socketId, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        perror("Port is not free\n");
        exit(1);
    }
    
    printf("Proxy server started on port %d\n\n", port);
    int listen_status = listen(proxy_socketId, MAX_CLIENTS); //listen for incoming connections

    if(listen_status < 0) {
        perror("Error listening\n");
        exit(1);
    }

    int i=0, client_len;
    struct sockaddr client_address;
    int connected_socketID[MAX_CLIENTS];

    while(1){
        //accept the connection (blocking)
        bzero((char*) &client_address, sizeof(client_address)); //zero out the address block
        client_len = sizeof(client_address); //size of client address structure
        connected_socketID[i] = accept(proxy_socketId, (struct sockaddr*)&client_address, (socklen_t*)&client_len);
        if(connected_socketID[i] < 0) {
            perror("Error accepting connection\n");
            exit(1);
        }
        
        struct sockaddr_in* client_ptr=(struct sockaddr_in*)&client_address; //converting sockaddr
        struct in_addr ip_addr=client_ptr->sin_addr; //struct of 32 bit IP address
        char str[INET_ADDRSTRLEN]; //len of inet address length
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN); //convert IP to human readable form 
        printf("Client connect at port %d with IP %s\n", ntohs(client_ptr->sin_port), str); //big to little endian

        ///where to store, attributes (null=defualt), function to execute when thread is created, arg to pass to func
        pthread_create(&threadId[i], NULL, thread_fn, (void*)&connected_socketID[i]); 
        i++;
    }

    close(proxy_socketId);
    return 0;
}

cache_element* find(char* url){
    cache_element* ele=NULL;

    int temp_lock=pthread_mutex_lock(&mutex);
    printf("cache_element lock acquired %d\n", temp_lock);

    if(head!=NULL){
        ele=head;
        while(ele!=NULL){
            if(!strcmp(ele->url, url)){
                printf("\nURL found\n");
                printf("LRU time before %ld", ele->time);
                ele->time=time(NULL);
                printf("LRU time after %ld", ele->time);
                break;
            }
            ele=ele->next;
        }
    }else{
        printf("\nURL not found in cache_element\n");
    }

    temp_lock=pthread_mutex_unlock(&mutex);
    printf("cache_element lock released %d\n", temp_lock);
    return ele;
}

int add_cache_element(char* data, int len, char* url, ParsedRequest *request) {
    // Check the Content-Encoding header to decide if decompression is needed
    char *content_encoding = (char*)ParsedHeader_get(request, "Content-Encoding");

    if (content_encoding != NULL) {
        char* decompressed_data = NULL;
        int decompressed_len = 0;

        // If the content encoding is gzip or deflate, decompress the data
        if (strcmp(content_encoding, "gzip") == 0 || strcmp(content_encoding, "deflate") == 0) {
            if (decompress_data(data, len, &decompressed_data, &decompressed_len) != 0) {
                printf("Error decompressing data\n");
                return -1;
            }

            // Replace the original data with decompressed data
            free(data);  // Free the original compressed data
            data = decompressed_data;
            len = decompressed_len;
        }

        // Remove the Content-Encoding header as it is no longer needed after decompression
        ParsedHeader_remove(request, "Content-Encoding");
    }

    // Lock the cache mutex to ensure thread-safety while modifying the cache
    int temp_lock = pthread_mutex_lock(&mutex);
    printf("Add cache_element lock acquired %d\n", temp_lock);

    // Check if the cache size exceeds the maximum allowed size
    int ele_size = len + 1 + strlen(url) + sizeof(cache_element);
    if (ele_size > MAX_ELEMENT_SIZE) {
        pthread_mutex_unlock(&mutex); // Release lock if size exceeds limit
        printf("Cache size exceeded\n");
        return 0;
    } else {
        // Make room for new cache elements if necessary
        while (cache_element_size + ele_size > MAX_SIZE) {
            remove_cache_element();  // Remove least recently used cache element
        }

        // Create and populate a new cache element
        cache_element* element = (cache_element*)malloc(sizeof(cache_element));
        element->data = (char*)malloc(len + 1);
        strcpy(element->data, data);  // Store the decompressed data
        element->url = (char*)malloc(strlen(url) + 1);
        strcpy(element->url, url);    // Store the URL
        element->len = len;
        element->time = time(NULL);
        element->next = head;         // Insert the new element at the head of the cache
        head = element;
        cache_element_size += ele_size;

        // Optionally print cache info
        /*
        printf("\n\nData added to cache\n");
        printf("URL: %s\n", element->url);
        printf("Length: %d\n", element->len);
        printf("Time: %ld\n", element->time);
        */

        // Release lock after modifying the cache
        pthread_mutex_unlock(&mutex);
        return 1;
    }
}

void remove_cache_element(){
    cache_element *p, *q, *temp;
    int temp_lock=pthread_mutex_lock(&mutex);
    printf("Remove cache_element lock acquired %d\n", temp_lock);

    if(head!=NULL){
        for(q=head, p=head, temp=head;q->next!=NULL;q=q->next){
            if(((q->next)->time)<(temp->time)){
                temp=q->next;
                p=q;
            }
        }

        if(temp==head)
            head=head->next;
        else   
            p->next=temp->next;

        cache_element_size=cache_element_size-(temp->len)-sizeof(cache_element)-strlen(temp->url)-1;
        free(temp->data);
        free(temp->url);
        free(temp);
    }

    temp_lock=pthread_mutex_unlock(&mutex);
    printf("Remove cache_element lock released %d\n", temp_lock);
}
