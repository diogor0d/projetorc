/*
    Projeto de Redes de Comunicação 2024/2025 - PowerUDP
    Diogo Nuno Fonseca Rodrigues 2022257625
    Guilherme Teixeira Gonçalves Rosmaninho 2022257636
*/

#ifndef MSG_STRUCTS_H
#define MSG_STRUCTS_H

#include <stdint.h>    // para tipos como uint8_t, uint16_t, uint32_t
#include <arpa/inet.h> // para struct sockaddr_in

// constantes
#define MAX_PAYLOAD_SIZE 1024           // tamanho máximo do payload da aplicação numa mensagem PowerUDP
#define PSK_DEFAULT "projetorc20242025" // psk default
#define MAX_PSK_LEN 64

// portas default
#define SERVER_TCP_PORT 8000       // porta TCP no servidor para registo e pedidos de alteracao config
#define POWER_UDP_PORT_CLIENT 8001 // porta UDP (default!) que os clientes usam para comunicar via PowerUDP
#define MULTICAST_PORT 8002        // porta para as mensagens multicast de configuração

// grupo multicast
#define MULTICAST_ADDRESS "239.1.2.3"

#define MAX_BUFFER_SIZE (MAX_PAYLOAD_SIZE + sizeof(power_udp_header_t) + 100) // buffer generico

typedef struct
{
    uint8_t server_shutdown_signal; // 1 se o servidor cessou atividade, 0 caso contrário
    uint8_t enable_retrans;
    uint8_t enable_backoff;
    uint8_t enable_seq;
    uint16_t base_timeout;
    uint8_t max_retries;
} ConfigMessage;

struct RegisterMessage
{
    char psk[64]; // psk
};

typedef enum
{
    PACKET_TYPE_DATA,
    PACKET_TYPE_ACK,
    PACKET_TYPE_NAK
} power_udp_packet_type_t;

typedef struct
{
    uint32_t sequence_number;     // número de sequência para ordenação
    power_udp_packet_type_t type; // tipo de pacote (DATA, ACK, NAK)
    uint16_t data_length;         // tamanho do payload (dados)
} power_udp_header_t;

// estrutura de um pacote PowerUDP
typedef struct
{
    power_udp_header_t header;
    char payload[MAX_PAYLOAD_SIZE];
} power_udp_packet_t;

// estrutura para armazenamento das estatisticas
struct PowerUDPStats
{
    int retransmissions;
    int delivery_time_ms; // Tempo desde o primeiro envio até ao ACK
    int successful;       // 1 se ACK recebido, 0 caso contrário (timeout após max_retries)
};

// armazenamento do estado interno do PowerUDP
typedef struct
{
    uint8_t retransmission_enabled;
    uint8_t backoff_enabled;
    uint8_t sequence_enabled;
    uint16_t base_timeout_ms;
    uint8_t max_retries;
    uint32_t current_send_sequence_number;
    uint32_t expected_recv_sequence_number;
    int packet_loss_probability;
} power_udp_config_t;

#endif