#define _POSIX_C_SOURCE 201112L
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <stdlib.h>

#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/types.h>

int main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return EX_USAGE;
    }

    struct addrinfo hints = {0};
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // Hints that we want to bind to a socket in server scenario
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *results;
    int err = getaddrinfo(NULL, argv[1], &hints, &results);
    if(err != 0) {
        fprintf(stderr, "Cannot get address: %s\n", gai_strerror(err));
        return EX_NOHOST;
    }

    int sd = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    if(sd < 0) {
        perror("Could not create socket");
        freeaddrinfo(results);
        return EX_OSERR;
    }

    err = bind(sd, results->ai_addr, results->ai_addrlen);
    if(err < 0) {
        perror("Could not bind socket");
        close(sd);
        freeaddrinfo(results);
        return EX_OSERR;
    }
    freeaddrinfo(results);

    // Backlog of 5 is typical
    err = listen(sd, 5);
    if(err < 0) {
        perror("Could not listen on socket");
        close(sd);
        return EX_OSERR;
    }

    for(;;) {
        struct sockaddr_storage client;
        socklen_t client_sz = sizeof(client);
        char addr[INET6_ADDRSTRLEN];

        int remote = accept(sd, (struct sockaddr *)&client, &client_sz);

        unsigned short port = 0;
        if(client.ss_family == AF_INET6) {
            inet_ntop(client.ss_family,
                    &((struct sockaddr_in6 *)&client)->sin6_addr, addr, sizeof(addr));
            port = ntohs(((struct sockaddr_in6 *)&client)->sin6_port);
        } else {
            inet_ntop(client.ss_family, &((struct sockaddr_in *)&client)->sin_addr,
                    addr, sizeof(addr));
            port = ntohs(((struct sockaddr_in *)&client)->sin_port);
        }
        printf("Received from %s:%hu\n", addr, port);

        pid_t child_pid = fork();
        if(child_pid < 0) {
            perror("Could not fork process");
            close(remote);
            continue;
        } else if(child_pid == 0) {
            // Child process
            close(sd);  // Close the listening socket in the child process

            // seems like a reasonable buffer size
            size_t buffer_size = 1024;
            char *buffer = malloc(buffer_size);
            if(buffer == NULL) {
                perror("Failed to allocate memory for buffer");
                close(remote);
                return EX_OSERR;
            }

            size_t total_received = 0;
            ssize_t received;
            while((received = recv(remote, buffer + total_received, buffer_size - total_received - 1, 0)) > 0) {
                total_received += (size_t)received;
                if(total_received >= buffer_size - 1) {
                    // Expand the buffer size using realloc
                    buffer_size *= 2;
                    char *new_buffer = realloc(buffer, buffer_size);
                    if(new_buffer == NULL) {
                        perror("Failed to reallocate memory for buffer");
                        free(buffer);
                        close(remote);
                        return EX_OSERR;
                    }
                    buffer = new_buffer;
                }
            }

            if(received < 0) {
                perror("Unable to receive");
            }

            buffer[total_received] = '\0';  // Null-terminate the received message
            printf("Received message:\n%s\n", buffer);

            free(buffer);  // Free the dynamically allocated buffer
            close(remote);
            puts("");

            return 0;  // Exit the child process
        } else {
            // Parent process
            close(remote);  // Close the remote socket in the parent process
        }
    }

    close(sd);
}
