/*
    Projeto de Redes de Comunicação 2024/2025 - PowerUDP
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#include "msg_structs.h"
#include "powerudp.h"

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

#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define YELLOW "\x1b[33m"
#define BLUE "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN "\x1b[36m"
#define RESET "\x1b[0m"
#define BOLD "\x1b[1m"

#define MAX_CLIENTS 10
#define SERVER_LOG_PREFIX "\x1b[34m[Servidor]\x1b[0m "

typedef struct
{
    int client_socket_fd;
    struct sockaddr_in client_address; // para aceder ao endereco do cliente na thread
} client_thread_args_t;

// estrutura para armazenar informações de um cliente
typedef struct
{
    int socket_fd;
    struct sockaddr_in address;
    pthread_t thread_id;
    int active;
    time_t registration_time;
} client_info_t;

static client_info_t registered_clients[MAX_CLIENTS];
static int num_registered_clients = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static ConfigMessage current_global_config;
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
    memset(&shutdown_msg, 0, sizeof(ConfigMessage)); // inicializa a estrutura
    shutdown_msg.server_shutdown_signal = 1;

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
        printf(SERVER_LOG_PREFIX "Sinal de shutdown difundido via multicast para %s%s:%d%s.\n",
               BLUE, inet_ntoa(multicast_addr_send.sin_addr), ntohs(multicast_addr_send.sin_port), RESET);
    }
}

void handle_sigint_server(int sig)
{
    (void)sig; // Unused parameter

    printf("\n" SERVER_LOG_PREFIX "SIGINT recebido. A encerrar...\n");

    broadcast_shutdown_signal_multicast();

    sigint_received_server = 1;

    if (listen_fd_signal_handler != -1)
    {
        close(listen_fd_signal_handler);
        listen_fd_signal_handler = -1;
    }
}
// inciializar lista de clientes
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

// adicionar um cliente a lista
// devolve o indice na lista ou -1 se cheia
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

// um client da lista atraves do seu fd
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

// funcao para transmitir a configuração atual para todos os clientes
void broadcast_config_multicast()
{
    if (multicast_sock < 0)
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Socket multicast não inicializado. Não é possível difundir config.\n");
        return;
    }

    ConfigMessage msg_to_send;
    memset(&msg_to_send, 0, sizeof(ConfigMessage));

    pthread_mutex_lock(&config_mutex);
    msg_to_send.enable_retrans = current_global_config.enable_retrans;
    msg_to_send.enable_backoff = current_global_config.enable_backoff;
    msg_to_send.enable_seq = current_global_config.enable_seq;
    msg_to_send.base_timeout = htons(current_global_config.base_timeout);
    msg_to_send.max_retries = current_global_config.max_retries;
    pthread_mutex_unlock(&config_mutex);

    ssize_t bytes_sent = sendto(multicast_sock, &msg_to_send, sizeof(ConfigMessage), 0,
                                (struct sockaddr *)&multicast_addr_send, sizeof(multicast_addr_send));

    if (bytes_sent < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao enviar configuração via multicast");
    }
    else if (bytes_sent != sizeof(ConfigMessage))
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Erro: tamanho incorreto enviado via multicast (%zd vs %zu bytes).\n",
                bytes_sent, sizeof(ConfigMessage));
    }
    else
    {
        printf(SERVER_LOG_PREFIX "Nova configuração difundida via multicast para %s%s:%d%s.\n",
               BLUE, inet_ntoa(multicast_addr_send.sin_addr), ntohs(multicast_addr_send.sin_port), RESET);
    }
}

// thread para tratar cada cliente tcp
void *client_handler_thread(void *arg)
{
    client_thread_args_t *thread_data = (client_thread_args_t *)arg;
    int client_sock = thread_data->client_socket_fd;
    struct sockaddr_in client_addr_info = thread_data->client_address;
    free(arg);

    // determinar o maior tamanho do buffer a alocar
    size_t buffer_alloc_size = sizeof(struct RegisterMessage) > sizeof(ConfigMessage) ? sizeof(struct RegisterMessage) : sizeof(ConfigMessage);
    char *buffer = malloc(buffer_alloc_size);
    if (!buffer)
    {
        perror(SERVER_LOG_PREFIX "malloc for client buffer failed");
        close(client_sock);
        return NULL;
    }

    ssize_t bytes_received;
    int client_authenticated = 0;

    // aguardar por mensagem de registo do cliente
    bytes_received = recv(client_sock, buffer, sizeof(struct RegisterMessage), 0);
    if (bytes_received <= 0)
    {
        if (bytes_received == 0)
        {
            printf(SERVER_LOG_PREFIX "Cliente %s%s:%d%s desconectou-se antes do registo.\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
        }
        else
        {
            perror(SERVER_LOG_PREFIX "Erro ao receber mensagem de registo");
        }
        close(client_sock);
        free(buffer);
        return NULL;
    }

    uint8_t auth_response_status = 0;

    if (bytes_received == sizeof(struct RegisterMessage))
    {
        struct RegisterMessage *reg_msg = (struct RegisterMessage *)buffer;
        reg_msg->psk[MAX_PSK_LEN - 1] = '\0';
        if (strcmp(reg_msg->psk, server_psk) == 0)
        {
            client_authenticated = 1;
            auth_response_status = 1;
            printf(SERVER_LOG_PREFIX "Cliente %s%s:%d%s autenticado %scom sucesso%s.\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET, GREEN, RESET);
        }
        else
        {
            auth_response_status = 0;
            fprintf(stderr, SERVER_LOG_PREFIX "Falha na autenticação do cliente %s%s:%d%s (PSK inválida: '%s').\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET, reg_msg->psk);
        }
    }
    else
    {
        auth_response_status = 0;
        fprintf(stderr, SERVER_LOG_PREFIX "Mensagem de registo com tamanho inválido (%zd bytes) de %s%s:%d%s.\n", bytes_received, BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
    }

    // enviar resposta de autenticacao
    if (send(client_sock, &auth_response_status, sizeof(auth_response_status), 0) < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao enviar resposta de autenticação para o cliente");

        if (!client_authenticated)
        {
            close(client_sock);
            free(buffer);
            return NULL;
        }
    }

    if (!client_authenticated)
    {
        close(client_sock);
        free(buffer);
        return NULL;
    }

    // cliente autenticado
    if (add_client(client_sock, client_addr_info, pthread_self()) == -1)
    {
        fprintf(stderr, SERVER_LOG_PREFIX "Falha ao adicionar cliente autenticado %s%s:%d%s à lista. Fechando conexão.\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
        close(client_sock);
        free(buffer);
        return NULL;
    }

    // receber mensagens de configuração do cliente (apos autenticação)
    while (1)
    {
        bytes_received = recv(client_sock, buffer, sizeof(ConfigMessage), 0);

        if (bytes_received <= 0)
        {
            if (bytes_received == 0)
            {
                printf(SERVER_LOG_PREFIX "Cliente %s%s:%d%s desconectou-se (conexão TCP fechada).\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
            }
            else
            {
                perror(SERVER_LOG_PREFIX "Erro ao receber dados do cliente (TCP)");
            }
            break;
        }

        if (bytes_received == sizeof(ConfigMessage))
        {
            ConfigMessage *new_config_msg = (ConfigMessage *)buffer;

            if (new_config_msg->server_shutdown_signal == 1)
            {
                fprintf(stderr, SERVER_LOG_PREFIX "Cliente %s%s:%d%s enviou pedido de config com sinal de shutdown. Ignorando.\n",
                        BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
                continue;
            }

            pthread_mutex_lock(&config_mutex);
            current_global_config.enable_retrans = new_config_msg->enable_retrans;
            current_global_config.enable_backoff = new_config_msg->enable_backoff;
            current_global_config.enable_seq = new_config_msg->enable_seq;
            current_global_config.base_timeout = ntohs(new_config_msg->base_timeout);
            current_global_config.max_retries = new_config_msg->max_retries;
            pthread_mutex_unlock(&config_mutex);

            printf(SERVER_LOG_PREFIX "Configuração atualizada por %s%s:%d%s:\n",
                   BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
            printf("  Retransmissão: %s, Backoff: %s, Sequência: %s, Timeout: %u ms, Retries: %u\n",
                   current_global_config.enable_retrans ? "Ativada" : "Desativada",
                   current_global_config.enable_backoff ? "Ativada" : "Desativada",
                   current_global_config.enable_seq ? "Ativada" : "Desativada",
                   current_global_config.base_timeout,
                   current_global_config.max_retries);

            broadcast_config_multicast(); // transmitir nova configuração para todos os clientes
        }
        else
        {
            fprintf(stderr, SERVER_LOG_PREFIX "Recebida mensagem TCP de tamanho inesperado (%zd bytes) de %s%s:%d%s. Esperado %zu bytes para ConfigMessage. Ignorando.\n",
                    bytes_received, BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET, sizeof(ConfigMessage));
        }
    }

    // limpeza
    close(client_sock);
    remove_client_by_socket(client_sock); // remover cliente da lista
    printf(SERVER_LOG_PREFIX "Thread para cliente %s%s:%d%s terminada.\n", BLUE, inet_ntoa(client_addr_info.sin_addr), ntohs(client_addr_info.sin_port), RESET);
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
    server_psk[MAX_PSK_LEN - 1] = '\0';

    printf(SERVER_LOG_PREFIX "A iniciar na porta TCP %s%d%s...\n", YELLOW, server_tcp_port_main, RESET);
    printf(SERVER_LOG_PREFIX "PSK do Servidor: %s%s%s\n", YELLOW, server_psk, RESET);
    printf(SERVER_LOG_PREFIX "Grupo Multicast para Config: %s%s:%d%s\n", BLUE, MULTICAST_ADDRESS, MULTICAST_PORT, RESET);

    // inicializar configuracao default
    pthread_mutex_lock(&config_mutex);
    current_global_config.enable_retrans = 1;
    current_global_config.enable_backoff = 1;
    current_global_config.enable_seq = 1;
    current_global_config.base_timeout = 1000;
    current_global_config.max_retries = 3;
    pthread_mutex_unlock(&config_mutex);
    printf(SERVER_LOG_PREFIX "Configuração PowerUDP inicial padrão definida.\n");

    init_client_list();

    // configurar socket tcp
    int listen_fd;
    struct sockaddr_in serv_addr_tcp_main;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao criar socket de escuta TCP");
        return 1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0)
    {
        perror(SERVER_LOG_PREFIX "setsockopt SO_REUSEADDR falhou");
    }

    memset(&serv_addr_tcp_main, 0, sizeof(serv_addr_tcp_main));
    serv_addr_tcp_main.sin_family = AF_INET;
    serv_addr_tcp_main.sin_addr.s_addr = htonl(INADDR_ANY); // ouvir em todas as interfaces
    serv_addr_tcp_main.sin_port = htons(server_tcp_port_main);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr_tcp_main, sizeof(serv_addr_tcp_main)) < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro no bind do socket de escuta TCP");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, MAX_CLIENTS) < 0)
    { // fila de espera para conexões
        perror(SERVER_LOG_PREFIX "Erro no listen do socket TCP");
        close(listen_fd);
        return 1;
    }
    printf(SERVER_LOG_PREFIX "A escutar por conexões TCP na porta %s%d%s.\n", YELLOW, server_tcp_port_main, RESET);

    // configurar socket multicast para enviar mensagens de configuração
    multicast_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast_sock < 0)
    {
        perror(SERVER_LOG_PREFIX "Erro ao criar socket multicast");
        close(listen_fd);
        return 1;
    }

    int mc_ttl = 5;
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

    // broadcast inicial ao iniciar o servidor
    broadcast_config_multicast();

    listen_fd_signal_handler = listen_fd;
    signal(SIGINT, handle_sigint_server); // tratar SIGINT novamente

    // loop principal para aceitar conexoes tcp
    while (!sigint_received_server)
    {
        struct sockaddr_in client_addr_tcp_loop;
        socklen_t client_len = sizeof(client_addr_tcp_loop);
        int new_client_sock = accept(listen_fd, (struct sockaddr *)&client_addr_tcp_loop, &client_len);

        if (new_client_sock < 0)
        {
            if (sigint_received_server)
                break;
            perror(SERVER_LOG_PREFIX "Erro ao aceitar conexão TCP");
            continue;
        }

        printf(SERVER_LOG_PREFIX "Nova conexão TCP de %s%s:%d%s.\n",
               BLUE, inet_ntoa(client_addr_tcp_loop.sin_addr), ntohs(client_addr_tcp_loop.sin_port), RESET);

        pthread_mutex_lock(&clients_mutex);
        int current_client_count = num_registered_clients;
        pthread_mutex_unlock(&clients_mutex);

        if (current_client_count >= MAX_CLIENTS)
        {
            fprintf(stderr, SERVER_LOG_PREFIX "Máximo de clientes (%d) atingido. Rejeitando nova conexão de %s%s:%d%s.\n",
                    MAX_CLIENTS, BLUE, inet_ntoa(client_addr_tcp_loop.sin_addr), ntohs(client_addr_tcp_loop.sin_port), RESET);
            close(new_client_sock);
            continue;
        }

        pthread_t tid;
        // passar uma copia alocada dinamicamente do socket e endereco para a thread
        client_thread_args_t *p_thread_args = malloc(sizeof(client_thread_args_t));
        if (!p_thread_args)
        {
            perror(SERVER_LOG_PREFIX "Erro malloc para argumentos da thread do cliente");
            close(new_client_sock);
            continue;
        }
        p_thread_args->client_socket_fd = new_client_sock;
        p_thread_args->client_address = client_addr_tcp_loop;

        if (pthread_create(&tid, NULL, client_handler_thread, (void *)p_thread_args) != 0)
        {
            perror(SERVER_LOG_PREFIX "Erro ao criar thread para o cliente");
            free(p_thread_args);
            close(new_client_sock);
            continue;
        }

        // libertar recursos da thread no fim da sua execucao
        pthread_detach(tid);
    }

    // cleanup
    close(listen_fd);
    if (multicast_sock != -1)
        close(multicast_sock);
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&config_mutex);
    printf(SERVER_LOG_PREFIX "Servidor a terminar.\n");
    return 0;
}