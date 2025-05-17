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


# MUDAR PARA OS DIRETORIOS NOS CONTAINERS DO GNS3
SERVER_OUT_DIR = ../gns/projetorc/project-files/docker/7adc1767-9cdb-4ea9-aa9d-b3919e7e1c9f/home/servidor
CLIENT1_OUT_DIR = ../gns/projetorc/project-files/docker/2a41ccef-db04-4e50-aa3e-dcd9e7368a9c/home/cliente
CLIENT2_OUT_DIR = ../gns/projetorc/project-files/docker/8031c9a9-60ae-4307-ac13-e187a88f2dd4/home/cliente
CLIENT3_OUT_DIR = ../gns/projetorc/project-files/docker/f962cb57-facb-43ee-b43c-6191f209946f/home/cliente

$(SERVER_TARGET): $(SERVER_SRCS)
	mkdir -p $(dir $(SERVER_OUT_DIR))
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o $(SERVER_OUT_DIR) $(LDFLAGS)
	@echo "Servidor '$(SERVER_TARGET)' compilado com sucesso em $(SERVER_OUT_DIR)."
	mkdir -p $(dir $(CLIENT1_OUT_DIR))
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $(CLIENT1_OUT_DIR) $(LDFLAGS)
	@echo "Cliente 1 compilado com sucesso em $(CLIENT1_OUT_DIR)."
	mkdir -p $(dir $(CLIENT2_OUT_DIR))
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $(CLIENT2_OUT_DIR) $(LDFLAGS)
	@echo "Cliente 2 compilado com sucesso em $(CLIENT2_OUT_DIR)."
	mkdir -p $(dir $(CLIENT3_OUT_DIR))
	$(CC) $(CFLAGS) $(CLIENT_SRCS) -o $(CLIENT3_OUT_DIR) $(LDFLAGS)
	@echo "Cliente 3 compilado com sucesso em $(CLIENT3_OUT_DIR)."


# Regra para limpar os ficheiros compilados
clean:
	@echo "A limpar ficheiros compilados..."
	rm ${SERVER_OUT_DIR}
	@echo "Limpeza concluída."

# Phony targets são targets que não representam ficheiros
.PHONY: all clean