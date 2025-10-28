#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/wait.h>

int main() {
    pid_t pid; // pid_t is a type for process IDs

    // Create a child process
    pid = fork();

    if (pid < 0) { // Error occurred
        perror("fork failed");
        return EXIT_FAILURE;
    } else if (pid == 0) { // Child process
        printf("Hello from the child process!\n");
        printf("Child's PID: %d, Parent's PID: %d\n", getpid(), getppid());
        exit(0); // Child exits
    } else { // Parent process
        printf("Hello from the parent process!\n");
        printf("Parent's PID: %d, Child's PID: %d\n", getpid(), pid);

        // Parent waits for the child to complete
        int status;
        wait(&status);
        printf("Child process has terminated.\n");
        return EXIT_SUCCESS;
    }
}