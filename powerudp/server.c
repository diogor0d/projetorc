#include "msg_structs.h" // ConfigMessage is defined here
#include "powerudp.h"    // For constants like PSK_DEFAULT, MULTICAST_ADDRESS, etc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN    "\x1b[36m"
#define RESET   "\x1b[0m"
#define BOLD    "\x1b[1m"

#define MAX_CLIENTS 10
#define SERVER_LOG_PREFIX "\x1b[34m[Servidor]\x1b[0m "

// Structure to store information about a registered client
typedef struct
{
    int socket_fd;
    struct sockaddr_in address;
    pthread_t thread_id;
    int active;
    time_t registration_time;
} client_info_t;

// List of registered clients and global configuration
static client_info_t registered_clients[MAX_CLIENTS];
static int num_registered_clients = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global PowerUDP configuration, using the typedef 'ConfigMessage'
static ConfigMessage current_global_config; // Correct: Use typedef 'ConfigMessage'
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

static char server_psk[MAX_PSK_LEN];
static int multicast_sock = -1;
static struct sockaddr_in multicast_addr_send;

// sigint
volatile sig_atomic_t sigint_received_server = 0;
static int listen_fd_signal_handler = -1;

void broadcast_shutdown_signal_multicast()
{
    if (multicast_sock < 0)
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Socket multicast não inicializado. Não é possível difundir sinal de shutdown.\n");
        return;
    }

    ConfigMessage shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(ConfigMessage)); // Initialize all fields to 0/false
    shutdown_msg.server_shutdown_signal = 1;         // Signal server shutdown
    // Other fields can remain 0 or be set to specific "disabled" values if desired

    ssize_t bytes_sent = sendto(multicast_sock, &shutdown_msg, sizeof(ConfigMessage), 0,
                                (struct sockaddr *)&multicast_addr_send, sizeof(multicast_addr_send));

    if (bytes_sent < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao enviar sinal de shutdown via multicast");
    }
    else if (bytes_sent != sizeof(ConfigMessage))
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Erro: tamanho incorreto enviado para sinal de shutdown via multicast (%zd vs %zu bytes).\n",
                bytes_sent, sizeof(ConfigMessage));
    }
    else
    {
        printf(SERVER_LOG_PREFIX "Sinal de shutdown difundido via multicast para %s:%d.\n",
               inet_ntoa(multicast_addr_send.sin_addr), ntohs(multicast_addr_send.sin_port));
    }
}

void handle_sigint_server(int sig)
{
    (void)sig; // Unused parameter
    // Using printf as per previous discussions, though write is safer for async-signal operations.
    printf("\n" SERVER_LOG_PREFIX "SIGINT recebido. A encerrar...\n");

    // Broadcast shutdown signal to clients FIRST
    broadcast_shutdown_signal_multicast();

    sigint_received_server = 1;

    if (listen_fd_signal_handler != -1)
    {
        close(listen_fd_signal_handler);
        listen_fd_signal_handler = -1;
    }
}

// Function to initialize the client list
void init_client_list()
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        registered_clients[i].active = 0;
        registered_clients[i].socket_fd = -1;
    }
    num_registered_clients = 0;
    pthread_mutex_unlock(&clients_mutex);
}

// Adds a client to the list
// Returns the index in the list or -1 if full
int add_client(int client_socket, struct sockaddr_in client_address, pthread_t tid)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!registered_clients[i].active)
        {
            registered_clients[i].active = 1;
            registered_clients[i].socket_fd = client_socket;
            registered_clients[i].address = client_address;
            registered_clients[i].thread_id = tid;
            registered_clients[i].registration_time = time(NULL);
            num_registered_clients++;
            printf(SERVER_LOG_PREFIX "Cliente %s%s:%d%s adicionado. Total: %d.\n",
                   BLUE, inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port), RESET, num_registered_clients);
            pthread_mutex_unlock(&clients_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    fprintf(stderr, SERVER_LOG_PREFIX "Lista de clientes cheia. Não foi possível adicionar novo cliente.\n");
    return -1;
}

// Removes a client from the list by their socket descriptor
void remove_client_by_socket(int client_socket)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (registered_clients[i].active && registered_clients[i].socket_fd == client_socket)
        {
            registered_clients[i].active = 0;
            // Note: The socket itself is typically closed in the thread or accept loop that detected the issue.
            // Avoid closing it here again if it's already handled to prevent "bad file descriptor".
            registered_clients[i].socket_fd = -1; // Mark as invalid
            num_registered_clients--;
            printf(SERVER_LOG_PREFIX "Cliente %s%s:%d%s removido. Total: %d.\n",
                   BLUE, inet_ntoa(registered_clients[i].address.sin_addr),
                   ntohs(registered_clients[i].address.sin_port), RESET,
                   num_registered_clients);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Function to broadcast the current configuration to ALL registered clients via multicast
void broadcast_config_multicast()
{
    if (multicast_sock < 0)
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Socket multicast não inicializado. Não é possível difundir config.\n");
        return;
    }

    ConfigMessage msg_to_send; // Correct: Use typedef 'ConfigMessage'

    pthread_mutex_lock(&config_mutex);
    // Copy from current_global_config.
    // current_global_config.base_timeout is in host byte order.
    // Convert to network byte order for sending.
    msg_to_send.enable_retrans = current_global_config.enable_retrans;
    msg_to_send.enable_backoff = current_global_config.enable_backoff;
    msg_to_send.enable_seq = current_global_config.enable_seq;
    msg_to_send.base_timeout = htons(current_global_config.base_timeout); // Correct field: base_timeout
    msg_to_send.max_retries = current_global_config.max_retries;
    pthread_mutex_unlock(&config_mutex);

    ssize_t bytes_sent = sendto(multicast_sock, &msg_to_send, sizeof(ConfigMessage), 0, // Correct: sizeof(ConfigMessage)
                                (struct sockaddr *)&multicast_addr_send, sizeof(multicast_addr_send));

    if (bytes_sent < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao enviar configuração via multicast");
    }
    else if (bytes_sent != sizeof(ConfigMessage))
    { // Correct: sizeof(ConfigMessage)
        fprintf(stderr, SERVER_LOG_PREFIX "Erro: tamanho incorreto enviado via multicast (%zd vs %zu bytes).\n",
                bytes_sent, sizeof(ConfigMessage)); // Correct: sizeof(ConfigMessage)
    }
    else
    {
        printf(SERVER_LOG_PREFIX "Nova configuração difundida via multicast para %s%s:%d%s.\n",
               BLUE, inet_ntoa(multicast_addr_send.sin_addr), ntohs(multicast_addr_send.sin_port), RESET);
    }
}

// Thread function to handle each TCP client
void *client_handler_thread(void *arg)
{
    int client_sock = *((int *)arg);
    free(arg); // Free the dynamically allocated socket descriptor

    // Determine the larger size for the buffer between RegisterMessage and ConfigMessage
    size_t buffer_alloc_size = sizeof(struct RegisterMessage) > sizeof(ConfigMessage) ? sizeof(struct RegisterMessage) : sizeof(ConfigMessage);
    char *buffer = malloc(buffer_alloc_size);
    if (!buffer)
    {
        perror(SERVER_LOG_PREFIX "malloc for client buffer failed");
        close(client_sock);                   // Close socket before exiting thread
        remove_client_by_socket(client_sock); // Attempt to remove if it was added
        return NULL;
    }

    ssize_t bytes_received;
    int client_authenticated = 0;
    struct sockaddr_in client_addr_info;
    socklen_t addr_len = sizeof(client_addr_info);

    // Get client's address information for logging
    if (getpeername(client_sock, (struct sockaddr *)&client_addr_info, &addr_len) != 0)
    {
        perror(SERVER_LOG_PREFIX "getpeername failed for client socket");
        close(client_sock);
        remove_client_by_socket(client_sock);
        free(buffer);
        return NULL;
    }

    // 1. Wait for RegisterMessage and Authenticate
    bytes_received = recv(client_sock, buffer, sizeof(struct RegisterMessage), 0);
    if (bytes_received <= 0)
    {
        if (bytes_received == 0)
        {
            printf(SERVER_LOG_PREFIX "Cliente %s:%d desconectou-se antes do registo.\n", inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port));
        }
        else
        {
            perror(SERVER_LOG_PREFIX "Erro ao receber mensagem de registo");
        }
        close(client_sock);
        remove_client_by_socket(client_sock);
        free(buffer);
        return NULL;
    }

    if (bytes_received == sizeof(struct RegisterMessage))
    {
        struct RegisterMessage *reg_msg = (struct RegisterMessage *)buffer;
        reg_msg->psk[MAX_PSK_LEN - 1] = '\0'; // Ensure null-termination
        if (strcmp(reg_msg->psk, server_psk) == 0)
        {
            client_authenticated = 1;
            printf(SERVER_LOG_PREFIX "Cliente %s%s:%d%s autenticado %scom sucesso%s.\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET, GREEN, RESET);
        }
        else
        {
            fprintf(stderr, SERVER_LOG_PREFIX "Falha na autenticação do cliente %s%s:%d%s (PSK inválida: '%s').\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET, reg_msg->psk);
        }
    }
    else
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Mensagem de registo com tamanho inválido (%zd bytes) de %s%s:%d%s.\n", bytes_received, BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
    }

    if (!client_authenticated)
    {
        close(client_sock);
        remove_client_by_socket(client_sock);
        free(buffer);
        return NULL;
    }

    // Client authenticated, now can receive configuration requests
    while (1)
    {
        memset(buffer, 0, buffer_alloc_size);
        bytes_received = recv(client_sock, buffer, sizeof(ConfigMessage), 0); // Expect ConfigMessage

        if (bytes_received <= 0)
        {
            if (bytes_received == 0)
            {
                printf(SERVER_LOG_PREFIX "Cliente %s:%d desconectou-se.\n", inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port));
            }
            else
            {
                perror(SERVER_LOG_PREFIX "Erro ao receber do cliente");
            }
            break; // Exit loop, thread will terminate
        }

        if (bytes_received == sizeof(ConfigMessage))
        {                                                         // Correct: sizeof(ConfigMessage)
            ConfigMessage *req_cfg_msg = (ConfigMessage *)buffer; // Correct: Use typedef 'ConfigMessage'
            printf(SERVER_LOG_PREFIX "Recebido pedido de alteração de configuração de %s:%d.\n", inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port));

            pthread_mutex_lock(&config_mutex);
            // Update global configuration.
            // req_cfg_msg->base_timeout is in network byte order from client.
            // Convert to host byte order for storage in current_global_config.
            current_global_config.enable_retrans = req_cfg_msg->enable_retrans;
            current_global_config.enable_backoff = req_cfg_msg->enable_backoff;
            current_global_config.enable_seq = req_cfg_msg->enable_seq;
            current_global_config.base_timeout = ntohs(req_cfg_msg->base_timeout); // Correct field: base_timeout
            current_global_config.max_retries = req_cfg_msg->max_retries;
            pthread_mutex_unlock(&config_mutex);

            printf(SERVER_LOG_PREFIX "Configuração global atualizada:\n");
            // Display current_global_config (base_timeout is now in host order)
            printf("  Retransmissão: %s\n", current_global_config.enable_retrans ? "Ativada" : "Desativada");
            printf("  Backoff: %s\n", current_global_config.enable_backoff ? "Ativada" : "Desativada");
            printf("  Sequência: %s\n", current_global_config.enable_seq ? "Ativada" : "Desativada");
            printf("  Timeout Base: %u ms\n", current_global_config.base_timeout); // Correct field
            printf("  Max Retries: %u\n", current_global_config.max_retries);

            // Broadcast the new configuration to all clients
            broadcast_config_multicast();
        }
        else
        {
            fprintf(stderr, SERVER_LOG_PREFIX "Recebida mensagem TCP de %s:%d com tamanho inesperado (%zd bytes) para ConfigMessage.\n",
                    inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), bytes_received);
        }
    }

    // Cleanup when client disconnects or error occurs
    close(client_sock);
    remove_client_by_socket(client_sock); // Remove client from the list
    printf(SERVER_LOG_PREFIX "Thread para cliente %s:%d terminada.\n", inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port));
    free(buffer);
    return NULL;
}

void print_server_usage(const char *prog_name)
{
    printf("Uso: %s <Porta_TCP_Servidor> [PSK]\n", prog_name);
    printf("  PSK (opcional): Chave pré-partilhada para autenticação dos clientes. Padrão: \"%s\"\n", PSK_DEFAULT);
    printf("Exemplo: %s %d MySecretServerPSK\n", prog_name, SERVER_TCP_PORT);
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3)
    {
        print_server_usage(argv[0]);
        return 1;
    }
    signal(SIGINT, SIG_IGN); // previnir termino inseguro do processo

    int server_tcp_port_main = atoi(argv[1]);
    const char *psk_param = (argc == 3) ? argv[2] : PSK_DEFAULT;
    strncpy(server_psk, psk_param, MAX_PSK_LEN - 1);
    server_psk[MAX_PSK_LEN - 1] = '\0'; // Ensure null-termination

    printf(SERVER_LOG_PREFIX "A iniciar na porta TCP %s%d%s...\n", YELLOW, server_tcp_port_main, RESET);
    printf(SERVER_LOG_PREFIX "PSK do Servidor: %s%s%s\n", YELLOW, server_psk, RESET);
    printf(SERVER_LOG_PREFIX "Grupo Multicast para Config: %s%s:%d%s\n", BLUE, MULTICAST_ADDRESS, MULTICAST_PORT, RESET);

    // Initialize global configuration (base_timeout stored in host order)
    pthread_mutex_lock(&config_mutex);
    current_global_config.enable_retrans = 1;
    current_global_config.enable_backoff = 1;
    current_global_config.enable_seq = 1;
    current_global_config.base_timeout = 1000; // Correct field: base_timeout (host order)
    current_global_config.max_retries = 3;
    pthread_mutex_unlock(&config_mutex);
    printf(SERVER_LOG_PREFIX "Configuração PowerUDP inicial padrão definida.\n");

    init_client_list();

    // Configure TCP listening socket
    int listen_fd;
    struct sockaddr_in serv_addr_tcp_main;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao criar socket de escuta TCP");
        return 1;
    }

    int opt = 1; // For SO_REUSEADDR
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0)
    {
        perror(SERVER_LOG_PREFIX "setsockopt SO_REUSEADDR falhou");
        // Continue anyway
    }

    memset(&serv_addr_tcp_main, 0, sizeof(serv_addr_tcp_main));
    serv_addr_tcp_main.sin_family = AF_INET;
    serv_addr_tcp_main.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
    serv_addr_tcp_main.sin_port = htons(server_tcp_port_main);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr_tcp_main, sizeof(serv_addr_tcp_main)) < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro no bind do socket de escuta TCP");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, MAX_CLIENTS) < 0)
    { // Queue for pending connections
        perror(SERVER_LOG_PREFIX "Erro no listen do socket TCP");
        close(listen_fd);
        return 1;
    }
    printf(SERVER_LOG_PREFIX "A escutar por conexões TCP na porta %s%d%s.\n", YELLOW, server_tcp_port_main, RESET);

    // Configure Multicast socket for sending
    multicast_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast_sock < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao criar socket multicast");
        close(listen_fd);
        return 1;
    }

    // Optional: Configure TTL for multicast if needed to cross routers
    int mc_ttl = 5; // Default is 1 (same subnet). Increase if routers are involved.
    if (setsockopt(multicast_sock, IPPROTO_IP, IP_MULTICAST_TTL, &mc_ttl, sizeof(mc_ttl)) < 0)
    {
        perror(SERVER_LOG_PREFIX "Aviso: não foi possível definir TTL multicast");
    }

    memset(&multicast_addr_send, 0, sizeof(multicast_addr_send));
    multicast_addr_send.sin_family = AF_INET;
    multicast_addr_send.sin_port = htons(MULTICAST_PORT);
    if (inet_pton(AF_INET, MULTICAST_ADDRESS, &multicast_addr_send.sin_addr) <= 0)
    {
        perror(SERVER_LOG_PREFIX "Erro inet_pton para endereço multicast");
        close(listen_fd);
        close(multicast_sock);
        return 1;
    }
    printf(SERVER_LOG_PREFIX "Socket multicast configurado para enviar para %s%s:%d%s.\n", BLUE, MULTICAST_ADDRESS, MULTICAST_PORT, RESET);

    // Broadcast initial configuration via multicast when server starts
    broadcast_config_multicast();

    listen_fd_signal_handler = listen_fd;
    signal(SIGINT, handle_sigint_server); // tratar SIGINT

    // Main loop to accept new client connections
    while (!sigint_received_server)
    {
        struct sockaddr_in client_addr_tcp_loop;
        socklen_t client_len = sizeof(client_addr_tcp_loop);
        int new_client_sock = accept(listen_fd, (struct sockaddr *)&client_addr_tcp_loop, &client_len);

        if (new_client_sock < 0)
        {
            perror(SERVER_LOG_PREFIX "Erro ao aceitar conexão TCP");
            continue; // Try to accept next connection
        }

        printf(SERVER_LOG_PREFIX "Nova conexão TCP de %s%s:%d%s.\n",
               BLUE, inet_ntoa(client_addr_tcp_loop.sin_addr), ntohs(client_addr_tcp_loop.sin_port), RESET);

        pthread_mutex_lock(&clients_mutex);
        int current_client_count = num_registered_clients; // Read while locked
        pthread_mutex_unlock(&clients_mutex);

        if (current_client_count >= MAX_CLIENTS)
        {
            fprintf(stderr, SERVER_LOG_PREFIX "Máximo de clientes (%d) atingido. Rejeitando nova conexão de %s:%d.\n",
                    MAX_CLIENTS, inet_ntoa(client_addr_tcp_loop.sin_addr), ntohs(client_addr_tcp_loop.sin_port));
            close(new_client_sock);
            continue;
        }

        pthread_t tid;
        // Pass a dynamically allocated copy of the socket descriptor to the thread
        int *p_client_sock = malloc(sizeof(int));
        if (!p_client_sock)
        {
            perror(SERVER_LOG_PREFIX "Erro malloc para socket do cliente");
            close(new_client_sock);
            continue;
        }
        *p_client_sock = new_client_sock;

        if (pthread_create(&tid, NULL, client_handler_thread, (void *)p_client_sock) != 0)
        {
            perror(SERVER_LOG_PREFIX "Erro ao criar thread para o cliente");
            free(p_client_sock); // Free if thread creation failed
            close(new_client_sock);
            continue;
        }

        // Detach the thread so its resources are automatically reclaimed when it exits.
        // This server model does not explicitly join client threads.
        pthread_detach(tid);

        // Add client to the list. The client_handler_thread is responsible for removing
        // the client on authentication failure or disconnection.
        // add_client is internally mutex-protected.
        if (add_client(new_client_sock, client_addr_tcp_loop, tid) == -1)
        {
            // This scenario (list full after check) should be rare but possible in a race.
            // Or if add_client has other failure conditions.
            fprintf(stderr, SERVER_LOG_PREFIX "Não foi possível adicionar cliente à lista após aceitar e criar thread. Socket %d.\n", new_client_sock);
            // The thread will likely fail on recv and attempt to clean up.
            // Closing the socket here could be problematic if the thread has already started using it.
            // p_client_sock is now owned by the thread.
        }
    }

    // Cleanup (this code is not typically reached in an infinite loop server)
    close(listen_fd);
    if (multicast_sock != -1)
        close(multicast_sock);
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&config_mutex);
    printf(SERVER_LOG_PREFIX "Servidor a terminar.\n");
    return 0;
}
