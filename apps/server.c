/*
 server.c

 A "switchboard" server that:
 - Listens on TCP port 12345 by default.
 - Accepts multiple clients (up to MAX_CLIENTS).
 - Each client is assumed to have 5 outputs (out0..out4) and 5 inputs (in0..in4).
 - Maintains a routing table so that outX of clientA can be connected to inY of clientB.
 - Provides a simple text-based console UI to:
    * list clients
    * list routes
    * route <A> <out-channel> <B> <in-channel>
    * print <clientID> <-- show last data for all channels of the given client
    * help
    * monitor [FPS]  <-- display in real time the five output values of all connected clients 
                      (update rate set by FPS argument, default if not given) 
                      and allow recording of data (toggle recording with 'R'; exit with 'Q')
    * exit        <-- shut down the current tmux session (i.e. all windows)
 - At startup, the server now checks for a file named "route.rt". If it exists,
   the file is read line-by-line to execute routing commands. If the file is missing
   or a failure occurs during processing, an appropriate message is displayed.
   Otherwise, the file's content is printed.

 Design principles for this modification:
 - We keep a per-client record of the last message seen on each output and each input channel.
 - We add a new monitor mode that polls the client sockets every time the view is updated.
 - The monitor mode uses an FPS value that can be specified as a command argument.
 - While in monitor mode, the operator can press 'R' to toggle recording of the outputs into a CSV file.
 - When recording starts, a snapshot of the active clients is taken and their outputs are written
   to the CSV with each client occupying CHANNELS_PER_APP columns. The very first column is a timestamp
   (relative to the start of monitor mode) with microsecond accuracy.
 - The CSV file is saved in the "logs" directory with a filename of the form "monitor_<timestamp>.csv".
 - A new "exit" command shuts down the whole tmux session (using tmux kill-session).
 - All modifications adhere to plain C with -std=c11 and only use standard cross-platform libraries.
 - We do not remove any existing functionality.
 
 Build:
   gcc -std=c11 -o server server.c

 Run:
   ./server (optionally specify a port)
*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>    // Support for bool type.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <termios.h>    // For non-canonical mode in monitor command
#include <time.h>       // For timestamping
#include <sys/stat.h>   // For mkdir
#include <errno.h>
#include <sys/time.h>   // For gettimeofday

#define SERVER_PORT 12345
#define MAX_CLIENTS 20
#define CHANNELS_PER_APP 5    // 5 outputs, 5 inputs
#define MAX_MSG_LENGTH 512    // maximum message length for storing channel data

// Default FPS if none is specified in monitor command.
#define DEFAULT_MONITOR_FPS 2

typedef struct {
    int sockfd;
    int client_id;
    int active;
    char name[64]; // optional label
} ClientInfo;

// Routing table: route[outClientID][outChannel] -> (inClientID, inChannel)
typedef struct {
    int in_client_id; // -1 if none
    int in_channel;   // 0..4
} Route;

// Structure to keep track of the last message for each channel for a client.
typedef struct {
    char last_out[CHANNELS_PER_APP][MAX_MSG_LENGTH]; // latest message from out channels
    char last_in[CHANNELS_PER_APP][MAX_MSG_LENGTH];  // latest message forwarded to this client's input
} ClientData;

static ClientData client_data[MAX_CLIENTS]; // one per client slot
static ClientInfo clients[MAX_CLIENTS];      // all possible clients
static int next_client_id = 1;                 // ID to assign to the next connecting client

// Routing table: indexed by client_id so ensure the array is big enough.
static Route routing[MAX_CLIENTS + 1][CHANNELS_PER_APP];

// Forward declarations
static void handle_new_connection(int server_fd);
static void handle_client_input(int idx);
static void handle_console_input(void);
static void show_help(void);
static void route_command(int outCID, int outCH, int inCID, int inCH);
static void list_clients(void);
static void list_routes(void);
static int find_client_index(int cid);
static void trim_newline(char *s);
static void shutdown_tmux(void);  // New function to shutdown tmux

// New helper functions for processing routing file.
static void process_routing_file(void);
static void route_command_from_file(int outCID, int outCH, int inCID, int inCH);

// New helper function for monitor mode. Accepts an FPS parameter.
static void monitor_mode(int fps);

int main(int argc, char *argv[]) {
    unsigned short port = SERVER_PORT;
    if (argc > 1) {
        unsigned short temp = (unsigned short)atoi(argv[1]);
        if (temp > 0) {
            port = temp;
        }
    }

    // Initialize arrays
    memset(clients, 0, sizeof(clients));
    memset(routing, -1, sizeof(routing));
    memset(client_data, 0, sizeof(client_data)); // initialize client data buffers

    // Create listening socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    printf("Switchboard Server listening on port %hu.\n", port);
    printf("Type 'help' for commands.\n");

    // Process routing file "route.rt" at startup.
    process_routing_file();

    // Main loop using select()
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int maxfd = server_fd;
        // Add all active clients
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                FD_SET(clients[i].sockfd, &readfds);
                if (clients[i].sockfd > maxfd) {
                    maxfd = clients[i].sockfd;
                }
            }
        }
        // Add stdin for console commands
        FD_SET(STDIN_FILENO, &readfds);
        if (STDIN_FILENO > maxfd) {
            maxfd = STDIN_FILENO;
        }
        int ret = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select");
            break;
        }
        // New connection?
        if (FD_ISSET(server_fd, &readfds)) {
            handle_new_connection(server_fd);
        }
        // Check each client
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && FD_ISSET(clients[i].sockfd, &readfds)) {
                handle_client_input(i);
            }
        }
        // Check console input
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            handle_console_input();
        }
    }
    close(server_fd);
    return 0;
}

// Accept a new client connection.
static void handle_new_connection(int server_fd) {
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int client_sock = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
    if (client_sock < 0) {
        perror("accept");
        return;
    }
    // Find free slot
    int idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        const char *msg = "Server full.\n";
        write(client_sock, msg, strlen(msg));
        close(client_sock);
        return;
    }
    int cid = next_client_id++;
    clients[idx].sockfd = client_sock;
    clients[idx].client_id = cid;
    clients[idx].active = 1;
    snprintf(clients[idx].name, sizeof(clients[idx].name), "Client%d", cid);
    // Initialize client_data for this slot (already zeroed by memset)
    char greet[128];
    snprintf(greet, sizeof(greet),
             "Welcome to Switchboard. You are client_id=%d, with 5 in / 5 out.\n",
             cid);
    write(client_sock, greet, strlen(greet));
    printf("Client %d connected (slot=%d).\n", cid, idx);
}

// Handle incoming data from a client.
static void handle_client_input(int i) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    ssize_t n = recv(clients[i].sockfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        // Client disconnected or error
        printf("Client %d disconnected.\n", clients[i].client_id);
        close(clients[i].sockfd);
        clients[i].active = 0;
        return;
    }
    // Process each line in the received buffer.
    char *start = buf;
    while (1) {
        char *nl = strchr(start, '\n');
        if (!nl) break; // no more lines
        *nl = '\0';
        // Trim potential CR
        char *cr = strchr(start, '\r');
        if (cr) *cr = '\0';
        trim_newline(start);
        // Expect lines like "outX: message"
        int out_ch = -1;
        if (strncmp(start, "out", 3) == 0 && isdigit((unsigned char)start[3])) {
            out_ch = start[3] - '0'; // channel number
        }
        if (out_ch >= 0 && out_ch < CHANNELS_PER_APP) {
            // Find colon and skip to message text
            char *colon = strchr(start, ':');
            const char *msg = "";
            if (colon) {
                msg = colon + 1;
                while (*msg == ' ' || *msg == '\t') { msg++; }
            }
            // Store the outgoing message for the client.
            strncpy(client_data[i].last_out[out_ch], msg, MAX_MSG_LENGTH - 1);
            client_data[i].last_out[out_ch][MAX_MSG_LENGTH - 1] = '\0';
            int out_cid = clients[i].client_id; // which client is sending
            Route r = routing[out_cid][out_ch];
            if (r.in_client_id >= 0) {
                int idx_in = find_client_index(r.in_client_id);
                if (idx_in >= 0 && clients[idx_in].active) {
                    char outbuf[600];
                    snprintf(outbuf, sizeof(outbuf),
                             "in%d from client%d: %s\n",
                             r.in_channel,
                             out_cid,
                             msg);
                    send(clients[idx_in].sockfd, outbuf, strlen(outbuf), 0);
                    // Also store this message as the latest input on channel r.in_channel for the receiving client.
                    strncpy(client_data[idx_in].last_in[r.in_channel], outbuf, MAX_MSG_LENGTH - 1);
                    client_data[idx_in].last_in[r.in_channel][MAX_MSG_LENGTH - 1] = '\0';
                }
            }
        }
        start = nl + 1;
    }
}

// Handle console input from the server operator.
static void handle_console_input(void) {
    char cmdline[256];
    if (!fgets(cmdline, sizeof(cmdline), stdin)) {
        return;
    }
    trim_newline(cmdline);
    if (strcmp(cmdline, "") == 0) {
        return;
    }
    if (strcmp(cmdline, "help") == 0) {
        show_help();
        return;
    }
    if (strcmp(cmdline, "list") == 0) {
        list_clients();
        return;
    }
    if (strcmp(cmdline, "routes") == 0) {
        list_routes();
        return;
    }
    if (strcmp(cmdline, "exit") == 0) {
        shutdown_tmux();
        return;
    }
    // Monitor command: support optional FPS argument.
    if (strncmp(cmdline, "monitor", 7) == 0) {
        strtok(cmdline, " ");
        int fps = DEFAULT_MONITOR_FPS;
        char *arg = strtok(NULL, " ");
        if (arg) {
            int temp = atoi(arg);
            if (temp > 0)
                fps = temp;
        }
        monitor_mode(fps);
        return;
    }
    // Modified command: print <clientID>
    if (strncmp(cmdline, "print", 5) == 0) {
        strtok(cmdline, " ");
        char *pClientID = strtok(NULL, " ");
        if (!pClientID) {
            printf("Usage: print <clientID>\n");
            return;
        }
        int clientID = atoi(pClientID);
        int idx = find_client_index(clientID);
        if (idx < 0) {
            printf("No active client with clientID %d\n", clientID);
            return;
        }
        printf("Data for client%d (%s):\n", clientID, clients[idx].name);
        printf("%-8s | %-50s | %-50s\n", "Channel", "Output", "Input");
        printf("--------------------------------------------------------------------------------\n");
        for (int ch = 0; ch < CHANNELS_PER_APP; ch++) {
            printf("%-8d | %-50.50s | %-50.50s\n", ch, client_data[idx].last_out[ch], client_data[idx].last_in[ch]);
        }
        return;
    }
    // Modified route command.
    if (strncmp(cmdline, "route", 5) == 0) {
        strtok(cmdline, " ");
        char *pOutCID = strtok(NULL, " ");
        char *pOutStr = strtok(NULL, " ");
        char *pInCID = strtok(NULL, " ");
        char *pInStr = strtok(NULL, " ");
        if (!pOutCID || !pOutStr || !pInCID || !pInStr) {
            printf("Usage: route <outCID> <outCH|all> <inCID> <inCH|all>\n");
            return;
        }
        int outCID = atoi(pOutCID);
        int inCID = atoi(pInCID);
        bool outAll = (strcmp(pOutStr, "all") == 0);
        int fixedOut = -1;
        if (!outAll) {
            if (isdigit((unsigned char)pOutStr[0])) {
                fixedOut = atoi(pOutStr);
            } else if (strncmp(pOutStr, "out", 3) == 0 && isdigit((unsigned char)pOutStr[3])) {
                fixedOut = pOutStr[3] - '0';
            }
        }
        bool inAll = (strcmp(pInStr, "all") == 0);
        int fixedIn = -1;
        if (!inAll) {
            if (isdigit((unsigned char)pInStr[0])) {
                fixedIn = atoi(pInStr);
            } else if (strncmp(pInStr, "in", 2) == 0 && isdigit((unsigned char)pInStr[2])) {
                fixedIn = pInStr[2] - '0';
            }
        }
        if (!outAll && (fixedOut < 0 || fixedOut >= CHANNELS_PER_APP)) {
            printf("Invalid output channel. Must be 0..%d or 'all'\n", CHANNELS_PER_APP - 1);
            return;
        }
        if (!inAll && (fixedIn < 0 || fixedIn >= CHANNELS_PER_APP)) {
            printf("Invalid input channel. Must be 0..%d or 'all'\n", CHANNELS_PER_APP - 1);
            return;
        }
        if (outAll && inAll) {
            for (int i = 0; i < CHANNELS_PER_APP; i++) {
                route_command(outCID, i, inCID, i);
            }
        } else if (outAll && !inAll) {
            for (int i = 0; i < CHANNELS_PER_APP; i++) {
                route_command(outCID, i, inCID, fixedIn);
            }
        } else if (!outAll && inAll) {
            for (int i = 0; i < CHANNELS_PER_APP; i++) {
                route_command(outCID, fixedOut, inCID, i);
            }
        } else {
            route_command(outCID, fixedOut, inCID, fixedIn);
        }
        return;
    }
    printf("Unknown command: %s\n", cmdline);
}

static void show_help(void) {
    printf("Commands:\n");
    printf(" help                        - show this help\n");
    printf(" list                        - list connected clients\n");
    printf(" routes                      - list routing table\n");
    printf(" route X Y Z W               - connect clientX outY -> clientZ inW\n");
    printf("   (Y and/or W can be 'all' to route multiple channels)\n");
    printf(" print <clientID>            - show last data for all channels of the given client\n");
    printf(" monitor [FPS]               - display real time output of all clients\n");
    printf("                              Optional FPS sets update rate (default %d FPS).\n", DEFAULT_MONITOR_FPS);
    printf("                              In monitor mode, press 'R' to toggle recording to CSV, 'Q' to quit.\n");
    printf(" exit                        - shutdown the current tmux session (all windows)\n");
    printf("\n");
}

static void route_command(int outCID, int outCH, int inCID, int inCH) {
    int idxO = find_client_index(outCID);
    if (idxO < 0) {
        printf("No such client %d\n", outCID);
        return;
    }
    int idxI = find_client_index(inCID);
    if (idxI < 0) {
        printf("No such client %d\n", inCID);
        return;
    }
    routing[outCID][outCH].in_client_id = inCID;
    routing[outCID][outCH].in_channel = inCH;
    printf("Routed client%d out%d -> client%d in%d\n", outCID, outCH, inCID, inCH);
}

static void list_clients(void) {
    printf("Active clients:\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            printf(" clientID=%d sockfd=%d name=%s\n", clients[i].client_id, clients[i].sockfd, clients[i].name);
        }
    }
}

static void list_routes(void) {
    printf("Routes:\n");
    for (int cid = 1; cid < next_client_id; cid++) {
        int idx = find_client_index(cid);
        if (idx < 0) continue;
        for (int ch = 0; ch < CHANNELS_PER_APP; ch++) {
            int inCID = routing[cid][ch].in_client_id;
            if (inCID >= 0) {
                int inCH = routing[cid][ch].in_channel;
                printf(" client%d.out%d -> client%d.in%d\n", cid, ch, inCID, inCH);
            }
        }
    }
}

static int find_client_index(int cid) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].client_id == cid) {
            return i;
        }
    }
    return -1;
}

static void trim_newline(char *s) {
    char *p = strchr(s, '\n');
    if (p) *p = '\0';
    p = strchr(s, '\r');
    if (p) *p = '\0';
}

/* 
 * process_routing_file()
 *
 * This function attempts to open "route.rt" for reading.
 * For each non-empty line that starts with "route", it parses the tokens in the expected format.
 * A new helper function, route_command_from_file(), is used to update the routing table without checking
 * for active clients (since preconfiguration occurs before clients connect).
 *
 * If the file is not found, or if any command fails to parse, a message is displayed.
 * If all commands are processed successfully, the contents of the file are re-read and displayed.
 */
static void process_routing_file(void) {
    FILE *fp = fopen("route.rt", "r");
    if (!fp) {
        printf("Routing file 'route.rt' not found.\n");
        return;
    }
    char line[256];
    bool all_success = true;
    int cmd_count = 0;
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (strlen(line) == 0 || strncmp(line, "route", 5) != 0) {
            continue;
        }
        cmd_count++;
        char *token = strtok(line, " ");
        if (!token || strcmp(token, "route") != 0) {
            printf("Invalid command in routing file.\n");
            all_success = false;
            continue;
        }
        char *pOutCID = strtok(NULL, " ");
        char *pOutStr = strtok(NULL, " ");
        char *pInCID = strtok(NULL, " ");
        char *pInStr = strtok(NULL, " ");
        if (!pOutCID || !pOutStr || !pInCID || !pInStr) {
            printf("Incomplete routing command in file.\n");
            all_success = false;
            continue;
        }
        int outCID = atoi(pOutCID);
        int inCID = atoi(pInCID);
        bool outAll = (strcmp(pOutStr, "all") == 0);
        int fixedOut = -1;
        if (!outAll) {
            if (isdigit((unsigned char)pOutStr[0])) {
                fixedOut = atoi(pOutStr);
            } else if (strncmp(pOutStr, "out", 3) == 0 && isdigit((unsigned char)pOutStr[3])) {
                fixedOut = pOutStr[3] - '0';
            } else {
                printf("Invalid output channel in routing file.\n");
                all_success = false;
                continue;
            }
        }
        bool inAll = (strcmp(pInStr, "all") == 0);
        int fixedIn = -1;
        if (!inAll) {
            if (isdigit((unsigned char)pInStr[0])) {
                fixedIn = atoi(pInStr);
            } else if (strncmp(pInStr, "in", 2) == 0 && isdigit((unsigned char)pInStr[2])) {
                fixedIn = pInStr[2] - '0';
            } else {
                printf("Invalid input channel in routing file.\n");
                all_success = false;
                continue;
            }
        }
        if (!outAll && (fixedOut < 0 || fixedOut >= CHANNELS_PER_APP)) {
            printf("Invalid output channel value in routing file. Must be 0..%d or 'all'\n", CHANNELS_PER_APP - 1);
            all_success = false;
            continue;
        }
        if (!inAll && (fixedIn < 0 || fixedIn >= CHANNELS_PER_APP)) {
            printf("Invalid input channel value in routing file. Must be 0..%d or 'all'\n", CHANNELS_PER_APP - 1);
            all_success = false;
            continue;
        }
        if (outAll && inAll) {
            for (int i = 0; i < CHANNELS_PER_APP; i++) {
                route_command_from_file(outCID, i, inCID, i);
            }
        } else if (outAll && !inAll) {
            for (int i = 0; i < CHANNELS_PER_APP; i++) {
                route_command_from_file(outCID, i, inCID, fixedIn);
            }
        } else if (!outAll && inAll) {
            for (int i = 0; i < CHANNELS_PER_APP; i++) {
                route_command_from_file(outCID, fixedOut, inCID, i);
            }
        } else {
            route_command_from_file(outCID, fixedOut, inCID, fixedIn);
        }
    }
    fclose(fp);
    if (!all_success || cmd_count == 0) {
        printf("Error processing routing file or no valid commands found.\n");
    } else {
        fp = fopen("route.rt", "r");
        if (fp) {
            printf("Routing file executed successfully. Contents of 'route.rt':\n");
            while (fgets(line, sizeof(line), fp)) {
                printf("%s", line);
            }
            fclose(fp);
        }
    }
}

/*
 * route_command_from_file()
 *
 * This helper function is similar to route_command but does not check for active clients.
 * It directly updates the routing table and prints a message indicating the preconfigured route.
 * This ensures that routing commands from the file are stored even if no clients are connected yet.
 */
static void route_command_from_file(int outCID, int outCH, int inCID, int inCH) {
    routing[outCID][outCH].in_client_id = inCID;
    routing[outCID][outCH].in_channel = inCH;
    printf("Preconfigured: client%d out%d -> client%d in%d\n", outCID, outCH, inCID, inCH);
}

/*
 * shutdown_tmux()
 *
 * This function retrieves the current tmux session name using
 * "tmux display-message -p '#S'" and then issues a system command to kill the session.
 * This shuts down the whole tmux session including all windows.
 */
static void shutdown_tmux(void) {
    FILE *fp = popen("tmux display-message -p '#S'", "r");
    if (!fp) {
        perror("popen");
        return;
    }
    char session_name[64];
    if (fgets(session_name, sizeof(session_name), fp) == NULL) {
        pclose(fp);
        fprintf(stderr, "Failed to get tmux session name.\n");
        return;
    }
    // Remove any trailing newline.
    trim_newline(session_name);
    pclose(fp);
    
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "tmux kill-session -t %s", session_name);
    printf("Executing: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "tmux kill-session command failed with code %d.\n", ret);
    }
    exit(0);
}

/*
 * monitor_mode()
 *
 * This function implements the new "monitor" command.
 * It switches the terminal into non-canonical mode for immediate key detection,
 * and polls all active client sockets to fetch fresh data.
 * The update rate is determined by the provided fps value.
 *
 * In the monitor view:
 *  - Press 'Q' to quit monitor mode.
 *  - Press 'R' to toggle recording.
 *    When recording is started, a new CSV file "logs/monitor_<timestamp>.csv" is created
 *    and a snapshot of the currently active clients is taken.
 *    Their outputs are written to the CSV with each client occupying CHANNELS_PER_APP columns.
 *    The very first column is a relative timestamp (starting from zero) with microsecond accuracy.
 *    Pressing 'R' stops recording; starting recording again creates a new file.
 *  - All statuses and instructions are displayed.
 */
static void monitor_mode(int fps) {
    struct termios orig_termios, new_termios;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        return;
    }
    new_termios = orig_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) == -1) {
        perror("tcsetattr");
        return;
    }

    // Capture the start time for relative timestamping.
    struct timeval start_time;
    gettimeofday(&start_time, NULL);

    // Variables for recording.
    bool recording = false;
    FILE *log_file = NULL;
    char log_filename[256] = "";
    int recorded_client_indices[MAX_CLIENTS];
    int recorded_client_count = 0;

    printf("Entering monitor mode at %d FPS.\nPress 'Q' to quit, 'R' to toggle recording.\n", fps);
    fflush(stdout);

    bool quit = false;
    while (!quit) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int maxfd = STDIN_FILENO;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                FD_SET(clients[i].sockfd, &readfds);
                if (clients[i].sockfd > maxfd)
                    maxfd = clients[i].sockfd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000000 / fps;

        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select in monitor_mode");
            break;
        }

        // Process new data from client sockets.
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && FD_ISSET(clients[i].sockfd, &readfds)) {
                handle_client_input(i);
            }
        }

        // Check for keypresses.
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) > 0) {
                if (ch == 'Q' || ch == 'q') {
                    quit = true;
                } else if (ch == 'R' || ch == 'r') {
                    // Toggle recording.
                    if (!recording) {
                        if (mkdir("logs", 0755) == -1 && errno != EEXIST) {
                            perror("mkdir");
                        }
                        time_t now = time(NULL);
                        struct tm *tm_info = localtime(&now);
                        char timestamp[64];
                        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
                        snprintf(log_filename, sizeof(log_filename), "logs/monitor_%s.csv", timestamp);
                        log_file = fopen(log_filename, "w");
                        if (!log_file) {
                            perror("fopen for log file");
                        } else {
                            recorded_client_count = 0;
                            for (int i = 0; i < MAX_CLIENTS; i++) {
                                if (clients[i].active) {
                                    recorded_client_indices[recorded_client_count++] = i;
                                }
                            }
                            // Write header: first column "timestamp", then each client's channels.
                            fprintf(log_file, "timestamp,");
                            for (int j = 0; j < recorded_client_count; j++) {
                                int idx = recorded_client_indices[j];
                                for (int ch = 0; ch < CHANNELS_PER_APP; ch++) {
                                    fprintf(log_file, "client%d_ch%d", clients[idx].client_id, ch);
                                    if (j != recorded_client_count - 1 || ch != CHANNELS_PER_APP - 1)
                                        fprintf(log_file, ",");
                                }
                            }
                            fprintf(log_file, "\n");
                            fflush(log_file);
                            recording = true;
                        }
                    } else {
                        if (log_file) {
                            fclose(log_file);
                            log_file = NULL;
                        }
                        recording = false;
                    }
                }
            }
        }

        // If recording, write current outputs to log file.
        if (recording && log_file) {
            struct timeval now, delta;
            gettimeofday(&now, NULL);
            delta.tv_sec = now.tv_sec - start_time.tv_sec;
            delta.tv_usec = now.tv_usec - start_time.tv_usec;
            if (delta.tv_usec < 0) {
                delta.tv_sec -= 1;
                delta.tv_usec += 1000000;
            }
            // Write relative timestamp as the first column.
            fprintf(log_file, "\"%ld.%06ld\",", delta.tv_sec, delta.tv_usec);
            for (int j = 0; j < recorded_client_count; j++) {
                int idx = recorded_client_indices[j];
                for (int ch = 0; ch < CHANNELS_PER_APP; ch++) {
                    char data[MAX_MSG_LENGTH];
                    strncpy(data, client_data[idx].last_out[ch], MAX_MSG_LENGTH - 1);
                    data[MAX_MSG_LENGTH - 1] = '\0';
                    for (char *p = data; *p; p++) {
                        if (*p == '\n' || *p == '\r')
                            *p = ' ';
                    }
                    fprintf(log_file, "\"%s\"", data);
                    if (j != recorded_client_count - 1 || ch != CHANNELS_PER_APP - 1)
                        fprintf(log_file, ",");
                }
            }
            fprintf(log_file, "\n");
            fflush(log_file);
        }

        // Clear the screen and display the monitoring view.
        system("clear");
        printf("=== Monitoring Mode (FPS: %d) ===\n", fps);
        printf("Press 'Q' to quit, 'R' to toggle recording.\n");
        if (recording)
            printf("Recording: ON (file: %s)\n", log_filename);
        else
            printf("Recording: OFF\n");
        printf("-------------------------------------------------------------\n");
        printf("%-10s | %-50s\n", "Client", "Output Channels (0..4)");
        printf("-------------------------------------------------------------\n");
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                printf("client%-4d | ", clients[i].client_id);
                for (int ch = 0; ch < CHANNELS_PER_APP; ch++) {
                    printf("[%d]: %-10.10s ", ch, client_data[i].last_out[ch]);
                }
                printf("\n");
            }
        }
        fflush(stdout);
    }

    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    if (tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios) == -1) {
        perror("tcsetattr");
    }
    printf("Exiting monitor mode.\n");
}
