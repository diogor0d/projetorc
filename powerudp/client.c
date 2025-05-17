#define _DEFAULT_SOURCE // para o usleep
#include "msg_structs.h"
#include "powerudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define MAX_INPUT_LINE 2048
#define MAX_MESSAGE_USER 1000 // tamanho maximo do payload de uma mensagem

// variável global para controlar a execução da thread de receção
pthread_t receiver_thread_id;
volatile int keep_receiver_thread_running = 0;
volatile sig_atomic_t sigint_received = 0; // flag para o sigint

// handler
void handle_sigint(int sig)
{
    (void)sig; // parametro nao usado
    printf("\n[Cliente] SIGINT recebido. A terminar...\n");
    sigint_received = 1;
    // terminar a thread de receção e fechar o protocolo
    if (keep_receiver_thread_running)
    {
        keep_receiver_thread_running = 0;
    }
    close_protocol();
    printf("[Cliente] Todos os recursos foram limpos.\n");
    exit(0);
}

// função para a thread que recebe mensagens PowerUDP
void *power_udp_receiver_thread_func(void *arg)
{
    (void)arg;
    char recv_buffer[MAX_PAYLOAD_SIZE + 1];
    int bytes_received;
    char sender_ip[INET_ADDRSTRLEN]; // Buffer for sender's IP string
    uint16_t sender_port;            // For sender's port

    printf("[Cliente RX Thread] À escuta por novas mensagens PowerUDP.\n");

    while (keep_receiver_thread_running)
    {
        memset(recv_buffer, 0, sizeof(recv_buffer));
        sender_ip[0] = '\0'; // inicializar buffer do ip do remetente
        sender_port = 0;     // inicializar porta do remetente

        // aguardar por mensagens
        bytes_received = receive_message(recv_buffer, MAX_PAYLOAD_SIZE, sender_ip, INET_ADDRSTRLEN, &sender_port);

        if (!keep_receiver_thread_running)
            break;

        if (bytes_received > 0)
        {
            recv_buffer[bytes_received] = '\0';

            if (sender_ip[0] != '\0' && sender_port != 0)
            {
                printf("\n<Mensagem Recebida de %s:%u> %s\n> ", sender_ip, sender_port, recv_buffer);
            }
            else
            {
                printf("\n<Mensagem Recebida> %s\n> ", recv_buffer); // failsafe caso não haja IP/Porta
            }
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
    printf("Uso: %s <IP_Servidor_Config> <Porta_TCP_Servidor_Config> [PSK]\n", prog_name);
    printf("  PSK (opcional): Chave pré-partilhada. Padrão: \"%s\"\n", PSK_DEFAULT);
    printf("Exemplo: %s 192.168.1.100 %d MySecurePSK\n", prog_name, SERVER_TCP_PORT);
}

void print_commands()
{
    printf("\nComandos disponíveis:\n");
    printf("  send <IP_Destino>:<Porta_Destino> <mensagem> - Envia uma mensagem PowerUDP\n");
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
    if (argc < 3 || argc > 4)
    { // programa + ip do servidor + porta do servidor (+ PSK opcional)
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, SIG_IGN); // ignorar sigint para prevenir termino inseguro do processo

    const char *server_ip = argv[1];
    int server_tcp_port = atoi(argv[2]);
    const char *psk = (argc == 4) ? argv[3] : PSK_DEFAULT;

    printf("[Cliente] A iniciar...\n");
    printf("  Servidor de Configuração: %s:%d\n", server_ip, server_tcp_port);
    printf("  Porta UDP Cliente (PowerUDP): %d\n", POWER_UDP_PORT_CLIENT);
    printf("  PSK: %s\n", psk);
    printf("  Grupo Multicast: %s:%d\n", MULTICAST_ADDRESS, MULTICAST_PORT);

    if (init_protocol(server_ip, server_tcp_port, psk) != 0)
    {
        fprintf(stderr, "[Cliente Error] Falha ao inicializar o protocolo PowerUDP.\n");
        return 1;
    }
    printf("[Cliente] Protocolo PowerUDP inicializado com sucesso.\n");

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

    // tratar sigint apos inicio seguro
    signal(SIGINT, handle_sigint);

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
            // Formato: send <IP_Destino>:<Porta_Destino> <mensagem>
            char *ip_port_str = strtok(input_line + strlen(command) + 1, " ");
            char *msg_start = strtok(NULL, ""); // Resto da linha

            if (ip_port_str && msg_start && strlen(msg_start) > 0)
            {
                char *dest_ip_str = strtok(ip_port_str, ":");
                char *dest_port_str = strtok(NULL, ":");

                if (dest_ip_str && dest_port_str)
                {
                    int dest_port_int = atoi(dest_port_str);
                    if (dest_port_int <= 0 || dest_port_int > 65535)
                    {
                        printf("Porta de destino inválida: %s\n", dest_port_str);
                        printf("> ");
                        fflush(stdout);
                        continue;
                    }

                    if (strlen(msg_start) > MAX_MESSAGE_USER - 1)
                    {
                        printf("Mensagem demasiado longa (max %d caracteres).\n", MAX_MESSAGE_USER - 1);
                    }
                    else
                    {
                        strncpy(message_payload, msg_start, MAX_MESSAGE_USER - 1);
                        message_payload[MAX_MESSAGE_USER - 1] = '\0';
                        printf("[Cliente] A enviar \"%s\" para %s:%d...\n", message_payload, dest_ip_str, dest_port_int);
                        // Chamada a send_message MODIFICADA
                        int bytes_sent = send_message(dest_ip_str, dest_port_int, message_payload, strlen(message_payload));
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
                            printf("[Cliente Error] Protocolo não inicializado.\n");
                        }
                        else
                        {
                            printf("[Cliente Error] Erro desconhecido ao enviar mensagem (%d).\n", bytes_sent);
                        }
                    }
                }
                else
                {
                    printf("Formato inválido para IP:Porta. Use: send <IP_Destino>:<Porta_Destino> <mensagem>\n");
                }
            }
            else
            {
                printf("Uso: send <IP_Destino>:<Porta_Destino> <mensagem>\n");
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
    // terminar a thread de receção e fechar o protocolo
    if (keep_receiver_thread_running)
    {
        keep_receiver_thread_running = 0;
    }
    close_protocol();
    pthread_join(receiver_thread_id, NULL);
    printf("[Cliente] Todos os recursos foram limpos.\n");

    return 0;
}