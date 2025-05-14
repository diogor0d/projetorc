#ifndef MSG_STRUCTS_H
#define MSG_STRUCTS_H

#include <stdint.h> // Para tipos como uint8_t, uint16_t, uint32_t
#include <arpa/inet.h> // Para struct sockaddr_in

// -- Constantes Globais --
#define MAX_PAYLOAD_SIZE 1024 // Tamanho máximo do payload da aplicação numa mensagem PowerUDP
#define PSK_DEFAULT "RC_Project_2024_2025_PowerUDP_PSK" // Chave pré-partilhada (Pre-Shared Key)
#define MAX_PSK_LEN 64

// Portas Padrão (podem ser alteradas ou tornadas configuráveis)
#define SERVER_TCP_PORT 8000        // Porta TCP no servidor para registo e pedidos de config
#define POWER_UDP_PORT_CLIENT 8001  // Porta UDP que os clientes usam para comunicar via PowerUDP
#define MULTICAST_PORT 8002         // Porta para as mensagens multicast de configuração

// Endereço Multicast Padrão (deve ser um endereço válido para multicast, ex: 224.0.0.0 a 239.255.255.255)
// Este endereço deve ser configurado nos routers para permitir o encaminhamento (ip pim sparse-dense-mode)
#define MULTICAST_ADDRESS "239.0.0.1"

#define MAX_BUFFER_SIZE (MAX_PAYLOAD_SIZE + sizeof(power_udp_header_t) + 100) // Buffer genérico

typedef struct {
    uint8_t enable_retrans;
    uint8_t enable_backoff;
    uint8_t enable_seq;
    uint16_t base_timeout;
    uint8_t max_retries;
} ConfigMessage;

struct RegisterMessage {
	char psk[64]; // Chave pré-definida para autenticação
};

// Tipos de pacotes PowerUDP
typedef enum {
    PACKET_TYPE_DATA, // Dados da aplicação
    PACKET_TYPE_ACK,  // Confirmação de receção
    PACKET_TYPE_NAK   // Confirmação negativa (erro de sequência, etc.)
} power_udp_packet_type_t;

typedef struct {
    uint32_t sequence_number;       // Número de sequência para ordenação
    power_udp_packet_type_t type;   // Tipo de pacote (DATA, ACK, NAK)
    uint16_t data_length;           // Comprimento do payload (dados da aplicação)
    // Poderia adicionar um checksum aqui se a integridade do UDP não for suficiente
    // uint16_t checksum;
} power_udp_header_t;

// Estrutura completa de um pacote PowerUDP (cabeçalho + payload)
typedef struct {
    power_udp_header_t header;
    char payload[MAX_PAYLOAD_SIZE];
} power_udp_packet_t;

// Estrutura para estatísticas da última mensagem enviada (API get_last_message_stats)
struct PowerUDPStats {
    int retransmissions;
    int delivery_time_ms; // Tempo desde o primeiro envio até ao ACK
    int successful;       // 1 se ACK recebido, 0 caso contrário (timeout após max_retries)
};

// Configuração interna do protocolo PowerUDP no cliente
// Esta estrutura será gerida pela biblioteca PowerUDP e atualizada via multicast.
typedef struct {
    uint8_t retransmission_enabled;
    uint8_t backoff_enabled;
    uint8_t sequence_enabled;
    uint16_t base_timeout_ms;
    uint8_t max_retries;
    // Estado interno
    uint32_t current_send_sequence_number;
    uint32_t expected_recv_sequence_number;
    // Para simulação de perda
    int packet_loss_probability; // Percentagem (0-100)
} power_udp_config_t;

#endif