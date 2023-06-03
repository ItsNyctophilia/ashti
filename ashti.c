#define _POSIX_C_SOURCE 201112L
#define _XOPEN_SOURCE 800
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

char *uid_to_str(uid_t uid);
int validate_root_dir(const char *directory);
char *extract_filename(const char *request, int *err);
bool validate_request_method(const char *request);
bool validate_request_legality(const char *root, const char *target,
			       char **full_name);
char *prepare_headers(const size_t server_code,
		      const char *filename, const off_t filesize);
char *execute_cgi_script(const char *script_path);

int main(int argc, char *argv[])
{
	if (argc == 1 || argc > 2) {
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

	struct addrinfo hints = { 0 };
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

	int sd = socket(results->ai_family, results->ai_socktype,
			results->ai_protocol);
	if (sd < 0) {
		perror("Could not create socket");
		freeaddrinfo(results);
		return EX_OSERR;
	}
	int enable = 1;
	if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
		perror("Could not mark socket for address reuse");
	}

	err = bind(sd, results->ai_addr, results->ai_addrlen);
	if (err < 0) {
		perror("Could not bind socket");
		close(sd);
		freeaddrinfo(results);
		return EX_OSERR;
	}
	freeaddrinfo(results);

	// Backlog of 5 is typical
	err = listen(sd, 5);
	if (err < 0) {
		perror("Could not listen on socket");
		close(sd);
		return EX_OSERR;
	}

	for (;;) {
		struct sockaddr_storage client;
		socklen_t client_sz = sizeof(client);
		char addr[INET6_ADDRSTRLEN];

		int remote = accept(sd, (struct sockaddr *)&client, &client_sz);

		if (client.ss_family == AF_INET6) {
			inet_ntop(client.ss_family,
				  &((struct sockaddr_in6 *)&client)->sin6_addr,
				  addr, sizeof(addr));
		} else {
			inet_ntop(client.ss_family,
				  &((struct sockaddr_in *)&client)->sin_addr,
				  addr, sizeof(addr));
		}

		pid_t child_pid = fork();
		if (child_pid < 0) {
			perror("Could not fork process");
			close(remote);
			continue;
		} else if (child_pid == 0) {
			// Child process
			close(sd);	// Close the listening socket in the child process

			// seems like a reasonable buffer size
			size_t buffer_size = 1024;
			char *buffer = malloc(buffer_size);
			if (buffer == NULL) {
				perror("Failed to allocate memory for buffer");
				close(remote);
				return EX_OSERR;
			}
			// Only reading a single header; one recv call is sufficient
			ssize_t received =
			    recv(remote, buffer, buffer_size - 1, 0);
			// Null-terminate the received message
			buffer[received] = '\0';
			// Devprint
			// printf("Received message:\n%s\n", buffer);

			int err = 0;
			size_t code = 200;	// Default is '200 OK'
			char *header = NULL;
			char *full_name = NULL;
			bool head_request = false;

			if (strstr(buffer, "HEAD")) {
				head_request = true;
			}
			char *filename = extract_filename(buffer, &err);
			if (!filename) {
				code = err;
				full_name = strdup("error/400.html");
			}
			if (code == 200) {
				bool valid =
				    validate_request_legality(argv[1], filename,
							      &full_name);
				if (!valid && !full_name) {
					perror
					    ("Failed to reallocate memory for buffer");
					return EX_OSERR;
				} else if (!valid && errno == ENOENT) {
					code = 404;
				} else if (!valid) {
					code = 403;
				}
			}

			if (strstr(full_name, "/cgi-bin/")) {
				char *cgi_output =
				    execute_cgi_script(full_name);
				if (cgi_output && !head_request) {
					ssize_t bytes_sent =
					    send(remote, cgi_output,
						 strlen(cgi_output), 0);
					free(cgi_output);

					if (bytes_sent == -1) {
						perror
						    ("Failed to send CGI output");
					}
					goto cleanup;
				} else if (!cgi_output) {
					free(full_name);
					full_name = strdup("error/500.html");
					code = 500;
				} else {
					free(cgi_output);
				}

			}
			// Get a file descriptor for sendfile() call later
			int fd = open(full_name, O_RDONLY);
			if (fd < 0) {
				if (errno == EACCES) {
					// Only occurs when file exists and has bad permissions
					code = 403;
					free(full_name);
					full_name = strdup("error/403.html");
					struct stat stat_buffer;
					if (stat(full_name, &stat_buffer) != 0) {
						perror
						    ("Failed to retrieve file information");
						goto cleanup;
					}
					header = prepare_headers(code, filename,
								 stat_buffer.
								 st_size);
					send(remote, header, strlen(header), 0);
					int fd = open(full_name, O_RDONLY);
					if (fd < 0) {
						perror("Failed to open file");
						goto cleanup;
					}
					ssize_t bytes_sent =
					    sendfile(remote, fd, NULL,
						     stat_buffer.st_size);
					if (bytes_sent == -1) {
						perror("Failed to send file");
					}
					goto cleanup;
				}
				perror("Failed to open file");
				goto cleanup;
			}
			// Retrieve filesize for sendfile() call
			struct stat stat_buffer;
			if (stat(full_name, &stat_buffer) != 0) {
				perror("Failed to retrieve file information");
				close(fd);
				goto cleanup;
			}

			header =
			    prepare_headers(code, filename,
					    stat_buffer.st_size);
			send(remote, header, strlen(header), 0);
			// Send file to remote client
			if (head_request && code == 200) {
				close(fd);
				goto cleanup;
			}
			ssize_t bytes_sent =
			    sendfile(remote, fd, NULL, stat_buffer.st_size);
			if (bytes_sent == -1) {
				perror("Failed to send file");
				close(fd);
			}

 cleanup:
			free(full_name);
			free(filename);
			free(buffer);
			free(header);
			close(remote);
			putchar('\n');

			return EXIT_SUCCESS;	// Exit the child process
		} else {
			// Parent process
			close(remote);	// Close the remote socket in the parent process
		}
	}

	close(sd);
}

char *execute_cgi_script(const char *script_path)
{
	int stdout_pipe[2];
	int stderr_pipe[2];
	if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
		perror("Failed to create pipe");
		return NULL;
	}

	pid_t child_pid = fork();
	if (child_pid < 0) {
		perror("Failed to fork process");
		return NULL;
	} else if (child_pid == 0) {
		// Child process
		// Close the read end of the stdout pipe
		close(stdout_pipe[0]);
		// Close the read end of the stderr pipe
		close(stderr_pipe[0]);
		if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
			perror("Failed to redirect stdout");
			return NULL;
		}
		// Close duplicated write end of the stdout pipe
		close(stdout_pipe[1]);

		if (dup2(STDERR_FILENO, STDERR_FILENO) < 0) {
			perror("Failed to redirect stderr");
			return NULL;
		}
		// Execute the CGI script
		if (execl(script_path, script_path, NULL) < 0) {
			perror("Failed to execute CGI script");
			return NULL;
		}
	} else {
		// Parent process
		// Close the write end of the stdout pipe
		close(stdout_pipe[1]);
		// Close the write end of the stderr pipe
		close(stderr_pipe[1]);
		// Arbitrary buffer size
		char buffer[1024];
		size_t total_received = 0;
		ssize_t received;

		char *output = NULL;
		while ((received = read(stdout_pipe[0], buffer, 1024)) > 0) {
			output = realloc(output, total_received + received + 1);
			if (!output) {
				perror("Failed to allocate memory for buffer");
				return NULL;
			}
			memcpy(output + total_received, buffer, received);
			total_received += received;
		}

		if (received < 0) {
			perror("Failed to read from pipe");
			return NULL;
		}

		output[total_received] = '\0';
		// Close the read end of the stdout pipe
		close(stdout_pipe[0]);

		// Wait for the child process to exit
		waitpid(child_pid, NULL, 0);

		return output;
	}
	return NULL;
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
		*err = 400;
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

bool validate_request_legality(const char *root, const char *target,
			       char **full_name)
{
	char *root_path = NULL;
	char *full_path = NULL;
	char *final_path = NULL;
	bool is_legal = false;
	// Default error is 403 forbidden
	const char *err_file = "error/403.html";

	root_path = realpath(root, NULL);
	if (!root_path) {
		goto cleanup;
	}

	size_t root_path_len = strlen(root_path);
	if (root_path[root_path_len - 1] == '/') {
		// Prune trailing slash if present
		root_path[root_path_len - 1] = '\0';
	}
	// Two paths combined + '/www/' + '\0'
	size_t full_path_len = root_path_len + strlen(target) + 6;
	full_path = malloc(sizeof(*full_path) * full_path_len);
	if (!full_path) {
		goto cleanup;
	}

	if (!strstr(target, "cgi-bin/")) {
		// Default directory is "www"
		snprintf(full_path, full_path_len, "%s/www/%s", root_path,
			 target);
	} else {
		// If user specifies "cgi-bin", do not add "www"
		snprintf(full_path, full_path_len, "%s/%s", root_path, target);
	}

	final_path = realpath(full_path, NULL);
	if (!final_path) {
		if (errno == ENOMEM) {
			goto cleanup;
		} else if (errno == ENOENT) {
			// File not found
			err_file = "error/404.html";
		}
		// 16 bytes from "/error/404.html" + '\0'
		*full_name = strdup(err_file);
		goto cleanup;
	}

	const char *legal_folders[2] = { "/cgi-bin", "/www" };
	for (size_t i = 0; i < 2; ++i) {
		// path length + "/cgi-bin" + '\0' at most
		size_t sub_folder_path_len = root_path_len + 9;
		char *tmp = malloc(sizeof(*tmp) * sub_folder_path_len);
		if (!tmp) {
			// TODO: disambiguate OOM case
			goto cleanup;
		}
		snprintf(tmp, sub_folder_path_len,
			 "%s%s", root_path, legal_folders[i]);
		if (strstr(final_path, tmp)) {
			free(tmp);
			is_legal = true;
			break;
		}
		free(tmp);
	}
	if (is_legal) {
		*full_name = strdup(full_path);
	} else {
		*full_name = strdup(err_file);
	}

 cleanup:
	free(root_path);
	free(full_path);
	free(final_path);
	return is_legal;
}

char *prepare_headers(const size_t server_code,
		      const char *filename, const off_t filesize)
{
	const char *header_msg;
	switch (server_code) {
	case 200:
		header_msg = "200 OK";
		break;
	case 400:
		header_msg = "400 Bad Request";
		break;
	case 403:
		header_msg = "403 Forbidden";
		break;
	case 404:
		header_msg = "404 Not Found";
		break;
	case 405:
		header_msg = "405 Method Not Allowed";
		break;
	case 500:
		header_msg = "500 Internal Server Error";
		break;
	default:
		header_msg = "418 I'm a teapot";
	}

	// This will very likely fail if multiple extensions are provided
	// ex. file.txt.html.gif
	const char *filetype;
	if (server_code == 200) {
		// Assign filetype based on extension
		if (strstr(filename, ".txt") || strstr(filename, ".text")) {
			filetype = "text/plain";
		} else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg")
			   || strstr(filename, ".jfif")
			   || strstr(filename, ".pjpeg")
			   || strstr(filename, ".pjp")) {
			filetype = "image/jpeg";
		} else if (strstr(filename, ".png")) {
			filetype = "image/png";
		} else if (strstr(filename, ".gif")) {
			filetype = "image/gif";
		} else {
			// Default to assume html
			filetype = "text/html";
		}
	} else {
		// All non-OK responses will be in HTML format
		filetype = "text/html";
	}

	// Prepare date
	time_t current_time;
	struct tm *date;
	// Date format will always be 29 bytes in length + '\0'
	char date_buf[30];
	time(&current_time);
	date = gmtime(&current_time);

	strftime(date_buf, 30, "%a, %d %b %Y %X GMT", date);

	// Prepare final string
	const char *c_type = "Content-Type:";
	const char *c_len = "Content-Length:";
	const char *format_str =
	    "HTTP/1.1 %s\r\nDate: %s\r\n%s %s\r\n%s %d\r\n\r\n";

	// Response header at absolute longest should not exceed 150 bytes
	char *final_buf = malloc(sizeof(*final_buf) * 150);

	snprintf(final_buf, 150, format_str, header_msg,
		 date_buf, c_type, filetype, c_len, filesize);

	return final_buf;
}
