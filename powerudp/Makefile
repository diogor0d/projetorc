# Makefile para o projeto PowerUDP


CC = gcc

CFLAGS = -Wall -Wextra -g -O2 -std=c11


LDFLAGS = -pthread


CLIENT_SRCS = client.c powerudp.c
SERVER_SRCS = server.c powerudp.c


CLIENT_TARGET = cliente
SERVER_TARGET = servidor

all: $(CLIENT_TARGET) $(SERVER_TARGET)


# MUDAR PARA OS DIRETORIOS NOS CONTAINERS DO GNS3 (para facilidade de iteracao)
SERVER_OUT_DIR = ../gns/projetorc/project-files/docker/7adc1767-9cdb-4ea9-aa9d-b3919e7e1c9f/home/servidor
CLIENT1_OUT_DIR = ../gns/projetorc/project-files/docker/2a41ccef-db04-4e50-aa3e-dcd9e7368a9c/home/cliente
CLIENT2_OUT_DIR = ../gns/projetorc/project-files/docker/8031c9a9-60ae-4307-ac13-e187a88f2dd4/home/cliente
CLIENT3_OUT_DIR = ../gns/projetorc/project-files/docker/f962cb57-facb-43ee-b43c-6191f209946f/home/cliente

$(SERVER_TARGET): $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o ${CLIENT_TARGET} $(LDFLAGS)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o ${SERVER_TARGET} $(LDFLAGS)

	# remove esta parte se necessario
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


clean:
	@echo "A limpar ficheiros compilados..."
	rm ${SERVER_OUT_DIR}
	rm ${CLIENT1_OUT_DIR}
	rm ${CLIENT2_OUT_DIR}
	rm ${CLIENT3_OUT_DIR}
	@echo "Ficheiros compilados removidos."
	@echo "Limpeza concluída."

.PHONY: all clean