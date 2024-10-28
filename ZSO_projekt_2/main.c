#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#define MAX_SLAVES 10
#define BUFFER_SIZE 256

typedef struct {
    int pid;
    int messages_sent;
    int messages_received;
} SlaveInfo;

typedef struct {
    int total_messages_sent;
    int total_messages_received;
    SlaveInfo slaves[MAX_SLAVES];
} SharedMemory;

int num_slaves;
SharedMemory *shared_mem;
int shm_id;
int monitor_pid;

pthread_mutex_t lock;

// Deklaracje funkcji testujących
void test_initialize_shared_memory();
void test_cleanup_shared_memory();
void test_create_channel();
void test_signal_handler();

// Inicjalizacja pamięci dzielonej
void initialize_shared_memory() {
    FILE *shmfile = fopen("shmfile", "w");
    if (shmfile == NULL) {
        perror("fopen");
        exit(1);
    }
    fclose(shmfile);

    key_t key = ftok("shmfile", 65);
    if (key == -1) {
        perror("ftok");
        exit(1);
    }

    shm_id = shmget(key, sizeof(SharedMemory), 0666 | IPC_CREAT);
    if (shm_id == -1) {
        perror("shmget");
        exit(1);
    }

    shared_mem = (SharedMemory *)shmat(shm_id, (void *)0, 0);
    if (shared_mem == (void *)-1) {
        perror("shmat");
        exit(1);
    }

    memset(shared_mem, 0, sizeof(SharedMemory));
}

// Sprzątanie pamięci dzielonej
void cleanup_shared_memory() {
    if (shmdt(shared_mem) == -1) {
        perror("shmdt");
    }
    if (shmctl(shm_id, IPC_RMID, NULL) == -1) {
        perror("shmctl");
    }
}

// Tworzenie kanałów komunikacyjnych (FIFOs)
void create_channel(int from, int to) {
    char pipe_name[BUFFER_SIZE];
    snprintf(pipe_name, BUFFER_SIZE, "/tmp/channel_%d_to_%d", from, to);
    if (mkfifo(pipe_name, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        exit(1);
    }
}

// Obsługa sygnałów
void signal_handler(int signum) {
    if (signum == SIGUSR1) {
        for (int i = 0; i < num_slaves; i++) {
            if (shared_mem->slaves[i].pid != 0) {
                printf("Slave %d sending stats to monitor\n", i);

                char buffer[BUFFER_SIZE];
                snprintf(buffer, BUFFER_SIZE, "Slave %d stats: sent=%d, received=%d\n",
                         shared_mem->slaves[i].pid,
                         shared_mem->slaves[i].messages_sent,
                         shared_mem->slaves[i].messages_received);

                int fd = open("/tmp/monitor_pipe", O_WRONLY);
                if (fd == -1) {
                    perror("open");
                    return;
                }
                write(fd, buffer, strlen(buffer) + 1);
                close(fd);
                printf("Signal handler sent data to monitor: %s\n", buffer);
            }
        }
    }
}

// Wątek monitorujący
void* monitor_thread(void *arg) {
    char pipe_name[BUFFER_SIZE];
    snprintf(pipe_name, BUFFER_SIZE, "/tmp/monitor_pipe");
    if (mkfifo(pipe_name, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        exit(1);
    }

    int fd;
    char buffer[BUFFER_SIZE];

    printf("Monitor thread started\n");

    while (1) {
        fd = open(pipe_name, O_RDONLY);
        if (fd == -1) {
            perror("open");
            continue;
        }
        if (read(fd, buffer, BUFFER_SIZE) == -1) {
            perror("read");
            close(fd);
            continue;
        }
        printf("Monitor received: %s\n", buffer);
        close(fd);
    }

    if (unlink(pipe_name) == -1) {
        perror("unlink");
    }
    return NULL;
}

// Wątek procesu podrzędnego (slave)
void* slave_thread(void *arg) {
    int id = *(int *)arg;
    printf("Slave %d PID: %d\n", id, getpid());  // Logowanie PID
    char pipe_name[BUFFER_SIZE];
    snprintf(pipe_name, BUFFER_SIZE, "/tmp/slave_pipe_%d", id);
    if (mkfifo(pipe_name, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        exit(1);
    }

    int fd;
    char buffer[BUFFER_SIZE];

    snprintf(buffer, BUFFER_SIZE, "REGISTER %d", id);
    fd = open("/tmp/master_pipe", O_WRONLY);
    if (fd == -1) {
        perror("open");
        printf("Slave %d: Error opening master pipe\n", id);
        exit(1);
    }
    printf("Slave %d: Opened master pipe for registration\n", id);  // Logowanie po otwarciu master pipe
    if (write(fd, buffer, strlen(buffer) + 1) == -1) {
        perror("write");
        printf("Slave %d: Error writing to master pipe\n", id);
        close(fd);
        exit(1);
    }
    printf("Slave %d: Written to master pipe for registration\n", id);  // Logowanie po zapisaniu do master pipe
    close(fd);

    printf("Slave %d registered with master\n", id);

    while (1) {
        snprintf(buffer, BUFFER_SIZE, "Slave %d reporting", id);
        fd = open("/tmp/master_pipe", O_WRONLY);
        if (fd == -1) {
            perror("open");
            printf("Slave %d: Error opening master pipe for reporting\n", id);
            continue;
        }
        if (write(fd, buffer, strlen(buffer) + 1) == -1) {
            perror("write");
            printf("Slave %d: Error writing report to master pipe\n", id);
            close(fd);
            continue;
        }
        close(fd);

        fd = open(pipe_name, O_RDONLY);
        if (fd == -1) {
            perror("open");
            printf("Slave %d: Error opening own pipe\n", id);
            continue;
        }
        if (read(fd, buffer, BUFFER_SIZE) == -1) {
            perror("read");
            printf("Slave %d: Error reading from own pipe\n", id);
            close(fd);
            continue;
        }
        printf("Slave %d received: %s\n", id, buffer);
        close(fd);

        pthread_mutex_lock(&lock);
        shared_mem->slaves[id].messages_received++;
        pthread_mutex_unlock(&lock);

        if (strncmp(buffer, "EXIT", 4) == 0) {
            break;
        }
    }

    // Derejestracja z mastera
    snprintf(buffer, BUFFER_SIZE, "DEREGISTER %d", id);
    fd = open("/tmp/master_pipe", O_WRONLY);
    if (fd == -1) {
        perror("open");
        printf("Slave %d: Error opening master pipe for deregistration\n", id);
        exit(1);
    }
    if (write(fd, buffer, strlen(buffer) + 1) == -1) {
        perror("write");
        printf("Slave %d: Error writing deregistration to master pipe\n", id);
        close(fd);
        exit(1);
    }
    close(fd);

    if (unlink(pipe_name) == -1) {
        perror("unlink");
    }
    printf("Slave %d deregistered from master\n", id);
    return NULL;
}

// Wątek procesu nadrzędnego (master)
void* master_thread(void *arg) {
    char pipe_name[BUFFER_SIZE];
    snprintf(pipe_name, BUFFER_SIZE, "/tmp/master_pipe");
    if (mkfifo(pipe_name, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        exit(1);
    }

    int fd;
    char buffer[BUFFER_SIZE];

    while (1) {
        fd = open(pipe_name, O_RDONLY);
        if (fd == -1) {
            perror("open");
            printf("Master: Error opening master pipe\n");
            continue;
        }
        printf("Master: Opened master pipe for reading\n");  // Logowanie po otwarciu master pipe
        if (read(fd, buffer, BUFFER_SIZE) == -1) {
            perror("read");
            printf("Master: Error reading from master pipe\n");
            close(fd);
            continue;
        }
        printf("Master received: %s\n", buffer);  // Dodanie logów po odczytaniu komunikatu
        close(fd);

        // Przetwarzanie wiadomości
        pthread_mutex_lock(&lock);
        shared_mem->total_messages_received++;
        pthread_mutex_unlock(&lock);

        if (strncmp(buffer, "REGISTER", 8) == 0) {
            int slave_id = atoi(buffer + 9);
            pthread_mutex_lock(&lock);
            shared_mem->slaves[slave_id].pid = slave_id;
            printf("Slave %d registered\n", slave_id);  // Dodanie logów po rejestracji
            pthread_mutex_unlock(&lock);
        } else if (strncmp(buffer, "DEREGISTER", 10) == 0) {
            int slave_id = atoi(buffer + 11);
            pthread_mutex_lock(&lock);
            shared_mem->slaves[slave_id].pid = 0;
            printf("Slave %d deregistered\n", slave_id);  // Dodanie logów po wyrejestrowaniu
            pthread_mutex_unlock(&lock);
        } else if (strncmp(buffer, "EXIT", 4) == 0) {
            break;
        }
    }

    if (unlink(pipe_name) == -1) {
        perror("unlink");
    }
    return NULL;
}

// Testy funkcjonalne
void test_initialize_shared_memory() {
    initialize_shared_memory();
    if (shared_mem == NULL) {
        fprintf(stderr, "Test failed: shared_mem is NULL\n");
        exit(1);
    }
    printf("Test passed: initialize_shared_memory\n");
}

void test_cleanup_shared_memory() {
    initialize_shared_memory();
    cleanup_shared_memory();
    // Sprawdzenie, czy segment pamięci dzielonej został usunięty
    key_t key = ftok("shmfile", 65);
    int id = shmget(key, sizeof(SharedMemory), 0666);
    if (id != -1) {
        fprintf(stderr, "Test failed: shared memory segment not removed\n");
        exit(1);
    }
    printf("Test passed: cleanup_shared_memory\n");
}

void test_create_channel() {
    create_channel(0, 1);
    char pipe_name[BUFFER_SIZE];
    snprintf(pipe_name, BUFFER_SIZE, "/tmp/channel_0_to_1");
    if (access(pipe_name, F_OK) == -1) {
        fprintf(stderr, "Test failed: channel not created\n");
        exit(1);
    }
    printf("Test passed: create_channel\n");
}

void test_signal_handler() {
    for (int i = 0; i < num_slaves; ++i) {
        if (kill(getpid(), SIGUSR1) == -1) {
            perror("kill");
            fprintf(stderr, "Test failed: signal not sent to self\n");
            exit(1);
        }
    }
    printf("Test passed: signal_handler\n");
}

// Główna funkcja programu
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num_slaves> <num_elements>\n", argv[0]);
        exit(1);
    }

    num_slaves = atoi(argv[1]);
    int num_elements = atoi(argv[2]);

    if (num_slaves > MAX_SLAVES) {
        fprintf(stderr, "Number of slaves exceeds the maximum limit (%d)\n", MAX_SLAVES);
        exit(1);
    }

    pthread_mutex_init(&lock, NULL);
    initialize_shared_memory();

    for (int i = 0; i < num_slaves; ++i) {
        for (int j = 0; j < num_slaves; ++j) {
            if (i != j) {
                create_channel(i, j);
            }
        }
    }

    monitor_pid = fork();
    if (monitor_pid == 0) {
        monitor_thread(NULL);
        exit(0);
    }

    pthread_t master_tid;
    pthread_create(&master_tid, NULL, master_thread, NULL);

    pthread_t slave_tids[MAX_SLAVES];
    int slave_ids[MAX_SLAVES];
    for (int i = 0; i < num_slaves; ++i) {
        slave_ids[i] = i;
        if (fork() == 0) {
            slave_thread(&slave_ids[i]);
            exit(0);
        }
    }

    pthread_join(master_tid, NULL);

    for (int i = 0; i < num_slaves; ++i) {
        wait(NULL);
    }

    kill(monitor_pid, SIGTERM);
    wait(NULL);

    cleanup_shared_memory();
    pthread_mutex_destroy(&lock);

    // Uruchomienie testów po zakończeniu programu
    test_initialize_shared_memory();
    test_cleanup_shared_memory();
    test_create_channel();
    test_signal_handler();

    return 0;
}
