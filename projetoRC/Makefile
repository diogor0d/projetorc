# Makefile para o projeto PowerUDP

# Compilador
CC = gcc

# Flags de compilação
# -Wall: Ativa a maioria dos warnings
# -Wextra: Ativa warnings adicionais
# -g: Adiciona informação de debugging
# -O2: Nível de otimização (pode ser -O0 para debugging mais fácil)
# -std=c11: Especifica o padrão C (ou c99 se preferir)
CFLAGS = -Wall -Wextra -g -O2 -std=c11

# Flags de linkagem
# -pthread: Necessário para compilar e linkar com pthreads
LDFLAGS = -pthread

# Ficheiros fonte
CLIENT_SRCS = client.c powerudp.c
SERVER_SRCS = server.c powerudp.c # powerudp.c é usado por ambos

# Ficheiros objeto correspondentes
# $(CLIENT_SRCS:.c=.o) substitui .c por .o em cada nome de ficheiro
# No entanto, como powerudp.o é partilhado, vamos listá-los explicitamente
# ou criar powerudp.o separadamente.

# Ficheiros objeto
POWER_UDP_OBJ = powerudp.o
CLIENT_OBJS = client.o $(POWER_UDP_OBJ)
SERVER_OBJS = server.o $(POWER_UDP_OBJ)

# Nomes dos executáveis
CLIENT_TARGET = cliente
SERVER_TARGET = servidor

# Regra padrão: compila tudo
all: $(CLIENT_TARGET) $(SERVER_TARGET)

# Regra para compilar o client
$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Cliente '$(CLIENT_TARGET)' compilado com sucesso."

# Regra para compilar o servidor
$(SERVER_TARGET): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Servidor '$(SERVER_TARGET)' compilado com sucesso."

# Regras genéricas para compilar ficheiros .c para .o
# Para client.c -> client.o
client.o: client.c msg_structs.h powerudp.h
	$(CC) $(CFLAGS) -c client.c -o client.o

# Para server.c -> server.o
server.o: server.c msg_structs.h
	$(CC) $(CFLAGS) -c server.c -o server.o

# Para powerudp.c -> powerudp.o
powerudp.o: powerudp.c powerudp.h msg_structs.h
	$(CC) $(CFLAGS) -c powerudp.c -o powerudp.o

# Regra para limpar os ficheiros compilados
clean:
	@echo "A limpar ficheiros compilados..."
	rm -f $(CLIENT_TARGET) $(SERVER_TARGET) *.o
	@echo "Limpeza concluída."

# Phony targets são targets que não representam ficheiros
.PHONY: all clean