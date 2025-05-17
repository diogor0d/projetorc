#define _DEFAULT_SOURCE // Added to make usleep available
#include "msg_structs.h"
#include "powerudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h> // Para sockaddr_in
#include <netinet/in.h> // Para sockaddr_in
#include <arpa/inet.h>  // Para inet_ntoa

#define MAX_INPUT_LINE 2048
#define MAX_MESSAGE_USER 1000 // Payload da mensagem do utilizador

// Variável global para controlar a execução da thread de receção
volatile int keep_receiver_thread_running = 0;

// Função para a thread que recebe mensagens PowerUDP
void *power_udp_receiver_thread_func(void *arg)
{
    (void)arg;
    char recv_buffer[MAX_PAYLOAD_SIZE + 1];
    int bytes_received;
    // struct sockaddr_in sender_addr; // Não mais preenchido por receive_message
    // socklen_t sender_addr_len = sizeof(sender_addr);

    printf("[Cliente RX Thread] Pronta para receber mensagens PowerUDP.\n");

    while (keep_receiver_thread_running)
    {
        memset(recv_buffer, 0, sizeof(recv_buffer));
        // Chamada a receive_message MODIFICADA
        bytes_received = receive_message(recv_buffer, MAX_PAYLOAD_SIZE);

        if (!keep_receiver_thread_running)
            break;

        if (bytes_received > 0)
        {
            recv_buffer[bytes_received] = '\0';
            // Não temos mais o IP/Porta do remetente diretamente de receive_message
            printf("\n<Mensagem Recebida> %s\n> ", recv_buffer);
            fflush(stdout);
        }
        else if (bytes_received == 0)
        {
            printf("[Cliente RX Thread] receive_message retornou 0. A thread vai terminar.\n");
            break;
        }
        else if (bytes_received == -1)
        {
            // fprintf(stderr, "[Cliente RX Thread] Erro em receive_message. Continuando...\n");
            usleep(100000);
        }
    }
    printf("[Cliente RX Thread] Terminada.\n");
    return NULL;
}

void print_usage(const char *prog_name)
{
    // Argumentos MODIFICADOS para init_protocol
    printf("Uso: %s <IP_Servidor_Config> <Porta_TCP_Servidor_Config> [PSK]\n", prog_name);
    printf("  PSK (opcional): Chave pré-partilhada. Padrão: \"%s\"\n", PSK_DEFAULT);
    // A porta UDP local do cliente e detalhes do multicast são agora geridos internamente pela biblioteca PowerUDP
    // usando constantes de protocol.h (POWER_UDP_PORT_CLIENT, MULTICAST_ADDRESS, MULTICAST_PORT)
    printf("Exemplo: %s 192.168.1.100 %d MySecurePSK\n", prog_name, SERVER_TCP_PORT);
}

void print_commands()
{
    printf("\nComandos disponíveis:\n");
    // Comando send MODIFICADO (sem porta de destino explícita, assume-se porta PowerUDP padrão)
    printf("  send <IP_Destino> <mensagem> - Envia uma mensagem PowerUDP (para a porta PowerUDP padrão)\n");
    printf("  config <retrans:0|1> <backoff:0|1> <seq:0|1> <timeout_ms> <retries> - Pede alteração de config\n");
    printf("  stats - Mostra estatísticas da última mensagem enviada\n");
    printf("  loss <probabilidade_percentual> - Simula perda de pacotes (0-100)\n");
    printf("  help - Mostra esta ajuda\n");
    printf("  quit - Fecha o cliente\n");
    printf("> ");
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    // Argumentos MODIFICADOS
    if (argc < 3 || argc > 4)
    { // Progname + ServerIP + ServerPort (+ PSK opcional)
        print_usage(argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int server_tcp_port = atoi(argv[2]);
    const char *psk = (argc == 4) ? argv[3] : PSK_DEFAULT;

    printf("[Cliente] A iniciar...\n");
    printf("  Servidor de Configuração: %s:%d\n", server_ip, server_tcp_port);
    // A porta UDP local do cliente e multicast são geridas internamente por init_protocol
    printf("  Porta UDP Cliente Local (PowerUDP): %d (padrão)\n", POWER_UDP_PORT_CLIENT);
    printf("  PSK: %s\n", psk);
    printf("  Grupo Multicast Config: %s:%d (padrão)\n", MULTICAST_ADDRESS, MULTICAST_PORT);

    // Chamada a init_protocol+
    if (init_protocol(server_ip, server_tcp_port, psk) != 0)
    {
        fprintf(stderr, "[Cliente Error] Falha ao inicializar o protocolo PowerUDP.\n");
        return 1;
    }
    printf("[Cliente] Protocolo PowerUDP inicializado com sucesso.\n");

    pthread_t receiver_thread_id;
    keep_receiver_thread_running = 1;
    if (pthread_create(&receiver_thread_id, NULL, power_udp_receiver_thread_func, NULL) != 0)
    {
        fprintf(stderr, "[Cliente Error] Falha ao criar a thread de receção PowerUDP.\n");
        close_protocol();
        return 1;
    }

    char input_line[MAX_INPUT_LINE];
    char command[64];
    char message_payload[MAX_MESSAGE_USER];

    print_commands();

    while (fgets(input_line, sizeof(input_line), stdin) != NULL)
    {
        input_line[strcspn(input_line, "\n")] = 0;
        if (strlen(input_line) == 0)
        {
            printf("> ");
            fflush(stdout);
            continue;
        }
        int num_parsed = sscanf(input_line, "%63s", command);
        if (num_parsed <= 0)
        {
            printf("> ");
            fflush(stdout);
            continue;
        }

        if (strcmp(command, "quit") == 0)
        {
            printf("[Cliente] A terminar...\n");
            break;
        }
        else if (strcmp(command, "help") == 0)
        {
            print_commands();
        }
        else if (strcmp(command, "send") == 0)
        {
            // Formato: send <IP_Destino> <mensagem>
            char *dest_ip_str = strtok(input_line + strlen(command) + 1, " ");
            char *msg_start = strtok(NULL, ""); // Resto da linha

            if (dest_ip_str && msg_start && strlen(msg_start) > 0)
            {
                if (strlen(msg_start) > MAX_MESSAGE_USER - 1)
                {
                    printf("Mensagem demasiado longa (max %d caracteres).\n", MAX_MESSAGE_USER - 1);
                }
                else
                {
                    strncpy(message_payload, msg_start, MAX_MESSAGE_USER - 1);
                    message_payload[MAX_MESSAGE_USER - 1] = '\0';
                    printf("[Cliente] A enviar \"%s\" para %s (porta PowerUDP padrão %d)...\n", message_payload, dest_ip_str, POWER_UDP_PORT_CLIENT);
                    // Chamada a send_message
                    int bytes_sent = send_message(dest_ip_str, message_payload, strlen(message_payload));
                    if (bytes_sent > 0)
                    {
                        printf("[Cliente] Mensagem enviada com sucesso (%d bytes).\n", bytes_sent);
                    }
                    else if (bytes_sent == -1)
                    {
                        printf("[Cliente Error] Falha ao enviar mensagem após todas as tentativas.\n");
                    }
                    else if (bytes_sent == -2)
                    {
                        printf("[Cliente Error] Protocolo não inicializado (erro interno?).\n");
                    }
                    else
                    {
                        printf("[Cliente Error] Erro desconhecido ao enviar mensagem (%d).\n", bytes_sent);
                    }
                }
            }
            else
            {
                printf("Uso: send <IP_Destino> <mensagem>\n");
            }
        }
        else if (strcmp(command, "config") == 0)
        {
            int retrans, backoff, seq, timeout, retries_val;
            if (sscanf(input_line, "%*s %d %d %d %d %d", &retrans, &backoff, &seq, &timeout, &retries_val) == 5)
            {
                if (request_protocol_config(retrans, backoff, seq, (uint16_t)timeout, (uint8_t)retries_val) == 0)
                {
                    printf("[Cliente] Pedido de configuração enviado ao servidor.\n");
                }
                else
                {
                    printf("[Cliente Error] Falha ao enviar pedido de configuração.\n");
                }
            }
            else
            {
                printf("Uso: config <retrans:0|1> <backoff:0|1> <seq:0|1> <timeout_ms> <retries>\n");
            }
        }
        else if (strcmp(command, "stats") == 0)
        {
            int retransmissions_val;
            int delivery_time_val;
            if (get_last_message_stats(&retransmissions_val, &delivery_time_val) == 0)
            {
                printf("[Cliente] Estatísticas da última mensagem enviada:\n");
                printf("  Retransmissões: %d\n", retransmissions_val);
                if (delivery_time_val >= 0)
                {
                    printf("  Tempo de Entrega: %d ms (Sucesso)\n", delivery_time_val);
                }
                else
                {
                    printf("  Entrega Falhou (ou sem tentativa completa). Tempo registado: %d ms\n", delivery_time_val);
                }
            }
            else
            {
                printf("[Cliente] Nenhuma estatística disponível ou erro ao obter.\n");
            }
        }
        else if (strcmp(command, "loss") == 0)
        {
            int probability;
            if (sscanf(input_line, "%*s %d", &probability) == 1)
            {
                inject_packet_loss(probability);
            }
            else
            {
                printf("Uso: loss <probabilidade_percentual (0-100)>\n");
            }
        }
        printf("> ");
        fflush(stdout);
    }
    // Terminar a thread de receção e fechar o protocolo
    if (keep_receiver_thread_running)
    {
        keep_receiver_thread_running = 0;
        // A thread de receção pode estar bloqueada em receive_message.
        // close_protocol() fechará o socket UDP, o que deve fazer receive_message retornar.
    }
    close_protocol();
    pthread_join(receiver_thread_id, NULL);
    printf("[Cliente] Todos os recursos foram libertados.\n");

    return 0;
}