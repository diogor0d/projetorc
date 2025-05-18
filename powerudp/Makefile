# Makefile para o projeto PowerUDP

# Compilador
CC = gcc

# Flags de compilação
CFLAGS = -Wall -Wextra -g -O2 -std=c11

# Flags de linkagem
LDFLAGS = -pthread

# Ficheiros fonte
CLIENT_SRCS = client.c powerudp.c
SERVER_SRCS = server.c powerudp.c

# Nomes dos executáveis
CLIENT_TARGET = cliente
SERVER_TARGET = servidor

# Regra padrão: compila tudo
all: $(CLIENT_TARGET) $(SERVER_TARGET)

# Regra para compilar o client diretamente
$(CLIENT_TARGET): $(CLIENT_SRCS)
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $@ $(LDFLAGS)
	@echo "Cliente '$(CLIENT_TARGET)' compilado com sucesso."

# Regra para compilar o servidor diretamente
$(SERVER_TARGET): $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o $@ $(LDFLAGS)
	@echo "Servidor '$(SERVER_TARGET)' compilado com sucesso."

# Regra para limpar os ficheiros compilados
clean:
	@echo "A limpar ficheiros compilados..."
	rm -f $(CLIENT_TARGET) $(SERVER_TARGET)
	@echo "Limpeza concluída."

# Phony targets são targets que não representam ficheiros
.PHONY: all clean