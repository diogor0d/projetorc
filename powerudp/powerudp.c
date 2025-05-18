/*
    Projeto de Redes de Comunicação 2024/2025 - PowerUDP
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#define _GNU_SOURCE
#include "powerudp.h"
#include "msg_structs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h> // struct ip_mreq é definida aqui com _GNU_SOURCE
#include <arpa/inet.h>
#include <sys/time.h> // para gettimeofday() e timers (select)
#include <pthread.h>  // para a thread do listener multicast
#include <time.h>     // para srand() e rand() na simulação de perda
#include <errno.h>    // para perror e errno
#include <signal.h>   // Para tratamento de sinais

#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define YELLOW "\x1b[33m"
#define BLUE "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN "\x1b[36m"
#define RESET "\x1b[0m"
#define BOLD "\x1b[1m"

static int udp_socket_internal = -1; // socket UDP principal para PowerUDP
static int server_tcp_socket = -1;   // socket TCP para comunicação com o servidor de configuração
static int multicast_socket = -1;    // socket para receber mensagens multicast de configuração

static struct sockaddr_in server_addr_tcp; // ip do servidor de configuração TCP
static struct sockaddr_in local_udp_addr;  // endereço do cliente PowerUDP
static char current_psk[MAX_PSK_LEN];

// estatisticas da ultima mensagem enviada
static int last_msg_retransmissions = 0;    // numero de retransmissões
static int last_msg_delivery_time_ms = -1;  // tempo de entrega em ms (-1 caso falha)
static int last_msg_attempt_successful = 0; // 1 se sucesso, 0 se falha
static int stats_are_valid_internal = 0;    // 1 se as estatísticas são de uma tentativa completa

// variaveis de estado do protocolo
static pthread_t multicast_thread_id;                    // threadid para o listener do multicast
static volatile int protocol_initialized = 0;            // 1= protocolo está inicializado, 0 caso contrário
static volatile int keep_multicast_listener_running = 0; // flag para controlar a execução da thread multicast
static volatile int server_is_shutting_down = 0;         // flag global para indicar se o fim da execucao do servidor

// configuracoes atuais do PowerUDP, a atualizar dinamicamente via multicast
static power_udp_config_t internal_power_udp_state;

void *multicast_listener_thread_func(void *arg)
{
    (void)arg;

    struct sockaddr_in sender_addr; // endereço do remetente da mensagem multicast
    socklen_t sender_addr_len = sizeof(sender_addr);
    // buffer para receber a mensagem multicast
    char buffer[sizeof(ConfigMessage) + 1];
    ssize_t bytes_received; // num de bytes recebidos

    printf("%s[PowerUDP]%s Multicast listener thread iniciada (Socket: %d).\n", RED, RESET, multicast_socket);

    // timeout para recvfrom no socket multicast permite que a thread verifique periodicamente a flag keep_multicast_listener_running
    struct timeval tv_mc_recv;
    tv_mc_recv.tv_sec = 1; // timeout de 1 segundo
    tv_mc_recv.tv_usec = 0;
    if (setsockopt(multicast_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_mc_recv, sizeof tv_mc_recv) < 0)
    {
        perror("[PowerUDP Warning] setsockopt SO_RCVTIMEO para multicast falhou");
    }

    // loop principal da thread: espera por mensagens multicast
    while (keep_multicast_listener_running)
    {
        memset(buffer, 0, sizeof(buffer)); // limpeza do buffer
        // esperar por uma mesnsagem no socket
        bytes_received = recvfrom(multicast_socket, buffer, sizeof(ConfigMessage), 0,
                                  (struct sockaddr *)&sender_addr, &sender_addr_len);

        // verificar se a thread deve terminar (apos o possivel periodo de bloqueio do recvfrom)
        if (!keep_multicast_listener_running)
        {
            break;
        }

        if (server_is_shutting_down)
        {
            break;
        }

        // tratar erro do recvfrom
        if (bytes_received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            { // ocorrencia de timeout
                continue;
            }
            // apresentar erro apenas se o protocolo estiver em funcionamento
            if (protocol_initialized && keep_multicast_listener_running)
            {
                perror("[PowerUDP Error] recvfrom multicast");
            }
            // prevenir timeout de forma simples
            if (errno != EINTR)
                usleep(100000);
            continue;
        }

        // verificar se o tamanho da mensagem recebida corresponde ao esperado de uma ConfigMessage
        if (bytes_received == sizeof(ConfigMessage))
        {
            ConfigMessage new_config_msg;
            memcpy(&new_config_msg, buffer, sizeof(ConfigMessage));

            // verificar o parametro caso o servidor tenha enviado o sinal de shutdown
            if (new_config_msg.server_shutdown_signal == 1)
            {
                printf("[PowerUDP] Recebido sinal de shutdown do servidor via multicast.\n");
                server_is_shutting_down = 1;
                keep_multicast_listener_running = 0; // parar a thread multicast

                // fechar o socket udp princiapl para desb;pqiear receive_message() na thread de rececao do cliente
                if (udp_socket_internal != -1)
                {
                    close(udp_socket_internal);
                }

                // para simplicidade enviar sigint para terminar o processo do cliente
                printf("[PowerUDP] Multicast listener enviando SIGINT para o processo principal...\n");
                if (kill(getpid(), SIGINT) != 0)
                {
                    perror("[PowerUDP Error] Multicast listener falhou ao enviar SIGINT");
                }
                break;
            }

            // aplicar a nova configuração.
            internal_power_udp_state.retransmission_enabled = new_config_msg.enable_retrans;
            internal_power_udp_state.backoff_enabled = new_config_msg.enable_backoff;
            internal_power_udp_state.sequence_enabled = new_config_msg.enable_seq;
            // ntohs converte de network byte order para host byte order
            internal_power_udp_state.base_timeout_ms = ntohs(new_config_msg.base_timeout);
            internal_power_udp_state.max_retries = new_config_msg.max_retries;

            // apresentar a configuração atualizada
            printf("%s[PowerUDP]%s Configuração atualizada via multicast:\n", RED, RESET);
            printf("  Retransmissão: %s%s%s\n", YELLOW, internal_power_udp_state.retransmission_enabled ? "Ativada" : "Desativada", RESET);
            printf("  Backoff: %s%s%s\n", YELLOW, internal_power_udp_state.backoff_enabled ? "Ativada" : "Desativada", RESET);
            printf("  Sequência: %s%s%s\n", YELLOW, internal_power_udp_state.sequence_enabled ? "Ativada" : "Desativada", RESET);
            printf("  Timeout Base: %s%u ms%s\n", YELLOW, internal_power_udp_state.base_timeout_ms, RESET);
            printf("  Max Retries: %s%u%s\n", YELLOW, internal_power_udp_state.max_retries, RESET);
        }
        else if (bytes_received > 0)
        { // mensagem recebida, mas tamanhos nao correspondem
            fprintf(stderr, "[PowerUDP Warning] Recebida mensagem multicast com tamanho inesperado: %zd bytes (esperado %zu)\n",
                    bytes_received, sizeof(ConfigMessage));
        }
    }
    printf("%s[PowerUDP]%s Multicast listener thread terminada.\n", RED, RESET);
    return NULL;
}

// incializa a stack de comunicação PowerUDP e efetua o resgisto do cliente no servidor
int init_protocol(const char *server_ip, int server_tcp_port_param, const char *psk)
{
    if (protocol_initialized)
    { // prevenir dupla inicialização
        fprintf(stderr, "%s[PowerUDP Error] Protocolo já inicializado.%s\n", RED, RESET);
        return -1;
    }
    server_is_shutting_down = 0; // inicializar a flag
    srand(time(NULL));

    // configuracoes default do PowerUDP
    internal_power_udp_state.retransmission_enabled = 1;
    internal_power_udp_state.backoff_enabled = 1;
    internal_power_udp_state.sequence_enabled = 1;
    internal_power_udp_state.base_timeout_ms = 1000;
    internal_power_udp_state.max_retries = 3;
    internal_power_udp_state.current_send_sequence_number = 0;
    internal_power_udp_state.expected_recv_sequence_number = 0;
    internal_power_udp_state.packet_loss_probability = 0;

    // guardar a psk atual fornecida
    strncpy(current_psk, psk, MAX_PSK_LEN - 1);
    current_psk[MAX_PSK_LEN - 1] = '\0'; // garantir integridade da string

    // conexao tcp com o servidor
    server_tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_tcp_socket < 0)
    {
        perror("[PowerUDP Error] socket TCP");
        return -1;
    }

    memset(&server_addr_tcp, 0, sizeof(server_addr_tcp));    // limpeza de buffer
    server_addr_tcp.sin_family = AF_INET;                    // ipv4
    server_addr_tcp.sin_port = htons(server_tcp_port_param); // para do servidor (convertida para network byte order)

    // converter ip em string para formato de rede
    if (inet_pton(AF_INET, server_ip, &server_addr_tcp.sin_addr) <= 0)
    {
        perror("[PowerUDP Error] inet_pton TCP server address");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        return -1;
    }

    // efetuar conecao ao servidor (via tcp)
    printf("[PowerUDP Debug] A tentar conectar a %s%s:%d%s (TCP)...\n", BLUE, inet_ntoa(server_addr_tcp.sin_addr), ntohs(server_addr_tcp.sin_port), RESET);
    if (connect(server_tcp_socket, (struct sockaddr *)&server_addr_tcp, sizeof(server_addr_tcp)) < 0)
    {
        perror("[PowerUDP Error] connect TCP servidor");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        return -1;
    }
    printf("%s[PowerUDP]%s Conectado ao servidor de configuração em %s%s:%d%s (TCP).\n", RED, RESET, GREEN, server_ip, server_tcp_port_param, RESET);

    // enviar RegisterMessage para o servidor
    struct RegisterMessage reg_msg;
    strncpy(reg_msg.psk, current_psk, sizeof(reg_msg.psk) - 1);
    reg_msg.psk[sizeof(reg_msg.psk) - 1] = '\0'; // garantir que a string está terminada
                                                 // enviar mensagem de registo
    if (send(server_tcp_socket, &reg_msg, sizeof(reg_msg), 0) < 0)
    {
        perror("[PowerUDP Error] send RegisterMessage");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        return -1;
    }
    printf("%s[PowerUDP]%s Mensagem de registo enviada ao servidor. A aguardar confirmação...\n", RED, RESET);

    // aguardar resposta do servidor
    uint8_t auth_status;
    ssize_t bytes_auth_recv = recv(server_tcp_socket, &auth_status, sizeof(auth_status), 0);

    if (bytes_auth_recv <= 0)
    {
        if (bytes_auth_recv == 0)
        {
            fprintf(stderr, "[PowerUDP Error] Servidor fechou a conexão durante a autenticação.\n");
        }
        else
        {
            perror("[PowerUDP Error] recv authentication status");
        }
        close(server_tcp_socket);
        server_tcp_socket = -1;
        return -1;
    }

    if (auth_status != 1) // 1 = sucesso, 0 = falha
    {
        fprintf(stderr, "[PowerUDP Error] Autenticação falhou no servidor (PSK incorreta ou outro problema).\n");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        return -1;
    }

    printf("%s[PowerUDP]%s Autenticado com sucesso pelo servidor.%s\n", RED, GREEN, RESET);

    // configurar socket UDP para receber mensagens PowerUDP
    udp_socket_internal = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_internal < 0)
    {
        perror("[PowerUDP Error] socket UDP");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        return -1;
    }
    memset(&local_udp_addr, 0, sizeof(local_udp_addr));     // limpeza de buffer
    local_udp_addr.sin_family = AF_INET;                    // ipv4
    local_udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);     // ouvir em qualquer interface de rede
    local_udp_addr.sin_port = htons(POWER_UDP_PORT_CLIENT); // usar a porta udp default do cliente

    // associar o socket UDP ao endereço local
    if (bind(udp_socket_internal, (struct sockaddr *)&local_udp_addr, sizeof(local_udp_addr)) < 0)
    {
        perror("[PowerUDP Error] bind UDP");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        close(udp_socket_internal);
        udp_socket_internal = -1;
        return -1;
    }
    printf("%s[PowerUDP]%s A aguardar por mensagens PowerUDP na porta %s%d%s (UDP).\n", RED, RESET, GREEN, POWER_UDP_PORT_CLIENT, RESET);

    // configurar socket multicast para receber mensagens de configuração
    multicast_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast_socket < 0)
    {
        perror("[PowerUDP Error] socket multicast");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        close(udp_socket_internal);
        udp_socket_internal = -1;
        return -1;
    }
    int reuse = 1; // permitir varios sockets a escutar no mesmo endereco
    if (setsockopt(multicast_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
    {
        perror("[PowerUDP Warning] setsockopt SO_REUSEADDR multicast falhou");
    }
    struct sockaddr_in multicast_addr_bind; // endereco para fazer bind do socket multicast
    memset(&multicast_addr_bind, 0, sizeof(multicast_addr_bind));
    multicast_addr_bind.sin_family = AF_INET;
    multicast_addr_bind.sin_addr.s_addr = htonl(INADDR_ANY); // ouvir em todas as interfaces por multicast
    multicast_addr_bind.sin_port = htons(MULTICAST_PORT);    // porta multicast default

    // associar socket multicast ao endereco
    if (bind(multicast_socket, (struct sockaddr *)&multicast_addr_bind, sizeof(multicast_addr_bind)) < 0)
    {
        perror("[PowerUDP Error] bind multicast");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        close(udp_socket_internal);
        udp_socket_internal = -1;
        close(multicast_socket);
        multicast_socket = -1;
        return -1;
    }

    struct ip_mreq mreq;
    // converter endereço do grupo multicast de string para formato de rede
    if (inet_pton(AF_INET, MULTICAST_ADDRESS, &mreq.imr_multiaddr.s_addr) <= 0)
    {
        perror("[PowerUDP Error] inet_pton multicast group address");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        close(udp_socket_internal);
        udp_socket_internal = -1;
        close(multicast_socket);
        multicast_socket = -1;
        return -1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); // juntar ao endereco do grupo multicast em todas as interfaces
    // adicionar o socket ao grupo multicast (IP_ADD_MEMBERSHIP)
    if (setsockopt(multicast_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    {
        perror("[PowerUDP Error] setsockopt IP_ADD_MEMBERSHIP");
        close(server_tcp_socket);
        server_tcp_socket = -1;
        close(udp_socket_internal);
        udp_socket_internal = -1;
        close(multicast_socket);
        multicast_socket = -1;
        return -1;
    }
    printf("%s[PowerUDP]%s Juntou-se ao grupo multicast %s%s%s na porta %s%d%s.\n", RED, RESET, GREEN, MULTICAST_ADDRESS, RESET, GREEN, MULTICAST_PORT, RESET);

    // inciar thread para receber mensagens multicast
    keep_multicast_listener_running = 1;
    if (pthread_create(&multicast_thread_id, NULL, multicast_listener_thread_func, NULL) != 0)
    {
        perror("[PowerUDP Error] pthread_create multicast_listener");
        keep_multicast_listener_running = 0;
        // remover subscrição multicast antes de sair
        setsockopt(multicast_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        close(server_tcp_socket);
        server_tcp_socket = -1;
        close(udp_socket_internal);
        udp_socket_internal = -1;
        close(multicast_socket);
        multicast_socket = -1;
        return -1;
    }

    protocol_initialized = 1;
    stats_are_valid_internal = 0;
    printf("%s[PowerUDP]%s Protocolo inicializado com sucesso.%s\n", RED, GREEN, RESET);
    return 0;
}

// termina a stack de comunicação PowerUDP e liberta recursos associados
void close_protocol()
{
    printf("%s[PowerUDP]%s A fechar protocolo...\n", RED, RESET);

    // 1. Sinalizar e juntar-se à thread do listener multicast
    if (keep_multicast_listener_running)
    {
        keep_multicast_listener_running = 0; // sinalizar para a thread terminar

        printf("%s[PowerUDP]%s A aguardar thread multicast listener terminar...\n", RED, RESET);
        if (multicast_thread_id != 0)
        {
            pthread_join(multicast_thread_id, NULL);
            multicast_thread_id = 0;
        }
        printf("%s[PowerUDP]%s Thread multicast listener terminada.\n", RED, RESET);
    }

    // limpar socket e remover membership do grupo multicast
    if (multicast_socket != -1)
    {
        struct ip_mreq mreq_drop; // remover a subcripção do grupo multicast
        if (inet_pton(AF_INET, MULTICAST_ADDRESS, &mreq_drop.imr_multiaddr.s_addr) > 0)
        {
            mreq_drop.imr_interface.s_addr = htonl(INADDR_ANY);
            // Remover o socket do grupo multicast (IP_DROP_MEMBERSHIP)
            if (setsockopt(multicast_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq_drop, sizeof(mreq_drop)) < 0)
            {
                perror("[PowerUDP Warning] setsockopt IP_DROP_MEMBERSHIP failed");
            }
            else
            {
                printf("%s[PowerUDP]%s Saiu do grupo multicast %s%s%s.\n", RED, RESET, YELLOW, MULTICAST_ADDRESS, RESET);
            }
        }
        close(multicast_socket); // fechar socket multicast
        multicast_socket = -1;
        printf("%s[PowerUDP]%s Socket multicast fechado.\n", RED, RESET);
    }

    // fechar socket tcp (para o servidor)
    if (server_tcp_socket != -1)
    {
        close(server_tcp_socket); // fechar socket tcp
        server_tcp_socket = -1;
        printf("%s[PowerUDP]%s Socket TCP do servidor fechado.\n", RED, RESET);
    }

    // fechar socket udp princiapl
    if (udp_socket_internal != -1)
    {
        close(udp_socket_internal); // fechar socket udp principal
        udp_socket_internal = -1;
        printf("%s[PowerUDP]%s Socket UDP principal fechado.\n", RED, RESET);
    }

    protocol_initialized = 0;
    printf("%s[PowerUDP]%s Protocolo fechado.\n", RED, RESET);
}

// solicitar ao servidor alteração de configuração do protocolo
int request_protocol_config(int enable_retransmission, int enable_backoff, int enable_sequence, uint16_t base_timeout_ms_param, uint8_t max_retries_param)
{
    if (!protocol_initialized || server_tcp_socket < 0)
    {
        fprintf(stderr, "%s[PowerUDP Error] Protocolo não inicializado ou sem conexão TCP ao servidor para request_protocol_config.%s\n", RED, RESET);
        return -1;
    }

    ConfigMessage config_req_msg;
    config_req_msg.enable_retrans = (uint8_t)enable_retransmission;
    config_req_msg.enable_backoff = (uint8_t)enable_backoff;
    config_req_msg.enable_seq = (uint8_t)enable_sequence;
    config_req_msg.base_timeout = htons(base_timeout_ms_param); // converter para network byte order para envio
    config_req_msg.max_retries = max_retries_param;

    // enviar o pedido de configuração para o servidor via TCP
    if (send(server_tcp_socket, &config_req_msg, sizeof(config_req_msg), 0) < 0)
    {
        perror("[PowerUDP Error] send ConfigMessage request");
        return -1;
    }
    printf("%s[PowerUDP]%s Pedido de alteração de configuração enviado ao servidor.\n", RED, RESET);
    return 0;
}

// rotina para enviar uma mensagem para outro cliente via PowerUDP
int send_message(const char *destination_ip, int destination_port, const char *message, int len)
{
    if (!protocol_initialized || udp_socket_internal < 0)
    {
        fprintf(stderr, "%s[PowerUDP Error] Protocolo não inicializado para send_message.%s\n", RED, RESET);
        return -2;
    }
    if (len <= 0 || len > MAX_PAYLOAD_SIZE)
    { // validar tamanho da mensagem
        fprintf(stderr, "%s[PowerUDP Error] Tamanho da mensagem inválido (%d). Max: %d.%s\n", RED, len, MAX_PAYLOAD_SIZE, RESET);
        return -1;
    }
    if (destination_port <= 0 || destination_port > 65535)
    {
        fprintf(stderr, "%s[PowerUDP Error] Porta de destino inválida (%s%d%s) para send_message.%s\n", RED, YELLOW, destination_port, RESET, RESET);
        return -1;
    }

    power_udp_packet_t packet_to_send;                  // pacote powerudp a ser enviado
    memset(&packet_to_send, 0, sizeof(packet_to_send)); // inicializar o pacote a enviar

    // preparar o cabeçalho do pacote (todos os campos em network byte order antes do envio)
    packet_to_send.header.sequence_number = htonl(internal_power_udp_state.current_send_sequence_number);
    packet_to_send.header.type = PACKET_TYPE_DATA;
    packet_to_send.header.data_length = htons((uint16_t)len); // comprimento dos dados
    memcpy(packet_to_send.payload, message, len);             // copiar dados para o campo de payload

    // preparar endereço de destino UDP
    struct sockaddr_in dest_addr_udp;
    memset(&dest_addr_udp, 0, sizeof(dest_addr_udp));
    dest_addr_udp.sin_family = AF_INET;
    dest_addr_udp.sin_port = htons((uint16_t)destination_port); // usar a porta de destino fornecida (para permitir comunicacao com clientes atras de NAT simples)
    if (inet_pton(AF_INET, destination_ip, &dest_addr_udp.sin_addr) <= 0)
    { // converter IP de string para formato de rede
        perror("[PowerUDP Error] inet_pton para destino UDP em send_message");
        return -1;
    }

    int attempts = 0;             // Contador de tentativas de envio
    long current_timeout_ms_calc; // Timeout calculado para a tentativa atual
    struct timeval tv_select;     // Estrutura de timeout para select()
    fd_set read_fds;              // Conjunto de descritores de ficheiro para select()

    // reset de estatísticas
    stats_are_valid_internal = 1; // marcar o inicio de uma nova tentativa de envio
    last_msg_retransmissions = 0;
    last_msg_delivery_time_ms = -1; // a atualizar futuramente
    last_msg_attempt_successful = 0;

    struct timeval time_start_send, time_end_send; // para calcular o tempo de entrega
    gettimeofday(&time_start_send, NULL);          // registar o temp de inicio do envio

    // loop para ENVIO e RETRANSMISSAO
    // Loop enquanto attempts <= max_retries. Se max_retries for 0, uma tentativa. Se 3, então 0,1,2,3 (4 tentativas no total)
    while (attempts <= internal_power_udp_state.max_retries)
    {
        // efetuar a simulacao da perda de pacotes (ficticia, apenas nao enviar)
        if (internal_power_udp_state.packet_loss_probability > 0 && (rand() % 100) < internal_power_udp_state.packet_loss_probability)
        {
            printf("%s[PowerUDP SIMULATE] Pacote PowerUDP (seq %u) para %s%s:%d%s PERDIDO intencionalmente (tentativa %d).%s\n",
                   CYAN, ntohl(packet_to_send.header.sequence_number), BLUE, destination_ip, destination_port, CYAN, attempts, RESET);
        }
        else
        {
            // envio efetivo de um pacote
            ssize_t bytes_sent = sendto(udp_socket_internal, &packet_to_send, sizeof(power_udp_header_t) + len, 0,
                                        (struct sockaddr *)&dest_addr_udp, sizeof(dest_addr_udp));
            if (bytes_sent < 0)
            { // erro no sendto
                perror("[PowerUDP Error] sendto data packet");
                last_msg_attempt_successful = 0; // marcar como falha
                gettimeofday(&time_end_send, NULL);
                last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                            (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
                return -1;
            }
            printf("%s[PowerUDP]%s Pacote PowerUDP (seq %u, %zd bytes) enviado para %s%s:%d%s (tentativa %d).\n",
                   RED, RESET, ntohl(packet_to_send.header.sequence_number), bytes_sent, BLUE, destination_ip, destination_port, RESET, attempts);
        }

        // se retransmissões estão desativadas, não esperamos por ACK
        if (!internal_power_udp_state.retransmission_enabled)
        {
            internal_power_udp_state.current_send_sequence_number++; // incrementar número de sequência para próxima mensagem
            last_msg_retransmissions = attempts;
            last_msg_attempt_successful = 1; // assumir sucesso se não há retransmissão
            gettimeofday(&time_end_send, NULL);
            last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                        (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
            return len; // devolver número de bytes de payload enviados
        }

        // calcular timeout para esta tentativa (backoff exponencial se ativado)
        if (internal_power_udp_state.backoff_enabled && attempts > 0)
        {
            current_timeout_ms_calc = internal_power_udp_state.base_timeout_ms * (1L << (attempts));
        }
        else
        {
            current_timeout_ms_calc = internal_power_udp_state.base_timeout_ms;
        }

        tv_select.tv_sec = current_timeout_ms_calc / 1000;
        tv_select.tv_usec = (current_timeout_ms_calc % 1000) * 1000;

        FD_ZERO(&read_fds);                     // limpar connjunto de file descriptors
        FD_SET(udp_socket_internal, &read_fds); // adicionar socket UDP ao conjunto para esperar por ACK

        printf("%s[PowerUDP]%s A esperar por ACK (seq %u) durante %ld ms...\n", RED, RESET, ntohl(packet_to_send.header.sequence_number), current_timeout_ms_calc);
        // o select permite aguardar por dados no socket com timeout
        int ret_select = select(udp_socket_internal + 1, &read_fds, NULL, NULL, &tv_select);

        if (ret_select < 0)
        { // erro no select()
            if (errno == EINTR)
            { // interrompido por um sinal
                printf("%s[PowerUDP]%s select() interrompido. A retentar envio/espera.\n", RED, RESET);
                continue;
            }
            perror("[PowerUDP Error] select_on_ack");
            last_msg_attempt_successful = 0;
            gettimeofday(&time_end_send, NULL);
            last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                        (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
            return -1;
        }
        else if (ret_select == 0)
        { // timeout do select()
            printf("%s[PowerUDP]%s Timeout ao esperar por ACK (seq %u).\n", RED, RESET, ntohl(packet_to_send.header.sequence_number));
            attempts++; // incrementar contador de tentativas
            last_msg_retransmissions = attempts;
            continue; // nova tentativa de envio
        }
        else
        { // dados no socket
            if (FD_ISSET(udp_socket_internal, &read_fds))
            {                                       // verificar se o socket UDP está pronto para leitura
                power_udp_packet_t ack_packet;      // pacote para guardar o ACK/NAK recebido
                struct sockaddr_in ack_sender_addr; // endereço do remetente do ACK/NAK
                socklen_t ack_sender_addr_len = sizeof(ack_sender_addr);

                // aguardar por ACK/NAK
                ssize_t bytes_recv_ack = recvfrom(udp_socket_internal, &ack_packet, sizeof(ack_packet), 0,
                                                  (struct sockaddr *)&ack_sender_addr, &ack_sender_addr_len);

                if (bytes_recv_ack < 0)
                { // erro ao receber ACK/NAK
                    if (errno == EINTR)
                        continue; // interrupcao, tentar select novamente
                    perror("[PowerUDP Error] recvfrom_ack");
                    attempts++;
                    last_msg_retransmissions = attempts;
                    continue;
                }

                // verificar se é um ACK/NAK válido para o pacote enviado
                if (bytes_recv_ack >= (ssize_t)sizeof(power_udp_header_t) &&                                    // tamanho min do cabecalho
                    (ack_packet.header.type == PACKET_TYPE_ACK || ack_packet.header.type == PACKET_TYPE_NAK) && // tipo ACK ou NAK
                    ntohl(ack_packet.header.sequence_number) == ntohl(packet_to_send.header.sequence_number))
                { // numero de sequencia corresponde

                    if (ack_packet.header.type == PACKET_TYPE_ACK)
                    { // ACK recebido
                        printf("%s[PowerUDP]%s ACK (seq %u) recebido de %s%s:%d%s.\n",
                               RED, RESET, ntohl(ack_packet.header.sequence_number),
                               BLUE, inet_ntoa(ack_sender_addr.sin_addr), ntohs(ack_sender_addr.sin_port), RESET);

                        internal_power_udp_state.current_send_sequence_number++; // incrementar para próxima mensagem
                        last_msg_retransmissions = attempts;
                        last_msg_attempt_successful = 1;    // marcar como sucesso
                        gettimeofday(&time_end_send, NULL); // calcular tempo de entrega
                        last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                                    (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
                        return len; // sucesso
                    }
                    else
                    { // PACKET_TYPE_NAK recebido
                        printf("%s[PowerUDP]%s NAK (seq %u) recebido de %s%s:%d%s. Tratar como falha para esta tentativa.\n",
                               RED, RESET, ntohl(ack_packet.header.sequence_number),
                               BLUE, inet_ntoa(ack_sender_addr.sin_addr), ntohs(ack_sender_addr.sin_port), RESET);
                        attempts++; // incrementar tentativas
                        last_msg_retransmissions = attempts;
                        continue;
                    }
                }
                else
                { // recebido algo inesperado (como um pacote de dados, ACK/NAK antigo)
                    // por simplicidade ignorar e deixar o select dar timeout eventualmente.
                    if (bytes_recv_ack >= (ssize_t)sizeof(power_udp_header_t))
                    {
                        printf("%s[PowerUDP]%s Pacote UDP inesperado (type %d, seq %u, size %zd) recebido enquanto esperava por ACK/NAK para seq %u. A ignorar.\n",
                               RED, RESET, ack_packet.header.type, ntohl(ack_packet.header.sequence_number), bytes_recv_ack, ntohl(packet_to_send.header.sequence_number));
                    }
                    else if (bytes_recv_ack > 0)
                    {
                        printf("%s[PowerUDP]%s Pacote UDP muito curto (%zd bytes) recebido enquanto esperava por ACK/NAK. A ignorar.\n", RED, RESET, bytes_recv_ack);
                    }
                }
            }
        }
    }

    // se o loop terminar, todas as retransmissões falharam
    printf("%s[PowerUDP Error] Falha ao enviar mensagem (seq %u) após %d tentativas (max_retries: %d).%s\n",
           RED, ntohl(packet_to_send.header.sequence_number), attempts, internal_power_udp_state.max_retries, RESET);
    last_msg_attempt_successful = 0;
    gettimeofday(&time_end_send, NULL); // registar tempo
    last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
    return -1;
}

// rotina para rececao de uma mensagem powerudp
int receive_message(char *buffer, int bufsize, char *sender_ip_str, int sender_ip_str_len, uint16_t *sender_port)
{
    if (server_is_shutting_down)
    {
        if (sender_ip_str && sender_ip_str_len > 0)
            sender_ip_str[0] = '\0';
        if (sender_port)
            *sender_port = 0;
        return -3; // retorno especial para termino do servidor
    }

    if (!protocol_initialized || udp_socket_internal < 0)
    {
        fprintf(stderr, "%s[PowerUDP Error] Protocolo não inicializado para receive_message.%s\n", RED, RESET);
        if (sender_ip_str && sender_ip_str_len > 0)
            sender_ip_str[0] = '\0';
        if (sender_port)
            *sender_port = 0;
        return server_is_shutting_down ? -3 : -2;
    }
    if (sender_ip_str == NULL || sender_ip_str_len <= 0 || sender_port == NULL)
    {
        fprintf(stderr, "%s[PowerUDP Error] Parâmetros inválidos para obter informações do remetente em receive_message.%s\n", RED, RESET);
        return -1;
    }

    power_udp_packet_t received_packet;     // pacote PowerUDP recebido
    struct sockaddr_in current_sender_addr; // guardar endereço do remetente
    socklen_t current_addr_len;

    while (protocol_initialized)
    {
        if (server_is_shutting_down)
        {
            if (sender_ip_str && sender_ip_str_len > 0)
                sender_ip_str[0] = '\0';
            if (sender_port)
                *sender_port = 0;
            return -3;
        }

        memset(&received_packet, 0, sizeof(received_packet));
        current_addr_len = sizeof(current_sender_addr);               // reset para cada chamada
        memset(&current_sender_addr, 0, sizeof(current_sender_addr)); // inicializacao sender_addr

        // "peek" de um pacote recebido sem o remover da queue
        ssize_t bytes_peeked = recvfrom(udp_socket_internal, &received_packet, sizeof(received_packet), MSG_PEEK,
                                        (struct sockaddr *)&current_sender_addr, &current_addr_len);

        if (server_is_shutting_down)
            return -3; // verificacao apos chamda bloqueante

        if (!protocol_initialized)
        {
            printf("%s[PowerUDP]%s receive_message: Protocolo fechado durante PEEK.\n", RED, RESET);
            return 0;
        }

        if (bytes_peeked < 0)
        {
            if (server_is_shutting_down)
                return -3;

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // esperar e tentar novamente e esperar apos timeouts
                usleep(10000);
                continue;
            }
            if (errno == EINTR)
                continue;
            if (errno == EBADF && server_is_shutting_down)
            {
                printf("[PowerUDP] receive_message: Socket fechado, provável shutdown do servidor.\n");
                if (!server_is_shutting_down)
                {
                    server_is_shutting_down = 1;
                }
                return -3;
            }

            if (server_is_shutting_down)
            {
                return -3;
            }

            if (protocol_initialized)
                perror("[PowerUDP Error] recvfrom MSG_PEEK em receive_message");
            return -1;
        }

        if (bytes_peeked == 0)
        {
            printf("[PowerUDP Warning] recvfrom MSG_PEEK devolveu 0 bytes. Retrying.\n");
            usleep(10000);
            continue;
        }

        // validaçao basica
        if (bytes_peeked < (ssize_t)sizeof(power_udp_header_t))
        {
            printf("[PowerUDP Warning] Pacote PEEKED demasiado curto (%zd bytes). Consumir e ignorar.\n", bytes_peeked);
            // consumir o pacote defeituoso
            char dummy_buffer[MAX_PAYLOAD_SIZE + sizeof(power_udp_header_t) + 100];
            recvfrom(udp_socket_internal, dummy_buffer, sizeof(dummy_buffer), 0, NULL, NULL);
            if (sender_ip_str && sender_ip_str_len > 0)
                sender_ip_str[0] = '\0';
            if (sender_port)
                *sender_port = 0;
            continue;
        }

        // deicisao sobre o que fazer com o pacote recebido com base no tipo
        if (received_packet.header.type == PACKET_TYPE_DATA)
        {
            // pacote de dados recebido, consumir e processar
            ssize_t bytes_consumed = recvfrom(udp_socket_internal, &received_packet, sizeof(received_packet), 0,
                                              NULL, NULL);

            if (server_is_shutting_down)
                return -3;

            if (!protocol_initialized)
            {
                printf("%s[PowerUDP]%s receive_message: Protocolo fechado após PEEK, durante consumo de DATA.\n", RED, RESET);
                if (sender_ip_str && sender_ip_str_len > 0)
                    sender_ip_str[0] = '\0';
                if (sender_port)
                    *sender_port = 0;
                return server_is_shutting_down ? -3 : 0;
            }

            if (bytes_consumed < 0)
            {
                if (server_is_shutting_down)
                    return -3;
                if (errno == EINTR)
                    continue;
                if (errno == EBADF && server_is_shutting_down)
                {
                    printf("[PowerUDP] receive_message: Socket fechado (consumo DATA), provável shutdown do servidor.\n");
                    return -3;
                }
                if (protocol_initialized)
                    perror("[PowerUDP Error] recvfrom (consumo de DATA) em receive_message");
                return server_is_shutting_down ? -3 : -1;
            }
            if (bytes_consumed < (ssize_t)sizeof(power_udp_header_t))
            {
                printf("[PowerUDP Warning] Pacote PowerUDP consumido demasiado curto (%zd bytes). A ignorar.\n", bytes_consumed);
                continue;
            }

            uint32_t recv_seq_num_net = received_packet.header.sequence_number;
            uint32_t recv_seq_num_host = ntohl(recv_seq_num_net);
            uint16_t data_len = ntohs(received_packet.header.data_length);

            if (data_len > (bytes_consumed - sizeof(power_udp_header_t)))
            {
                fprintf(stderr, "[PowerUDP Warning] Inconsistência no tamanho do payload PowerUDP (declarado %u, real %ld). A ignorar.\n",
                        data_len, (long)(bytes_consumed - sizeof(power_udp_header_t)));
                continue;
            }
            if (data_len == 0)
            {
                printf("[PowerUDP Info] Pacote PowerUDP (seq %u) recebido com payload vazio (len 0).\n", recv_seq_num_host);
            }
            if (data_len > (unsigned int)bufsize)
            {
                fprintf(stderr, "[PowerUDP Warning] Payload do pacote PowerUDP (%u bytes) excede buffer da aplicação (%d bytes). Pacote descartado.\n", data_len, bufsize);
                if (internal_power_udp_state.retransmission_enabled && internal_power_udp_state.sequence_enabled)
                {
                    power_udp_packet_t nak_packet;
                    nak_packet.header.type = PACKET_TYPE_NAK;
                    nak_packet.header.sequence_number = recv_seq_num_net;
                    nak_packet.header.data_length = 0;
                    sendto(udp_socket_internal, &nak_packet, sizeof(power_udp_header_t), 0,
                           (struct sockaddr *)&current_sender_addr, current_addr_len);
                    printf("%s[PowerUDP]%s NAK (seq %u, buffer overflow) enviado para %s%s:%d%s.\n", RED, RESET, recv_seq_num_host, BLUE, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port), RESET);
                }
                continue;
            }

            printf("%s[PowerUDP]%s Pacote PowerUDP recebido (seq %u, len %u) de %s%s:%d%s.\n",
                   RED, RESET, recv_seq_num_host, data_len,
                   BLUE, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port), RESET);

            int send_ack_flag = 0;
            int accept_packet_flag = 0;

            if (internal_power_udp_state.sequence_enabled)
            {
                // caso especial para permitir o reset da sequencia para acomodar um novo cliente:
                // se é recebido um pacote com seq 0, o seq local é redefinido para 1
                if (recv_seq_num_host == 0 && internal_power_udp_state.expected_recv_sequence_number > 0)
                {
                    printf("[PowerUDP Info] Recebido pacote com seq 0 (esperado %u). A redefinir seq local para 1 para receber comunicações de um novo cliente.\n",
                           internal_power_udp_state.expected_recv_sequence_number);
                    accept_packet_flag = 1;
                    send_ack_flag = 1;                                          // ACK para seq 0
                    internal_power_udp_state.expected_recv_sequence_number = 1; // esperar 1 agora
                }
                else if (recv_seq_num_host == internal_power_udp_state.expected_recv_sequence_number)
                {
                    accept_packet_flag = 1;
                    send_ack_flag = 1;
                    internal_power_udp_state.expected_recv_sequence_number++;
                }
                else if (recv_seq_num_host < internal_power_udp_state.expected_recv_sequence_number)
                {
                    // pacote antigo/duplicado (e não é o caso de reset seq 0) : enviar NACK.
                    accept_packet_flag = 0; // não processar payload
                    if (internal_power_udp_state.retransmission_enabled)
                    {
                        power_udp_packet_t nak_packet;
                        nak_packet.header.type = PACKET_TYPE_NAK;
                        nak_packet.header.sequence_number = recv_seq_num_net;
                        nak_packet.header.data_length = 0;
                        sendto(udp_socket_internal, &nak_packet, sizeof(power_udp_header_t), 0,
                               (struct sockaddr *)&current_sender_addr, current_addr_len);
                        printf("%s[PowerUDP Info]%s Pacote antigo/duplicado (seq %u, esperado %u). Enviado NAK para seq %u.\n",
                               RED, RESET, recv_seq_num_host, internal_power_udp_state.expected_recv_sequence_number, recv_seq_num_host);
                    }
                }
                else // recv_seq_num_host > internal_power_udp_state.expected_recv_sequence_number // recv_seq_num_host > internal_power_udp_state.expected_recv_sequence_number
                {
                    // pacote está "adiantado" em relação ao esperado.
                    // caso especial: se estamos a espera de seq 0 ( para o cenario de cliente recém-iniciado/reiniciado)
                    // e recebemos um pacote com seq > 0, podemos assumir que o remetente está à frente
                    // e tentar sincronizar "saltando" para a sequência do remetente.
                    if (internal_power_udp_state.expected_recv_sequence_number == 0 && recv_seq_num_host > 0)
                    {
                        printf("[PowerUDP Info] Sincronização de sequência: esperado 0, recebido %u. A aceitar novo cliente e alterando seq para %u.\n",
                               recv_seq_num_host, recv_seq_num_host + 1);
                        accept_packet_flag = 1;
                        send_ack_flag = 1;                                                              // enviar ACK para o pacote recebido
                        internal_power_udp_state.expected_recv_sequence_number = recv_seq_num_host + 1; // saltar para a próxima sequência esperada
                    }
                    else
                    {
                        // caso geral para pacote fora de ordem (adiantado, mas não estamos em seq 0) enviar NAK
                        if (internal_power_udp_state.retransmission_enabled)
                        {
                            power_udp_packet_t nak_packet;
                            nak_packet.header.type = PACKET_TYPE_NAK;
                            nak_packet.header.sequence_number = recv_seq_num_net; // NAK para o recv_seq_num
                            nak_packet.header.data_length = 0;
                            sendto(udp_socket_internal, &nak_packet, sizeof(power_udp_header_t), 0,
                                   (struct sockaddr *)&current_sender_addr, current_addr_len);
                            printf("[PowerUDP] Pacote PowerUDP fora de ordem (seq %u, esperado %u). Enviado NAK para seq %u.\n",
                                   recv_seq_num_host, internal_power_udp_state.expected_recv_sequence_number, recv_seq_num_host);
                        }
                        accept_packet_flag = 0;
                    }
                }
            }
            else // sequence_enabled == false
            {
                accept_packet_flag = 1;
                // se  as retransmissões estiverem ativadas, ainda devemos enviar ACK mesmo se a sequência estiver invativa para entrega de payload
                // a flag send_ack_flag será verificada juntamente com retransmission_enabled mais abaixo.
                send_ack_flag = 1;
            }

            if (send_ack_flag && internal_power_udp_state.retransmission_enabled)
            {
                power_udp_packet_t ack_packet;
                ack_packet.header.type = PACKET_TYPE_ACK;
                ack_packet.header.sequence_number = recv_seq_num_net;
                ack_packet.header.data_length = 0;

                if (internal_power_udp_state.packet_loss_probability > 0 && (rand() % 100) < (internal_power_udp_state.packet_loss_probability / 2))
                {
                    printf("[PowerUDP SIMULATE] Pacote ACK (seq %u) para %s%s:%d%s PERDIDO.\n",
                           recv_seq_num_host, BLUE, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port), RESET);
                }
                else
                {
                    sendto(udp_socket_internal, &ack_packet, sizeof(power_udp_header_t), 0,
                           (struct sockaddr *)&current_sender_addr, current_addr_len);
                    printf("%s[PowerUDP]%s ACK (seq %u) enviado para %s%s:%d%s.\n", RED, RESET, recv_seq_num_host, BLUE, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port), RESET);
                }
            }

            if (accept_packet_flag)
            {
                memcpy(buffer, received_packet.payload, data_len);
                buffer[data_len] = '\0';

                // preencher ip e porta
                if (inet_ntop(AF_INET, &current_sender_addr.sin_addr, sender_ip_str, sender_ip_str_len) == NULL)
                {
                    perror("[PowerUDP Error] inet_ntop em receive_message");
                    sender_ip_str[0] = '\0';
                }
                *sender_port = ntohs(current_sender_addr.sin_port);

                return data_len;
            }
            if (sender_ip_str && sender_ip_str_len > 0)
                sender_ip_str[0] = '\0';
            if (sender_port)
                *sender_port = 0;
            continue;
        }
        else if (received_packet.header.type == PACKET_TYPE_ACK ||
                 received_packet.header.type == PACKET_TYPE_NAK)
        {
            if (server_is_shutting_down)
                return -3;

            // se é um ack/nak provavelmente é respeitante ao send message. portanto nao sera processado aqui.
            // com este sleep, o ojetivo é largar o cpu de modo ao send message o processar (de forma simples)
            usleep(1000);
            continue;
        }
        else
        {
            printf("[PowerUDP Warning] Pacote PEEKED de tipo desconhecido (%d). A consumir e ignorar.\n", received_packet.header.type);
            char dummy_buffer[MAX_PAYLOAD_SIZE + sizeof(power_udp_header_t) + 100];
            recvfrom(udp_socket_internal, dummy_buffer, sizeof(dummy_buffer), 0, NULL, NULL); // Consume
            continue;
        }
    }
    printf("%s[PowerUDP]%s receive_message: Saindo do loop principal, protocolo não inicializado.\n", RED, RESET);
    if (sender_ip_str && sender_ip_str_len > 0)
        sender_ip_str[0] = '\0';
    if (sender_port)
        *sender_port = 0;
    return server_is_shutting_down ? -3 : 0;
}

// obter estatísticas da última mensagem enviada
int get_last_message_stats(int *retransmissions_out, int *delivery_time_ms_out)
{
    if (!protocol_initialized)
    {
        fprintf(stderr, "%s[PowerUDP Error] Protocolo não inicializado para get_last_message_stats.%s\n", RED, RESET);
        if (retransmissions_out)
            *retransmissions_out = -1;
        if (delivery_time_ms_out)
            *delivery_time_ms_out = -1;
        return -1; // Erro
    }
    if (!stats_are_valid_internal)
    {
        // nenhuma tentativa de envio concluida registada
        fprintf(stderr, "[PowerUDP Info] Nenhuma estatística de mensagem disponível ainda (nenhum envio completo).\n");
        if (retransmissions_out)
            *retransmissions_out = -1;
        if (delivery_time_ms_out)
            *delivery_time_ms_out = -1;
        return -1;
    }

    if (retransmissions_out)
        *retransmissions_out = last_msg_retransmissions;

    if (delivery_time_ms_out)
    {
        if (last_msg_attempt_successful)
        { // se a uiltma tentativa foi bem sucedida
            *delivery_time_ms_out = last_msg_delivery_time_ms;
        }
        else
        {
            *delivery_time_ms_out = -1;
        }
    }
    return 0;
}

// simulacao ficticia de perda de pacotes
void inject_packet_loss(int probability)
{
    if (!protocol_initialized)
    {
        fprintf(stderr, "%s[PowerUDP Error] Protocolo não inicializado para inject_packet_loss.%s\n", RED, RESET);
        return;
    }
    if (probability < 0)
        probability = 0;
    if (probability > 100)
        probability = 100;
    internal_power_udp_state.packet_loss_probability = probability; // determinar probabilidade de perda
    printf("%s[PowerUDP]%s Simulação de perda de pacotes definida para %d%%.\n", RED, RESET, probability);
}