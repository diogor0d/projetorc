#ifndef POWERUDP_H
#define POWERUDP_H

#include <stdint.h>
#include "msg_structs.h"

// Inicializa a stack de comunicação e regista-se no servidor
int init_protocol(const char *server_ip, int server_port, const char *psk);
// Termina a stack de comunicação e apaga o registo no servidor
void close_protocol();
// Solicita mudança na configuração do protocolo ao servidor
int request_protocol_config(int enable_retransmission, int enable_backoff, int enable_sequence, uint16_t base_timeout, uint8_t max_retries);

// Envia uma mensagem UDP
int send_message(const char *destination_ip, int destination_port, const char *message, int len);

// Recebe uma mensagem UDP
int receive_message(char *buffer, int bufsize);

// Obtém estatísticas da última mensagem enviada
int get_last_message_stats(int *retransmissions, int *delivery_time);
// Simula a perda de pacotes para testar retransmissões
void inject_packet_loss(int probability);

#endif