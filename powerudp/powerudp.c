#define _GNU_SOURCE // Deve ser a primeira linha para garantir que struct ip_mreq é definida
#include "powerudp.h"
#include "msg_structs.h" // ConfigMessage is defined here

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h> // struct ip_mreq é definida aqui com _GNU_SOURCE
#include <arpa/inet.h>
#include <sys/time.h> // Para gettimeofday() e timers (select)
#include <pthread.h>  // Para a thread do listener multicast
#include <time.h>     // Para srand() e rand() na simulação de perda
#include <errno.h>    // Para perror e errno

// Variáveis estáticas para o estado interno do protocolo PowerUDP
// Estas variáveis mantêm o estado dos sockets, configuração atual, estatísticas, etc.
static int udp_socket_internal = -1;             // Socket UDP principal para PowerUDP
static int server_tcp_socket = -1;               // Socket TCP para comunicação com o servidor de configuração
static int multicast_socket = -1;                // Socket para receber mensagens multicast de configuração

static struct sockaddr_in server_addr_tcp;       // Endereço do servidor de configuração TCP
static struct sockaddr_in local_udp_addr;        // Endereço UDP local para o cliente PowerUDP
static char current_psk[MAX_PSK_LEN];            // Chave pré-partilhada (PSK) atual

// Estatísticas da última mensagem enviada
static int last_msg_retransmissions = 0;         // Número de retransmissões
static int last_msg_delivery_time_ms = -1;       // Tempo de entrega em ms (-1 se falhou ou não tentado)
static int last_msg_attempt_successful = 0;      // 1 se sucesso, 0 se falha
static int stats_are_valid_internal = 0;         // 1 se as estatísticas são de uma tentativa completa

// Variáveis de estado do protocolo
static pthread_t multicast_thread_id;            // ID da thread para o listener multicast
static volatile int protocol_initialized = 0;    // Flag: 1 se o protocolo está inicializado, 0 caso contrário
static volatile int keep_multicast_listener_running = 0; // Flag para controlar a execução da thread multicast

// Parâmetros operacionais atuais do PowerUDP (atualizados via multicast)
static power_udp_config_t internal_power_udp_state; // Estrutura que contém a configuração ativa


// Função executada pela thread que escuta mensagens multicast de configuração do servidor
void* multicast_listener_thread_func(void* arg) {
    (void)arg; // Argumento não utilizado

    struct sockaddr_in sender_addr; // Endereço do remetente da mensagem multicast
    socklen_t sender_addr_len = sizeof(sender_addr);
    // Buffer para receber a mensagem de configuração. ConfigMessage é um typedef de msg_structs.h
    char buffer[sizeof(ConfigMessage) + 1];
    ssize_t bytes_received; // Número de bytes recebidos

    printf("[PowerUDP] Multicast listener thread iniciada (Socket: %d).\n", multicast_socket);

    // Configurar um timeout para recvfrom no socket multicast
    // Isto permite que a thread verifique periodicamente a flag keep_multicast_listener_running
    struct timeval tv_mc_recv;
    tv_mc_recv.tv_sec = 1; // Timeout de 1 segundo
    tv_mc_recv.tv_usec = 0;
    if (setsockopt(multicast_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_mc_recv, sizeof tv_mc_recv) < 0) {
        perror("[PowerUDP Warning] setsockopt SO_RCVTIMEO for multicast failed");
        // Continuar sem timeout, mas a terminação da thread pode ser atrasada
    }

    // Loop principal da thread: espera por mensagens multicast
    while (keep_multicast_listener_running) {
        memset(buffer, 0, sizeof(buffer)); // Limpar o buffer
        // Esperar por uma mensagem no socket multicast
        bytes_received = recvfrom(multicast_socket, buffer, sizeof(ConfigMessage), 0,
                                  (struct sockaddr*)&sender_addr, &sender_addr_len);

        // Verificar se a thread deve terminar (imediatamente após recvfrom retornar)
        if (!keep_multicast_listener_running) {
            break;
        }

        // Tratar erros de recvfrom
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { // Timeout ocorreu
                continue; // Voltar ao início do loop para verificar keep_multicast_listener_running
            }
            // Imprimir erro apenas se o protocolo estiver supostamente a correr
            if(protocol_initialized && keep_multicast_listener_running) {
                 perror("[PowerUDP Error] recvfrom multicast");
            }
            // Evitar busy-looping em caso de erros persistentes (que não sejam timeout)
            if (errno != EINTR) usleep(100000); // Pequena pausa para outros erros
            continue;
        }

        // Verificar se o tamanho da mensagem recebida corresponde ao esperado para ConfigMessage
        if (bytes_received == sizeof(ConfigMessage)) {
            ConfigMessage new_config_msg; // Estrutura para guardar a nova configuração
            memcpy(&new_config_msg, buffer, sizeof(ConfigMessage)); // Copiar os dados do buffer para a estrutura

            // Aplicar a nova configuração.
            // Considerar um mutex se internal_power_udp_state for acedida por múltiplas threads concorrentemente.
            // Para este projeto, send_message e receive_message irão ler esta estrutura.
            // Um mutex em torno de atualizações e leituras de internal_power_udp_state seria mais seguro.
            // Por agora, atualização direta:
            internal_power_udp_state.retransmission_enabled = new_config_msg.enable_retrans;
            internal_power_udp_state.backoff_enabled = new_config_msg.enable_backoff;
            internal_power_udp_state.sequence_enabled = new_config_msg.enable_seq;
            // ntohs converte de network byte order para host byte order
            internal_power_udp_state.base_timeout_ms = ntohs(new_config_msg.base_timeout);
            internal_power_udp_state.max_retries = new_config_msg.max_retries;

            // Imprimir a configuração atualizada
            printf("[PowerUDP] Configuração atualizada via multicast:\n");
            printf("  Retransmissão: %s\n", internal_power_udp_state.retransmission_enabled ? "Ativada" : "Desativada");
            printf("  Backoff: %s\n", internal_power_udp_state.backoff_enabled ? "Ativada" : "Desativada");
            printf("  Sequência: %s\n", internal_power_udp_state.sequence_enabled ? "Ativada" : "Desativada");
            printf("  Timeout Base: %u ms\n", internal_power_udp_state.base_timeout_ms);
            printf("  Max Retries: %u\n", internal_power_udp_state.max_retries);
        } else if (bytes_received > 0) { // Mensagem recebida, mas com tamanho inesperado
            fprintf(stderr, "[PowerUDP Warning] Recebida mensagem multicast com tamanho inesperado: %zd bytes (esperado %zu)\n",
                    bytes_received, sizeof(ConfigMessage));
        }
    }
    printf("[PowerUDP] Multicast listener thread terminada.\n");
    return NULL;
}

// Inicializa a stack de comunicação PowerUDP e regista o cliente no servidor
int init_protocol(const char *server_ip, int server_tcp_port_param, const char *psk) {
    if (protocol_initialized) { // Verificar se o protocolo já foi inicializado
        fprintf(stderr, "[PowerUDP Error] Protocolo já inicializado.\n");
        return -1; // Retornar erro
    }

    srand(time(NULL)); // Inicializar o gerador de números aleatórios para simulação de perda de pacotes

    // Inicializar o estado padrão do PowerUDP
    internal_power_udp_state.retransmission_enabled = 1; // Ativar retransmissão por defeito
    internal_power_udp_state.backoff_enabled = 1;        // Ativar backoff exponencial por defeito
    internal_power_udp_state.sequence_enabled = 1;       // Ativar numeração de sequência por defeito
    internal_power_udp_state.base_timeout_ms = 1000;     // Timeout base de 1 segundo por defeito
    internal_power_udp_state.max_retries = 3;            // Máximo de 3 retransmissões por defeito
    internal_power_udp_state.current_send_sequence_number = 0; // Número de sequência de envio inicial
    internal_power_udp_state.expected_recv_sequence_number = 0; // Número de sequência de receção esperado inicial
    internal_power_udp_state.packet_loss_probability = 0;    // Sem perda de pacotes por defeito

    // Guardar a PSK
    strncpy(current_psk, psk, MAX_PSK_LEN -1);
    current_psk[MAX_PSK_LEN-1] = '\0'; // Garantir terminação nula

    // 1. Conectar ao Servidor via TCP para registo
    server_tcp_socket = socket(AF_INET, SOCK_STREAM, 0); // Criar socket TCP
    if (server_tcp_socket < 0) {
        perror("[PowerUDP Error] socket TCP");
        return -1;
    }

    // --- INÍCIO DO BLOCO DE PREPARAÇÃO DO ENDEREÇO COM DEPURAÇÃO ---
    memset(&server_addr_tcp, 0, sizeof(server_addr_tcp)); // Limpar estrutura de endereço
    server_addr_tcp.sin_family = AF_INET;                 // Família de endereços IPv4
    server_addr_tcp.sin_port = htons(server_tcp_port_param); // Porta do servidor (convertida para network byte order)

    // Adicionar depuração aqui
    printf("[PowerUDP Debug] Tentando converter IP: \"%s\" para servidor TCP.\n", server_ip);
    printf("[PowerUDP Debug] Porta servidor TCP (host order): %d, Porta (network order): %d\n", server_tcp_port_param, ntohs(server_addr_tcp.sin_port));

    // Converter endereço IP de string para formato de rede
    if (inet_pton(AF_INET, server_ip, &server_addr_tcp.sin_addr) <= 0) {
        perror("[PowerUDP Error] inet_pton TCP server address");
        close(server_tcp_socket); server_tcp_socket = -1;
        return -1;
    }
    // Adicionar depuração aqui
    printf("[PowerUDP Debug] IP convertido com sucesso para: %s (para servidor TCP)\n", inet_ntoa(server_addr_tcp.sin_addr));
    // --- FIM DO BLOCO DE PREPARAÇÃO DO ENDEREÇO COM DEPURAÇÃO ---


    // Conectar ao servidor TCP
    printf("[PowerUDP Debug] Tentando conectar a %s:%d (TCP)...\n", inet_ntoa(server_addr_tcp.sin_addr), ntohs(server_addr_tcp.sin_port));
    if (connect(server_tcp_socket, (struct sockaddr*)&server_addr_tcp, sizeof(server_addr_tcp)) < 0) {
        perror("[PowerUDP Error] connect TCP servidor");
        close(server_tcp_socket); server_tcp_socket = -1;
        return -1;
    }
    printf("[PowerUDP] Conectado ao servidor de configuração em %s:%d (TCP).\n", server_ip, server_tcp_port_param);

    // Enviar RegisterMessage para o servidor
    struct RegisterMessage reg_msg; // Estrutura da mensagem de registo
    strncpy(reg_msg.psk, current_psk, sizeof(reg_msg.psk) -1);
    reg_msg.psk[sizeof(reg_msg.psk)-1] = '\0'; // Garantir terminação nula
    // Enviar a mensagem de registo
    if (send(server_tcp_socket, &reg_msg, sizeof(reg_msg), 0) < 0) {
        perror("[PowerUDP Error] send RegisterMessage");
        close(server_tcp_socket); server_tcp_socket = -1;
        return -1;
    }
    printf("[PowerUDP] Mensagem de registo enviada ao servidor.\n");
    // O PDF não especifica uma confirmação de registo do servidor. Assumir sucesso se o envio for bem-sucedido.

    // 2. Configurar socket UDP para comunicação PowerUDP
    udp_socket_internal = socket(AF_INET, SOCK_DGRAM, 0); // Criar socket UDP
    if (udp_socket_internal < 0) {
        perror("[PowerUDP Error] socket UDP");
        close(server_tcp_socket); server_tcp_socket = -1;
        return -1;
    }
    memset(&local_udp_addr, 0, sizeof(local_udp_addr));    // Limpar estrutura de endereço local
    local_udp_addr.sin_family = AF_INET;                   // Família de endereços IPv4
    local_udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);    // Escutar em qualquer interface de rede
    local_udp_addr.sin_port = htons(POWER_UDP_PORT_CLIENT); // Usar porta padrão do cliente PowerUDP

    // Associar (bind) o socket UDP ao endereço local
    if (bind(udp_socket_internal, (struct sockaddr*)&local_udp_addr, sizeof(local_udp_addr)) < 0) {
        perror("[PowerUDP Error] bind UDP");
        close(server_tcp_socket); server_tcp_socket = -1;
        close(udp_socket_internal); udp_socket_internal = -1;
        return -1;
    }
    printf("[PowerUDP] Escutando por mensagens PowerUDP na porta %d (UDP).\n", POWER_UDP_PORT_CLIENT);

    // 3. Configurar socket Multicast para receber configurações
    multicast_socket = socket(AF_INET, SOCK_DGRAM, 0); // Criar socket UDP para multicast
    if (multicast_socket < 0) {
        perror("[PowerUDP Error] socket multicast");
        close(server_tcp_socket); server_tcp_socket = -1;
        close(udp_socket_internal); udp_socket_internal = -1;
        return -1;
    }
    int reuse = 1; // Permitir reutilização do endereço (SO_REUSEADDR)
    if (setsockopt(multicast_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0) {
        perror("[PowerUDP Warning] setsockopt SO_REUSEADDR multicast failed");
        // Não é fatal, mas pode causar problemas se a porta estiver em TIME_WAIT
    }
    struct sockaddr_in multicast_addr_bind; // Endereço para fazer bind do socket multicast
    memset(&multicast_addr_bind, 0, sizeof(multicast_addr_bind));
    multicast_addr_bind.sin_family = AF_INET;
    multicast_addr_bind.sin_addr.s_addr = htonl(INADDR_ANY); // Escutar em todas as interfaces para multicast
    multicast_addr_bind.sin_port = htons(MULTICAST_PORT);    // Porta multicast definida

    // Associar (bind) o socket multicast ao endereço
    if (bind(multicast_socket, (struct sockaddr*)&multicast_addr_bind, sizeof(multicast_addr_bind)) < 0) {
        perror("[PowerUDP Error] bind multicast");
        close(server_tcp_socket); server_tcp_socket = -1;
        close(udp_socket_internal); udp_socket_internal = -1;
        close(multicast_socket); multicast_socket = -1;
        return -1;
    }

    struct ip_mreq mreq; // Estrutura de requisição multicast
    // Converter endereço do grupo multicast de string para formato de rede
    if (inet_pton(AF_INET, MULTICAST_ADDRESS, &mreq.imr_multiaddr.s_addr) <= 0) {
        perror("[PowerUDP Error] inet_pton multicast group address");
        close(server_tcp_socket); server_tcp_socket = -1;
        close(udp_socket_internal); udp_socket_internal = -1;
        close(multicast_socket); multicast_socket = -1;
        return -1;
    }
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); // Juntar-se ao grupo em todas as interfaces disponíveis
                                                   // Ou especificar um IP de interface particular
    // Adicionar o socket ao grupo multicast (IP_ADD_MEMBERSHIP)
    if (setsockopt(multicast_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("[PowerUDP Error] setsockopt IP_ADD_MEMBERSHIP");
        close(server_tcp_socket); server_tcp_socket = -1;
        close(udp_socket_internal); udp_socket_internal = -1;
        close(multicast_socket); multicast_socket = -1;
        return -1;
    }
    printf("[PowerUDP] Juntou-se ao grupo multicast %s na porta %d.\n", MULTICAST_ADDRESS, MULTICAST_PORT);

    // 4. Iniciar a thread do listener multicast
    keep_multicast_listener_running = 1; // Sinalizar para a thread começar a executar
    if (pthread_create(&multicast_thread_id, NULL, multicast_listener_thread_func, NULL) != 0) {
        perror("[PowerUDP Error] pthread_create multicast_listener");
        keep_multicast_listener_running = 0; // Garantir que a flag é resetada
        // Limpar subscrição multicast antes de sair
        setsockopt(multicast_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        close(server_tcp_socket); server_tcp_socket = -1;
        close(udp_socket_internal); udp_socket_internal = -1;
        close(multicast_socket); multicast_socket = -1;
        return -1;
    }

    protocol_initialized = 1; // Marcar o protocolo como inicializado
    stats_are_valid_internal = 0; // Nenhuma estatística ainda
    printf("[PowerUDP] Protocolo inicializado com sucesso.\n");
    return 0; // Sucesso
}

// Termina a stack de comunicação PowerUDP e liberta recursos
void close_protocol() {
    if (!protocol_initialized && !keep_multicast_listener_running && multicast_socket == -1 && server_tcp_socket == -1 && udp_socket_internal == -1) {
        // Permite chamar close_protocol múltiplas vezes ou se init falhou parcialmente
        // printf("[PowerUDP] Protocolo não inicializado ou já fechado.\n");
        // No entanto, para garantir a limpeza correta se a inicialização falhou a meio,
        // é melhor prosseguir com as verificações individuais de fecho.
    }
    printf("[PowerUDP] Fechando protocolo...\n");

    // 1. Sinalizar e juntar-se à thread do listener multicast
    if (keep_multicast_listener_running) {
        keep_multicast_listener_running = 0; // Sinalizar para a thread terminar
        // O recvfrom na thread pode estar a bloquear.
        // Fechar o socket fará com que recvfrom retorne com erro,
        // permitindo que a thread verifique keep_multicast_listener_running e saia.
        // Alternativamente, shutdown(multicast_socket, SHUT_RD) pode ser usado.
        // Se o socket já foi fechado (multicast_socket == -1), não fazer nada.
        // A thread deve sair devido ao timeout ou à flag.
        // Para garantir que a thread não fica bloqueada indefinidamente se o socket não for fechado
        // por outra razão, o timeout no recvfrom da thread é importante.
        printf("[PowerUDP] Aguardando thread multicast listener terminar...\n");
        if (multicast_thread_id != 0) { // Apenas fazer join se a thread foi criada
             pthread_join(multicast_thread_id, NULL);
             multicast_thread_id = 0; // Resetar ID da thread
        }
        printf("[PowerUDP] Thread multicast listener terminada.\n");
    }

    // 2. Limpar socket multicast e subscrição
    if (multicast_socket != -1) {
        struct ip_mreq mreq_drop; // Para remover a subscrição do grupo
        if (inet_pton(AF_INET, MULTICAST_ADDRESS, &mreq_drop.imr_multiaddr.s_addr) > 0) {
            mreq_drop.imr_interface.s_addr = htonl(INADDR_ANY);
            // Remover o socket do grupo multicast (IP_DROP_MEMBERSHIP)
            if (setsockopt(multicast_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq_drop, sizeof(mreq_drop)) < 0) {
                perror("[PowerUDP Warning] setsockopt IP_DROP_MEMBERSHIP failed");
            } else {
                printf("[PowerUDP] Saiu do grupo multicast %s.\n", MULTICAST_ADDRESS);
            }
        }
        close(multicast_socket); // Fechar o socket multicast
        multicast_socket = -1;   // Marcar como fechado
        printf("[PowerUDP] Socket multicast fechado.\n");
    }

    // 3. Fechar socket TCP para o servidor
    if (server_tcp_socket != -1) {
        close(server_tcp_socket); // Fechar o socket TCP
        server_tcp_socket = -1;   // Marcar como fechado
        printf("[PowerUDP] Socket TCP do servidor fechado.\n");
    }

    // 4. Fechar socket UDP principal
    if (udp_socket_internal != -1) {
        close(udp_socket_internal); // Fechar o socket UDP principal
        udp_socket_internal = -1;   // Marcar como fechado
        printf("[PowerUDP] Socket UDP principal fechado.\n");
    }

    protocol_initialized = 0; // Marcar o protocolo como completamente fechado
    printf("[PowerUDP] Protocolo fechado.\n");
}

// Solicita ao servidor uma mudança na configuração do protocolo PowerUDP
int request_protocol_config(int enable_retransmission, int enable_backoff, int enable_sequence, uint16_t base_timeout_ms_param, uint8_t max_retries_param) {
    if (!protocol_initialized || server_tcp_socket < 0) { // Verificar se o protocolo está inicializado e conectado
        fprintf(stderr, "[PowerUDP Error] Protocolo não inicializado ou sem conexão TCP ao servidor para request_protocol_config.\n");
        return -1;
    }

    ConfigMessage config_req_msg; // Estrutura para a mensagem de pedido de configuração
    // Preencher a estrutura com os novos parâmetros de configuração
    config_req_msg.enable_retrans = (uint8_t)enable_retransmission;
    config_req_msg.enable_backoff = (uint8_t)enable_backoff;
    config_req_msg.enable_seq = (uint8_t)enable_sequence;
    config_req_msg.base_timeout = htons(base_timeout_ms_param); // Converter para network byte order para envio
    config_req_msg.max_retries = max_retries_param;

    // Enviar o pedido de configuração para o servidor via TCP
    if (send(server_tcp_socket, &config_req_msg, sizeof(config_req_msg), 0) < 0) {
        perror("[PowerUDP Error] send ConfigMessage request");
        // Pode indicar que o servidor desconectou. Pode ser necessário um tratamento de erro mais robusto.
        return -1;
    }
    printf("[PowerUDP] Pedido de alteração de configuração enviado ao servidor.\n");
    return 0;
}

// Envia uma mensagem usando o protocolo PowerUDP
int send_message(const char *destination_ip, const char *message, int len) {
    if (!protocol_initialized || udp_socket_internal < 0) { // Verificar inicialização
        fprintf(stderr, "[PowerUDP Error] Protocolo não inicializado para send_message.\n");
        return -2; // Código de erro especial para não inicializado
    }
    if (len <= 0 || len > MAX_PAYLOAD_SIZE) { // Validar tamanho da mensagem
        fprintf(stderr, "[PowerUDP Error] Tamanho da mensagem inválido (%d). Max: %d.\n", len, MAX_PAYLOAD_SIZE);
        return -1;
    }

    power_udp_packet_t packet_to_send; // Pacote PowerUDP a ser enviado
    memset(&packet_to_send, 0, sizeof(packet_to_send)); // Limpar pacote

    // Preparar cabeçalho do pacote (todos os campos em network byte order antes do envio)
    packet_to_send.header.sequence_number = htonl(internal_power_udp_state.current_send_sequence_number);
    packet_to_send.header.type = PACKET_TYPE_DATA; // Este é um pacote de dados
    packet_to_send.header.data_length = htons((uint16_t)len); // Comprimento dos dados
    memcpy(packet_to_send.payload, message, len); // Copiar dados da aplicação para o payload

    // Preparar endereço de destino UDP
    struct sockaddr_in dest_addr_udp;
    memset(&dest_addr_udp, 0, sizeof(dest_addr_udp));
    dest_addr_udp.sin_family = AF_INET;
    dest_addr_udp.sin_port = htons(POWER_UDP_PORT_CLIENT); // Porta de destino é a porta padrão do cliente PowerUDP
    if (inet_pton(AF_INET, destination_ip, &dest_addr_udp.sin_addr) <= 0) { // Converter IP de destino
        perror("[PowerUDP Error] inet_pton para destino UDP em send_message");
        return -1;
    }

    int attempts = 0; // Contador de tentativas de envio
    long current_timeout_ms_calc; // Timeout calculado para a tentativa atual
    struct timeval tv_select; // Estrutura de timeout para select()
    fd_set read_fds;          // Conjunto de descritores de ficheiro para select()

    // Resetar estatísticas para esta tentativa de mensagem
    stats_are_valid_internal = 1; // Marcar que uma tentativa está a ser feita
    last_msg_retransmissions = 0;
    last_msg_delivery_time_ms = -1; // Será atualizado no sucesso ou falha final
    last_msg_attempt_successful = 0;

    struct timeval time_start_send, time_end_send; // Para calcular o tempo de entrega
    gettimeofday(&time_start_send, NULL); // Registar tempo de início da operação de envio

    // Loop para enviar e potencialmente retransmitir
    // Loop enquanto attempts <= max_retries. Se max_retries for 0, uma tentativa. Se 3, então 0,1,2,3 (4 tentativas no total)
    while (attempts <= internal_power_udp_state.max_retries) {
        // Simular perda de pacotes se ativado
        if (internal_power_udp_state.packet_loss_probability > 0 && (rand() % 100) < internal_power_udp_state.packet_loss_probability) {
            printf("[PowerUDP SIMULATE] Pacote DATA (seq %u) para %s:%d PERDIDO intencionalmente (tentativa %d).\n",
                   ntohl(packet_to_send.header.sequence_number), destination_ip, POWER_UDP_PORT_CLIENT, attempts);
        } else {
            // Envio real do pacote
            ssize_t bytes_sent = sendto(udp_socket_internal, &packet_to_send, sizeof(power_udp_header_t) + len, 0,
                                    (struct sockaddr*)&dest_addr_udp, sizeof(dest_addr_udp));
            if (bytes_sent < 0) { // Erro no sendto
                perror("[PowerUDP Error] sendto data packet");
                last_msg_attempt_successful = 0; // Marcar como falha
                gettimeofday(&time_end_send, NULL);
                last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                            (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
                return -1;
            }
            printf("[PowerUDP] Pacote DATA (seq %u, %zd bytes) enviado para %s:%d (tentativa %d).\n",
                   ntohl(packet_to_send.header.sequence_number), bytes_sent, destination_ip, POWER_UDP_PORT_CLIENT, attempts);
        }

        // Se retransmissões estão desativadas, não esperamos por ACK
        if (!internal_power_udp_state.retransmission_enabled) {
            internal_power_udp_state.current_send_sequence_number++; // Incrementar número de sequência para próxima mensagem
            last_msg_retransmissions = attempts;
            last_msg_attempt_successful = 1; // Assumir sucesso se não há retransmissão
            gettimeofday(&time_end_send, NULL);
            last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                            (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
            return len; // Devolver número de bytes de payload enviados
        }

        // Calcular timeout para esta tentativa (backoff exponencial se ativado)
        if (internal_power_udp_state.backoff_enabled && attempts > 0) { // Sem backoff para a primeira tentativa (attempts = 0)
            // Tn = Tmin * 2^n (n=attempts aqui, pois attempts começa em 0 para a 1ª transmissão, 1 para a 1ª retransmissão, etc.)
            // Se attempts = 0 (primeira tentativa), Tmin.
            // Se attempts = 1 (primeira retransmissão), Tmin * 2.
            // Assim, para a retransmissão 'k' (onde k=attempts > 0), o multiplicador é 2^k.
            // No entanto, a fórmula do PDF é Tn = Tmin * 2^n, onde n é "número de tentativas de transmissão falhadas".
            // Se attempts é o número total de envios (0 para o primeiro, 1 para o segundo, etc.),
            // então 'n' (falhas) seria 'attempts' se a primeira tentativa (attempts=0) for a base.
            // Ou 'attempts - 1' se n=0 é a primeira tentativa e n=1 é a primeira retransmissão.
            // A lógica aqui é: attempts=0 -> Tmin; attempts=1 -> Tmin*2; attempts=2 -> Tmin*4
            current_timeout_ms_calc = internal_power_udp_state.base_timeout_ms * (1L << (attempts));
        } else {
            current_timeout_ms_calc = internal_power_udp_state.base_timeout_ms;
        }
        // Limitar timeout a um máximo razoável, ex: 60 segundos
        // if (current_timeout_ms_calc > 60000) current_timeout_ms_calc = 60000;

        tv_select.tv_sec = current_timeout_ms_calc / 1000;
        tv_select.tv_usec = (current_timeout_ms_calc % 1000) * 1000;

        FD_ZERO(&read_fds); // Limpar conjunto de descritores
        FD_SET(udp_socket_internal, &read_fds); // Adicionar socket UDP ao conjunto para esperar por ACK

        printf("[PowerUDP] Esperando por ACK (seq %u) durante %ld ms...\n", ntohl(packet_to_send.header.sequence_number), current_timeout_ms_calc);
        // Usar select() para esperar por dados no socket com timeout
        int ret_select = select(udp_socket_internal + 1, &read_fds, NULL, NULL, &tv_select);

        if (ret_select < 0) { // Erro no select()
            if (errno == EINTR) { // Interrompido por um sinal
                printf("[PowerUDP] select() interrompido. Retentando envio/espera.\n");
                // Não incrementar tentativas, apenas tentar select novamente ou o envio.
                continue;
            }
            perror("[PowerUDP Error] select_on_ack");
            last_msg_attempt_successful = 0;
            gettimeofday(&time_end_send, NULL); // Atualizar tempo antes de retornar
            last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                            (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
            return -1; // Erro geral
        } else if (ret_select == 0) { // Timeout do select()
            printf("[PowerUDP] Timeout esperando por ACK (seq %u).\n", ntohl(packet_to_send.header.sequence_number));
            attempts++; // Incrementar contador de tentativas
            last_msg_retransmissions = attempts;
            // Loop para retransmitir se attempts <= max_retries
            continue;
        } else { // Dados disponíveis no socket
            if (FD_ISSET(udp_socket_internal, &read_fds)) { // Verificar se é o nosso socket UDP
                power_udp_packet_t ack_packet; // Pacote para guardar o ACK/NAK recebido
                struct sockaddr_in ack_sender_addr; // Endereço do remetente do ACK/NAK
                socklen_t ack_sender_addr_len = sizeof(ack_sender_addr);

                // Esperamos um ACK ou NAK. Também pode ser dados para receive_message().
                // Este modelo simples processa um pacote de entrada aqui.
                ssize_t bytes_recv_ack = recvfrom(udp_socket_internal, &ack_packet, sizeof(ack_packet), 0,
                                                 (struct sockaddr*)&ack_sender_addr, &ack_sender_addr_len);

                if (bytes_recv_ack < 0) { // Erro ao receber ACK/NAK
                    if (errno == EINTR) continue; // Interrompido, tentar select novamente
                    perror("[PowerUDP Error] recvfrom_ack");
                    // Erro durante receção do ACK. Tratar como timeout para esta tentativa.
                    attempts++;
                    last_msg_retransmissions = attempts;
                    continue;
                }

                // Verificar se é um ACK/NAK válido para o pacote enviado
                if (bytes_recv_ack >= (ssize_t)sizeof(power_udp_header_t) && // Tamanho mínimo do cabeçalho
                    (ack_packet.header.type == PACKET_TYPE_ACK || ack_packet.header.type == PACKET_TYPE_NAK) && // Tipo ACK ou NAK
                    ntohl(ack_packet.header.sequence_number) == ntohl(packet_to_send.header.sequence_number)) { // Número de sequência corresponde

                    if (ack_packet.header.type == PACKET_TYPE_ACK) { // ACK recebido
                        printf("[PowerUDP] ACK (seq %u) recebido de %s:%d.\n",
                               ntohl(ack_packet.header.sequence_number),
                               inet_ntoa(ack_sender_addr.sin_addr), ntohs(ack_sender_addr.sin_port));

                        internal_power_udp_state.current_send_sequence_number++; // Incrementar para próxima mensagem
                        last_msg_retransmissions = attempts;
                        last_msg_attempt_successful = 1; // Marcar como sucesso
                        gettimeofday(&time_end_send, NULL); // Calcular tempo de entrega
                        last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                                        (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
                        return len; // Sucesso!
                    } else { // PACKET_TYPE_NAK recebido
                         printf("[PowerUDP] NAK (seq %u) recebido de %s:%d. Tratando como falha para esta tentativa.\n",
                                ntohl(ack_packet.header.sequence_number),
                                inet_ntoa(ack_sender_addr.sin_addr), ntohs(ack_sender_addr.sin_port));
                        attempts++; // Incrementar tentativas
                        last_msg_retransmissions = attempts;
                        // Loop para retransmitir se attempts <= max_retries
                        continue;
                    }
                } else { // Recebido algo inesperado (ex: pacote de dados, ACK/NAK antigo)
                    // Simplificação: ignorar e deixar o select dar timeout eventualmente.
                    // Um sistema mais robusto poderia guardar pacotes de dados inesperados.
                    if (bytes_recv_ack >= (ssize_t)sizeof(power_udp_header_t)) {
                        printf("[PowerUDP] Pacote UDP inesperado (type %d, seq %u, size %zd) recebido enquanto esperava por ACK/NAK para seq %u. Ignorando.\n",
                               ack_packet.header.type, ntohl(ack_packet.header.sequence_number), bytes_recv_ack, ntohl(packet_to_send.header.sequence_number));
                    } else if (bytes_recv_ack > 0) { // Pacote muito curto
                         printf("[PowerUDP] Pacote UDP muito curto (%zd bytes) recebido enquanto esperava por ACK/NAK. Ignorando.\n", bytes_recv_ack);
                    }
                    // Deixar o select dar timeout para a tentativa atual.
                    // Não é ideal, pois pode atrasar o processamento do ACK se chegar logo após este pacote inesperado.
                    // Um loop recvfrom não bloqueante após o select seria melhor.
                }
            }
        }
    }

    // Se o loop terminar, todas as retransmissões falharam
    printf("[PowerUDP Error] Falha ao enviar mensagem (seq %u) após %d tentativas (max_retries: %d).\n",
           ntohl(packet_to_send.header.sequence_number), attempts, internal_power_udp_state.max_retries);
    last_msg_attempt_successful = 0; // Marcar como falha
    gettimeofday(&time_end_send, NULL); // Registar tempo mesmo em falha
    last_msg_delivery_time_ms = (time_end_send.tv_sec - time_start_send.tv_sec) * 1000 +
                                    (time_end_send.tv_usec - time_start_send.tv_usec) / 1000;
    return -1; // Erro geral indicando falha após retransmissões
}

// Recebe uma mensagem usando o protocolo PowerUDP
int receive_message(char *buffer, int bufsize) {
    if (!protocol_initialized || udp_socket_internal < 0) { // Verificar inicialização
        fprintf(stderr, "[PowerUDP Error] Protocolo não inicializado para receive_message.\n");
        return -2;
    }

    power_udp_packet_t received_packet; // Pacote PowerUDP recebido
    struct sockaddr_in current_sender_addr; // Para guardar endereço do remetente
    socklen_t current_addr_len = sizeof(current_sender_addr);

    // Loop para apenas retornar pacotes DATA válidos para a aplicação.
    // Ignorará ACKs/NAKs perdidos (tratados pelo loop select de send_message)
    // e tratará da numeração de sequência.
    while(protocol_initialized) { // Verificar protocol_initialized na condição do loop
        memset(&received_packet, 0, sizeof(received_packet)); // Limpar pacote
        // Chamada bloqueante a recvfrom para esperar por pacotes
        ssize_t bytes_received = recvfrom(udp_socket_internal, &received_packet, sizeof(received_packet), 0,
                                          (struct sockaddr*)&current_sender_addr, &current_addr_len);

        if (!protocol_initialized) { // Verificar se o protocolo foi fechado enquanto bloqueado
             printf("[PowerUDP] receive_message: Protocolo fechado durante recvfrom.\n");
             return 0;
        }

        if (bytes_received < 0) { // Erro no recvfrom
            if (errno == EINTR) continue; // Interrompido por sinal, tentar recvfrom novamente
            // Imprimir erro apenas se o protocolo ainda estiver inicializado e não for um sinal de encerramento gracioso
            if(protocol_initialized) perror("[PowerUDP Error] recvfrom data packet");
            return -1;
        }
        if (bytes_received == 0) {
            printf("[PowerUDP Warning] recvfrom devolveu 0 bytes.\n");
            continue;
        }

        // Validação básica: deve ter pelo menos o tamanho de um cabeçalho
        if (bytes_received < (ssize_t)sizeof(power_udp_header_t)) {
            printf("[PowerUDP Warning] Pacote recebido demasiado curto (%zd bytes). Ignorando.\n", bytes_received);
            continue;
        }

        // Se for um pacote de dados
        if (received_packet.header.type == PACKET_TYPE_DATA) {
            uint32_t recv_seq_num_net = received_packet.header.sequence_number; // Manter em network order para ACK/NAK
            uint32_t recv_seq_num_host = ntohl(recv_seq_num_net); // Converter para host order para lógica
            uint16_t data_len = ntohs(received_packet.header.data_length); // Comprimento dos dados

            // Validar data_len contra o tamanho real do payload recebido e capacidade do buffer da aplicação
            if (data_len > (bytes_received - sizeof(power_udp_header_t))) {
                 fprintf(stderr, "[PowerUDP Warning] Inconsistência no tamanho do payload DATA (declarado %u, efetivo %ld). Ignorando.\n",
                        data_len, (long)(bytes_received - sizeof(power_udp_header_t)));
                 continue;
            }
            if (data_len == 0) { // Pacote de dados vazio
                printf("[PowerUDP Info] Pacote DATA (seq %u) recebido com payload vazio (len 0).\n", recv_seq_num_host);
            }
             if (data_len > (unsigned int)bufsize) { // Payload excede buffer da aplicação
                fprintf(stderr, "[PowerUDP Warning] Payload do pacote DATA (%u bytes) excede buffer da aplicação (%d bytes). Pacote descartado.\n", data_len, bufsize);
                // Enviar NAK se retransmissões estiverem ativadas
                if (internal_power_udp_state.retransmission_enabled && internal_power_udp_state.sequence_enabled) { // NAK implica sequência
                    power_udp_packet_t nak_packet;
                    nak_packet.header.type = PACKET_TYPE_NAK;
                    nak_packet.header.sequence_number = recv_seq_num_net; // NAK para a sequência recebida
                    nak_packet.header.data_length = 0;
                    sendto(udp_socket_internal, &nak_packet, sizeof(power_udp_header_t), 0,
                           (struct sockaddr*)&current_sender_addr, current_addr_len);
                    printf("[PowerUDP] NAK (seq %u, buffer overflow) enviado para %s:%d.\n", recv_seq_num_host, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port));
                }
                continue;
            }

            printf("[PowerUDP] Pacote DATA recebido (seq %u, len %u) de %s:%d.\n",
                   recv_seq_num_host, data_len,
                   inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port));

            int send_ack_flag = 0;      // Flag para indicar se um ACK deve ser enviado
            int accept_packet_flag = 0; // Flag para indicar se o pacote deve ser entregue à aplicação

            // Lógica de numeração de sequência
            if (internal_power_udp_state.sequence_enabled) {
                if (recv_seq_num_host == internal_power_udp_state.expected_recv_sequence_number) {
                    // Pacote está em ordem
                    accept_packet_flag = 1; // Aceitar pacote
                    send_ack_flag = 1;      // Enviar ACK para este pacote
                    internal_power_udp_state.expected_recv_sequence_number++; // Esperar pelo próximo
                } else if (recv_seq_num_host < internal_power_udp_state.expected_recv_sequence_number) {
                    // Pacote duplicado/antigo. Já processado.
                    // Enviar ACK novamente caso o ACK anterior tenha sido perdido (conforme RDT típico).
                    send_ack_flag = 1;
                    accept_packet_flag = 0; // Não entregar à aplicação novamente
                    printf("[PowerUDP Info] Pacote DATA duplicado/antigo (seq %u, esperado %u). Reenviando ACK.\n",
                           recv_seq_num_host, internal_power_udp_state.expected_recv_sequence_number);
                } else { // recv_seq_num_host > internal_power_udp_state.expected_recv_sequence_number (Falha detetada / Fora de ordem)
                    // Pacote está fora de ordem (pacote futuro). Rejeitar e enviar NAK.
                    if (internal_power_udp_state.retransmission_enabled) { // NAK só faz sentido se o emissor retransmitir
                        power_udp_packet_t nak_packet;
                        nak_packet.header.type = PACKET_TYPE_NAK;
                        // NAK para o número de sequência *recebido* fora de ordem
                        nak_packet.header.sequence_number = recv_seq_num_net;
                        nak_packet.header.data_length = 0;
                        sendto(udp_socket_internal, &nak_packet, sizeof(power_udp_header_t), 0,
                               (struct sockaddr*)&current_sender_addr, current_addr_len);
                        printf("[PowerUDP] Pacote DATA fora de ordem (seq %u, esperado %u). Enviado NAK para seq %u.\n",
                               recv_seq_num_host, internal_power_udp_state.expected_recv_sequence_number, recv_seq_num_host);
                    }
                    accept_packet_flag = 0; // Não aceitar pacote fora de ordem
                }
            } else { // Numeração de sequência desativada
                accept_packet_flag = 1; // Aceitar qualquer pacote de dados
                send_ack_flag = 1;      // Ainda enviar ACK se retransmissões estiverem ativadas
            }

            // Enviar ACK se necessário (e retransmissões estiverem globalmente ativadas)
            if (send_ack_flag && internal_power_udp_state.retransmission_enabled) {
                power_udp_packet_t ack_packet;
                ack_packet.header.type = PACKET_TYPE_ACK;
                // ACK para o número de sequência que foi realmente recebido e aceite (ou re-ACKed para duplicados)
                ack_packet.header.sequence_number = recv_seq_num_net;
                ack_packet.header.data_length = 0;

                // Simular perda de ACK
                if (internal_power_udp_state.packet_loss_probability > 0 && (rand() % 100) < (internal_power_udp_state.packet_loss_probability / 2) ) { // Simular perda de ACK com menor frequência que dados
                     printf("[PowerUDP SIMULATE] Pacote ACK (seq %u) para %s:%d PERDIDO.\n",
                           recv_seq_num_host, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port));
                } else { // Enviar ACK
                    sendto(udp_socket_internal, &ack_packet, sizeof(power_udp_header_t), 0,
                           (struct sockaddr*)&current_sender_addr, current_addr_len);
                     printf("[PowerUDP] ACK (seq %u) enviado para %s:%d.\n", recv_seq_num_host, inet_ntoa(current_sender_addr.sin_addr), ntohs(current_sender_addr.sin_port));
                }
            }

            // Se o pacote for aceite, copiar para o buffer da aplicação e retornar
            if (accept_packet_flag) {
                memcpy(buffer, received_packet.payload, data_len);
                buffer[data_len] = '\0';
                return data_len;
            }
            continue;
        } else { // Tipo de pacote desconhecido
            printf("[PowerUDP Warning] Pacote de tipo desconhecido (%d) recebido. Ignorando.\n", received_packet.header.type);
            continue; // Ignorar e esperar por um pacote DATA válido
        }
    }
    printf("[PowerUDP] receive_message: Saindo do loop principal, protocolo não inicializado.\n");
    return 0;
}

// Obtém estatísticas da última mensagem enviada pelo PowerUDP
int get_last_message_stats(int *retransmissions_out, int *delivery_time_ms_out) {
    if (!protocol_initialized) { // Verificar inicialização
        fprintf(stderr, "[PowerUDP Error] Protocolo não inicializado para get_last_message_stats.\n");
        if (retransmissions_out) *retransmissions_out = -1; // Indicar erro/sem dados
        if (delivery_time_ms_out) *delivery_time_ms_out = -1;
        return -1; // Erro
    }
    if (!stats_are_valid_internal) { // Verificar se há estatísticas válidas
        // Nenhuma tentativa de envio concluída (com sucesso ou não) desde init ou último reset.
        fprintf(stderr, "[PowerUDP Info] Nenhuma estatística de mensagem disponível ainda (nenhum envio completo).\n");
        if (retransmissions_out) *retransmissions_out = -1;
        if (delivery_time_ms_out) *delivery_time_ms_out = -1;
        return -1; // Sem estatísticas válidas ainda, ou erro
    }

    // Fornecer as estatísticas
    if (retransmissions_out) *retransmissions_out = last_msg_retransmissions;

    if (delivery_time_ms_out) {
        if (last_msg_attempt_successful) { // Se a última tentativa foi bem-sucedida
            *delivery_time_ms_out = last_msg_delivery_time_ms;
        } else { // Se falhou
            *delivery_time_ms_out = -1;
        }
    }
    return 0;
}

// Simula a perda de pacotes para testar retransmissões
void inject_packet_loss(int probability) {
    if (!protocol_initialized) { // Verificar inicialização
        fprintf(stderr, "[PowerUDP Error] Protocolo não inicializado para inject_packet_loss.\n");
        return;
    }
    if (probability < 0) probability = 0;
    if (probability > 100) probability = 100;
    internal_power_udp_state.packet_loss_probability = probability; // Definir probabilidade de perda
    printf("[PowerUDP] Simulação de perda de pacotes definida para %d%%.\n", probability);
}
