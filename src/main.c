#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "storage.h"

#define DEFAULT_PORT 9606

extern int handle_binary_request(gb_storage *db, int fd, int *done);
extern void run_cli(gb_storage *db, bool show_prompt);

static void print_usage(void)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  googdb <database.gdb>              Interactive CLI\n");
    fprintf(stderr, "  googdb --serve <database.gdb>      TCP server (port %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  googdb --serve <database.gdb> <p>  TCP server (custom port)\n");
}

static int run_server(const char *db_path, int port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    gb_storage *db = gb_open(db_path);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", db_path);
        close(server_fd);
        return 1;
    }

    printf("googdb server listening on port %d (db: %s)\n", port, db_path);
    fflush(stdout);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        int done = 0;
        while (handle_binary_request(db, client_fd, &done) >= 0 && !done)
            ;
        close(client_fd);
    }

    gb_close(db);
    close(server_fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "--serve") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        char *db_path = argv[2];
        int port = (argc >= 4) ? atoi(argv[3]) : DEFAULT_PORT;
        if (port <= 0) { fprintf(stderr, "Invalid port\n"); return 1; }
        return run_server(db_path, port);
    }

    gb_storage *db = gb_open(argv[1]);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", argv[1]);
        return 1;
    }

    run_cli(db, true);
    gb_close(db);
    return 0;
}
