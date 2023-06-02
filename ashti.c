#define _POSIX_C_SOURCE 201112L
#define _XOPEN_SOURCE 800
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <stdlib.h>

#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/types.h>

char *uid_to_str(uid_t uid);
int validate_root_dir(const char *directory);
char *extract_filename(const char *request, int *err);
bool validate_request_method(const char *request);

int main(int argc, char *argv[])
{
    if(argc == 1 || argc > 2) {
        fprintf(stderr, "Usage: %s server_root\n", argv[0]);
        return EX_USAGE;
    }
    int rc = validate_root_dir(argv[1]);
    if (rc == -1) {
        return EX_USAGE;
    } else if (rc == EX_OSERR) {
        perror("Failed to allocate memory for buffer");
        return EX_OSERR;
    }

    struct addrinfo hints = {0};
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // Hints that we want to bind to a socket in server scenario
    hints.ai_flags = AI_PASSIVE;

    uid_t uid = getuid();
    char *uid_str = uid_to_str(uid);
    if (!uid_str) {
        perror("Failed to allocate memory for buffer");
        return EX_OSERR;
    }
    struct addrinfo *results;
    int err = getaddrinfo(NULL, uid_str, &hints, &results);
    free(uid_str);
    if (err != 0) {
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
                // Error?
            }

            // Potentially sometimes overwrites a single char?
            buffer[total_received] = '\0';  // Null-terminate the received message
            printf("Received message:\n%s\n", buffer);
            int err = 0;
            char *filename = extract_filename(buffer, &err);
            if (!filename) {
                printf("error2");
                // error handling for 400 error and malloc issues
            }
            

            free(buffer);  // Free the dynamically allocated buffer
            close(remote);
            putchar('\n');

            return 0;  // Exit the child process
        } else {
            // Parent process
            close(remote);  // Close the remote socket in the parent process
        }
    }

    close(sd);
}

char *uid_to_str(uid_t uid)
{
    // Max port: 65535 + '\0'
    char *uid_str = malloc(sizeof(*uid_str) * 7);
    if (!uid_str) {
        return NULL;
    }
    snprintf(uid_str, 7, "%d", uid);
    return uid_str;
}

int validate_root_dir(const char *directory)
{
    // Return code; success by default
    int rc = 0;
	char *dir_cpy = strdup(directory);
	if (!dir_cpy) {
		return EX_OSERR;
	}

	DIR *dir = opendir(dir_cpy);
	if (dir) {
		closedir(dir);
		
		size_t dir_len = strlen(directory);
		// strlen(directory) + '\0' + "/cgi-bin" at longest
		size_t new_dir_len = dir_len + 9;
		char *tmp = realloc(dir_cpy, new_dir_len);
		if (!tmp) {
			rc = EX_OSERR;
            goto cleanup;
		}
		dir_cpy = tmp;

		if (directory[dir_len - 1] == '/') {
			// Prune trailing slash if present
			dir_cpy[dir_len - 1] = '\0';
		}

		// Sub-directories to check for
		const char *dirs[2] = { "/cgi-bin", "/www" };
		// Check for each sub-directory
		for (size_t i = 0; i < 2; ++i) {
			char *tmp = malloc(sizeof(*tmp) * new_dir_len);
			if (!tmp) {
                rc = EX_OSERR;
                goto cleanup;
			}
			snprintf(tmp, new_dir_len, "%s%s", dir_cpy, dirs[i]);
			DIR *sub_dir = opendir(tmp);
			if (!sub_dir) {
				fprintf(stderr, "Unable to find %s", tmp);
				// Format perror
				perror(" \b");
				free(tmp);
                rc = -1;
				goto cleanup;
			}
			closedir(sub_dir);
			free(tmp);
		}

	} else {
		fprintf(stderr, "Unable to find %s", dir_cpy);
		// Format perror
		perror(" \b");
        rc = -1;
	}

cleanup:
	free(dir_cpy);
	return rc;
}

char *extract_filename(const char *request, int *err)
{
	char *req_cpy = strdup(request);
	if (!req_cpy) {
        *err = EX_OSERR;
		return NULL;
	}

	if (!validate_request_method(req_cpy)) {
		// Request did not begin with acceptable method
		// TODO: return 400 error to client
        *err = -1;
		free(req_cpy);
		return NULL;
	}

	char *saveptr;
	// First call consumes request method
	char *sub_str = strtok_r(req_cpy, " \n", &saveptr);
	// Second call has sub_str pointing at requested resource
	sub_str = strtok_r(NULL, " \n", &saveptr);
	char *filename = strdup(sub_str);
	if (!filename) {
        *err = EX_OSERR;
		free(req_cpy);
		return NULL;
	}
	free(req_cpy);
	return filename;
}

bool validate_request_method(const char *request)
{
	char *get_chk = strstr(request, "GET");
	char *head_chk = strstr(request, "HEAD");
	// Validate that substr "GET" or "HEAD" is in request
	if (!get_chk && !head_chk) {
		return false;
	}
	// Validate that if "GET" in request, that it begins the request
	// and that it is followed by a space
	if (get_chk && (*(get_chk + 3) != ' ' || get_chk != request)) {
		return false;
	}
	// Same as for "GET", but for "HEAD"
	if (head_chk && (*(head_chk + 4) != ' ' || head_chk != request)) {
		return false;
	}
	return true;
}
