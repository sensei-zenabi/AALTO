/*
 * commandparser.c
 *
 * Updated to include wildcard expansion for parameters.
 * This ensures that when a command is entered in the shell, any wildcard patterns in parameters
 * are expanded using POSIX glob() and the resulting list is passed to the command.
 *
 * Design principles:
 * - Separation of concerns: Wildcard expansion is handled at the shell level, not by individual commands.
 * - Modularity: The parse_input function is responsible for tokenization and expansion.
 * - Scalability: Centralized wildcard handling makes the shell behavior consistent and future-proof.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700  /* Ensures declaration of realpath() on some systems */

#include "commandparser.h"
#include <limits.h>   /* For PATH_MAX */
#include <stdlib.h>   /* For malloc, free, realpath, exit */
#include <unistd.h>   /* For access, fork, chdir */
#include <string.h>   /* For strlen, strncpy, strtok, memcpy */
#include <stdio.h>    /* For fprintf, perror */
#include <sys/wait.h> /* For waitpid */
#include <errno.h>    /* For errno */
#include <glob.h>     /* For glob() */

/* Provide a fallback definition for PATH_MAX if it's not defined */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Custom implementation of strdup since it's not part of standard C11 */
static char *my_strdup(const char *s) {
    size_t len = strlen(s);
    char *dup = malloc(len + 1);
    if (dup)
        memcpy(dup, s, len + 1);
    return dup;
}

#ifndef strdup
#define strdup(s) my_strdup(s)
#endif

/*
 * Global base_path for command lookup.
 * This variable is set to the directory where the executable is located.
 * Using this base path decouples command lookup from the current working directory,
 * ensuring that commands can always be found.
 */
static char base_path[PATH_MAX] = "";

/*
 * Sets the base directory for command lookup.
 * The base directory is typically the directory where the executable is located.
 * This allows commands to be found regardless of the current working directory.
 */
void set_base_path(const char *path) {
    if (path) {
        strncpy(base_path, path, PATH_MAX - 1);
        base_path[PATH_MAX - 1] = '\0';
    }
}

/*
 * Helper function to determine if a string contains wildcard characters.
 * Returns 1 if any of '*', '?' or '[' is found, otherwise 0.
 */
static int contains_wildcard(const char *str) {
    return (strchr(str, '*') || strchr(str, '?') || strchr(str, '[')) ? 1 : 0;
}

/*
 * Parses the input string into a CommandStruct.
 * This function tokenizes the input based on spaces, and for each token that is not an option,
 * it performs wildcard expansion using glob() if the token contains wildcard characters.
 * The resulting tokens are stored as parameters or options in the CommandStruct.
 *
 * Design principles:
 * - Centralized wildcard expansion at the shell level for consistency.
 * - Uses only standard C libraries and POSIX functions.
 */
void parse_input(const char *input, CommandStruct *cmd) {
    char buffer[INPUT_SIZE];
    /* Copy input into a local buffer safely */
    strncpy(buffer, input, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    char *token = strtok(buffer, " ");
    if (token == NULL)
        return;
    /* Store the command; do not perform wildcard expansion on command name */
    strncpy(cmd->command, token, sizeof(cmd->command) - 1);
    cmd->command[sizeof(cmd->command) - 1] = '\0';
    cmd->param_count = 0;
    cmd->opt_count = 0;
    /* Process remaining tokens */
    while ((token = strtok(NULL, " ")) != NULL) {
        if (token[0] == '-') {
            if (cmd->opt_count < MAX_OPTIONS) {
                char *dup = strdup(token);
                if (dup == NULL) {
                    perror("strdup failed");
                    exit(EXIT_FAILURE);
                }
                cmd->options[cmd->opt_count++] = dup;
            }
        } else {
            /* Check if token contains wildcards */
            if (contains_wildcard(token)) {
                glob_t glob_result;
                /* Use glob() to expand the token; GLOB_NOCHECK returns the pattern itself if no match is found */
                int ret = glob(token, GLOB_NOCHECK, NULL, &glob_result);
                if (ret == 0 || ret == GLOB_NOMATCH) {
                    for (size_t i = 0; i < glob_result.gl_pathc; i++) {
                        if (cmd->param_count < MAX_PARAMETERS) {
                            char *dup = strdup(glob_result.gl_pathv[i]);
                            if (dup == NULL) {
                                perror("strdup failed");
                                exit(EXIT_FAILURE);
                            }
                            cmd->parameters[cmd->param_count++] = dup;
                        }
                    }
                } else {
                    /* On glob error, add the original token */
                    if (cmd->param_count < MAX_PARAMETERS) {
                        char *dup = strdup(token);
                        if (dup == NULL) {
                            perror("strdup failed");
                            exit(EXIT_FAILURE);
                        }
                        cmd->parameters[cmd->param_count++] = dup;
                    }
                }
                globfree(&glob_result);
            } else {
                if (cmd->param_count < MAX_PARAMETERS) {
                    char *dup = strdup(token);
                    if (dup == NULL) {
                        perror("strdup failed");
                        exit(EXIT_FAILURE);
                    }
                    cmd->parameters[cmd->param_count++] = dup;
                }
            }
        }
    }
}

/*
 * Executes a command as specified in the CommandStruct.
 * Searches for the executable in predefined directories relative to the base_path.
 * If found, converts the relative path to an absolute path and executes the command using fork() and execv().
 */
void execute_command(const CommandStruct *cmd) {
    char command_path[PATH_MAX];
    int found = 0;

    /* Define an array of relative directories to search for executables.
       These directories are considered relative to the base_path.
       If base_path is not set, we fallback to using the current directory.
    */
    static const char *relative_commands_dirs[] = {
        "commands",   // Primary commands directory.
        "apps",       // Additional folder for executables.
        "utilities"   // Another folder, for example.
    };
    int num_dirs = sizeof(relative_commands_dirs) / sizeof(relative_commands_dirs[0]);

    /* Loop through each directory to find the executable */
    for (int i = 0; i < num_dirs; i++) {
        if (base_path[0] != '\0') {
            snprintf(command_path, sizeof(command_path), "%s/%s/%s", base_path, relative_commands_dirs[i], cmd->command);
        } else {
            snprintf(command_path, sizeof(command_path), "./%s/%s", relative_commands_dirs[i], cmd->command);
        }
        if (access(command_path, X_OK) == 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Command not found or not executable: %s\n", cmd->command);
        return;
    }

    /* Convert the relative path to an absolute path */
    char abs_path[PATH_MAX];
    if (realpath(command_path, abs_path) == NULL) {
        perror("realpath failed");
        return;
    }

    /* Prepare the arguments array for execv.
       Use the absolute path as argv[0]. */
    int total_args = 1 + cmd->param_count + cmd->opt_count;
    char *args[total_args + 1]; // +1 for the NULL terminator
    args[0] = abs_path;
    int index = 1;
    for (int i = 0; i < cmd->param_count; i++) {
        args[index++] = cmd->parameters[i];
    }
    for (int i = 0; i < cmd->opt_count; i++) {
        args[index++] = cmd->options[i];
    }
    args[index] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return;
    } else if (pid == 0) {
        /* Child process: execute the command */
        execv(abs_path, args);
        perror("execv failed");
        exit(EXIT_FAILURE);
    } else {
        /* Parent process waits for the child */
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid failed");
        }
    }
}

/*
 * Frees the dynamically allocated memory in the CommandStruct.
 */
void free_command_struct(CommandStruct *cmd) {
    for (int i = 0; i < cmd->param_count; i++) {
        free(cmd->parameters[i]);
    }
    for (int i = 0; i < cmd->opt_count; i++) {
        free(cmd->options[i]);
    }
}
