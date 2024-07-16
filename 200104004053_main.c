/*
    Erkan hw5
*/

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <stdatomic.h>

#define MAX_PATH_LENGTH 1024

volatile sig_atomic_t terminate = 0;

atomic_int numRegularFiles = 0;
atomic_int numFIFOFiles = 0;
atomic_int numDirectories = 0;
atomic_long totalBytesCopied = 0;
struct timeval start, end;
int buffer_size;
int worker_count;
pthread_t mgr_thread;
pthread_t *workers;
pthread_barrier_t barrier;

typedef struct {
    char src[MAX_PATH_LENGTH];
    char dst[MAX_PATH_LENGTH];
} FilePair;

typedef struct {
    int buffer_size;
    int num_workers;
    char *src_dir;
    char *dst_dir;
} Arguments;

typedef struct {
    FilePair *items;
    int capacity;
    int count;
    int head;
    int tail;
    int stop;
    pthread_mutex_t lock;
    pthread_cond_t can_produce;
    pthread_cond_t can_consume;
} Buffer;

struct manager_args {
    char *src_dir;
    char *dst_dir;
};

Buffer buffer;

void init_buffer(Buffer *buffer, int size) {
    buffer->items = malloc(sizeof(FilePair) * size);
    if (!buffer->items) {
        perror("Failed to allocate buffer");
        exit(EXIT_FAILURE);
    }
    buffer->capacity = size;
    buffer->count = 0;
    buffer->head = 0;
    buffer->tail = 0;
    buffer->stop = 0;
    pthread_mutex_init(&buffer->lock, NULL);
    pthread_cond_init(&buffer->can_produce, NULL);
    pthread_cond_init(&buffer->can_consume, NULL);
}

void destroy_buffer(Buffer *buffer) {
    free(buffer->items);
    pthread_mutex_destroy(&buffer->lock);
    pthread_cond_destroy(&buffer->can_produce);
    pthread_cond_destroy(&buffer->can_consume);
}

void buffer_put(Buffer *buffer, FilePair item) {
    pthread_mutex_lock(&buffer->lock);
    while (buffer->count == buffer->capacity && buffer->stop == 0) {
        pthread_cond_wait(&buffer->can_produce, &buffer->lock);
    }
    if (buffer->stop) {
        pthread_cond_broadcast(&buffer->can_consume);
        pthread_mutex_unlock(&buffer->lock);
        return;
    }
    buffer->items[buffer->tail] = item;
    buffer->tail = (buffer->tail + 1) % buffer->capacity;
    buffer->count++;
    pthread_cond_signal(&buffer->can_consume);
    pthread_mutex_unlock(&buffer->lock);
}

FilePair buffer_get(Buffer *buffer) {
    pthread_mutex_lock(&buffer->lock);
    while (buffer->count == 0 && buffer->stop == 0) {
        pthread_cond_wait(&buffer->can_consume, &buffer->lock);
    }
    if (buffer->stop && buffer->count == 0) {
        pthread_mutex_unlock(&buffer->lock);
        pthread_exit(NULL);
    }
    FilePair item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % buffer->capacity;
    buffer->count--;
    pthread_cond_signal(&buffer->can_produce);
    pthread_mutex_unlock(&buffer->lock);
    return item;
}

Arguments parse_arguments(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <buffer_size> <num_workers> <src_dir> <dst_dir>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    Arguments args;
    args.buffer_size = atoi(argv[1]);
    args.num_workers = atoi(argv[2]);
    args.src_dir = argv[3];
    args.dst_dir = argv[4];
    return args;
}

void *manager_recursive(void *arg) {
    struct manager_args *args = (struct manager_args *)arg;
    const char *src_dir = args->src_dir;
    const char *dst_dir = args->dst_dir;

    DIR *dir = opendir(src_dir);
    if (!dir) {
        perror("Failed to open source directory");
        free(args->src_dir);
        free(args->dst_dir);
        free(arg);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char src_path[MAX_PATH_LENGTH];
        char dst_path[MAX_PATH_LENGTH];
        snprintf(src_path, MAX_PATH_LENGTH, "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, MAX_PATH_LENGTH, "%s/%s", dst_dir, entry->d_name);

        if (entry->d_type == DT_DIR) {
            mkdir(dst_path, 0755);
            atomic_fetch_add(&numDirectories, 1);
            struct manager_args *new_args = malloc(sizeof(struct manager_args));
            if (!new_args) {
                perror("Failed to allocate memory for manager args");
                closedir(dir);
                free(args->src_dir);
                free(args->dst_dir);
                free(arg);
                return NULL;
            }
            new_args->src_dir = strdup(src_path);
            new_args->dst_dir = strdup(dst_path);
            manager_recursive(new_args);
        } else if (entry->d_type == DT_REG) {
            atomic_fetch_add(&numRegularFiles, 1);
            FilePair pair = { .src = "", .dst = "" };
            strcpy(pair.src, src_path);
            strcpy(pair.dst, dst_path);
            buffer_put(&buffer, pair);
        }
    }
    closedir(dir);
    free(args->src_dir);
    free(args->dst_dir);
    free(arg);
    return NULL;
}

void copy_file(const char *src, const char *dst) {
    struct stat stat_buf;
    if (stat(src, &stat_buf) == -1) {
        perror("Stat source file/directory");
        return;
    }

    if (S_ISDIR(stat_buf.st_mode)) {
        mkdir(dst, stat_buf.st_mode);
        return;
    }

    int input_fd = open(src, O_RDONLY);
    if (input_fd == -1) {
        perror("Open source file");
        return;
    }
    int output_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output_fd == -1) {
        perror("Open destination file");
        close(input_fd);
        return;
    }

    char buffer[4096];
    ssize_t read_bytes;
    while ((read_bytes = read(input_fd, buffer, sizeof(buffer))) > 0) {
        atomic_fetch_add(&totalBytesCopied, read_bytes);
        if (write(output_fd, buffer, read_bytes) != read_bytes) {
            perror("Write to destination file");
            break;
        }
    }

    close(input_fd);
    close(output_fd);
}

void *worker(void *arg) {
    Buffer *buffer = (Buffer *)arg;
    while (1) {
        FilePair file = buffer_get(buffer);
        if (terminate) {
            pthread_exit(NULL);
        }
        copy_file(file.src, file.dst);
    }
    pthread_barrier_wait(&barrier);
    pthread_exit(NULL);
}

void handle_signal(int sig) {
    if (sig == SIGINT) {
        terminate = 1;
        pthread_mutex_lock(&buffer.lock);
        buffer.stop = 1;
        pthread_cond_broadcast(&buffer.can_consume);
        pthread_mutex_unlock(&buffer.lock);
        printf("Exiting with ctrl+c\n");

        gettimeofday(&end, NULL);
        long seconds = (end.tv_sec - start.tv_sec);
        long milliseconds = ((seconds * 1000) + (end.tv_usec - start.tv_usec) / 1000);

        long minutes = seconds / 60;
        seconds = seconds % 60;
        milliseconds = milliseconds % 1000;

        printf("\n---------------STATISTICS--------------------\n");
        printf("Consumers: %d - Buffer Size: %d\n", worker_count , buffer_size );
        printf("Number of Regular Files: %d\n", numRegularFiles);
        printf("Number of FIFO Files: %d\n", numFIFOFiles);
        printf("Number of Directories: %d\n", numDirectories);
        printf("TOTAL BYTES COPIED: %ld\n", totalBytesCopied);
        printf("TOTAL TIME: %02ld:%02ld.%03ld (min:sec.milli)\n", minutes, seconds, milliseconds);

        exit(EXIT_SUCCESS);
    }
}

int main(int argc, char *argv[]) {
    gettimeofday(&start, NULL);

    Arguments args = parse_arguments(argc, argv);
    buffer_size = args.buffer_size;
    worker_count = args.num_workers;

    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    pthread_barrier_init(&barrier, NULL, args.num_workers + 1);
    init_buffer(&buffer, args.buffer_size);

    workers = malloc(args.num_workers * sizeof(pthread_t));
    if (!workers) {
        perror("Failed to allocate memory for workers");
        destroy_buffer(&buffer);
        exit(EXIT_FAILURE);
    }

    struct manager_args *mgr_args = malloc(sizeof(struct manager_args));
    if (!mgr_args) {
        perror("Failed to allocate memory for manager args");
        free(workers);
        destroy_buffer(&buffer);
        exit(EXIT_FAILURE);
    }
    mgr_args->src_dir = strdup(args.src_dir);
    mgr_args->dst_dir = strdup(args.dst_dir);
    if (!mgr_args->src_dir || !mgr_args->dst_dir) {
        perror("Failed to allocate memory for directory paths");
        free(mgr_args->src_dir);
        free(mgr_args->dst_dir);
        free(mgr_args);
        free(workers);
        destroy_buffer(&buffer);
        exit(EXIT_FAILURE);
    }

    if (pthread_create(&mgr_thread, NULL, manager_recursive, mgr_args) != 0) {
        perror("Failed to create manager thread");
        free(mgr_args->src_dir);
        free(mgr_args->dst_dir);
        free(mgr_args);
        free(workers);
        destroy_buffer(&buffer);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < args.num_workers; ++i) {
        if (pthread_create(&workers[i], NULL, worker, &buffer) != 0) {
            perror("Failed to create worker thread");
            terminate = 1;
            pthread_barrier_wait(&barrier);
            break;
        }
    }

    pthread_join(mgr_thread, NULL);

    buffer.stop = 1;
    pthread_cond_broadcast(&buffer.can_consume);

    for (int i = 0; i < args.num_workers; ++i) {
        pthread_join(workers[i], NULL);
    }
    pthread_barrier_destroy(&barrier);
    destroy_buffer(&buffer);
    free(workers);

    gettimeofday(&end, NULL);
    long seconds = (end.tv_sec - start.tv_sec);
    long milliseconds = ((seconds * 1000) + (end.tv_usec - start.tv_usec) / 1000);

    long minutes = seconds / 60;
    seconds = seconds % 60;
    milliseconds = milliseconds % 1000;

    printf("\n---------------STATISTICS--------------------\n");
    printf("Consumers: %d - Buffer Size: %d\n", worker_count , buffer_size );
    printf("Number of Regular Files: %d\n", numRegularFiles);
    printf("Number of FIFO Files: %d\n", numFIFOFiles);
    printf("Number of Directories: %d\n", numDirectories);
    printf("TOTAL BYTES COPIED: %ld\n", totalBytesCopied);
    printf("TOTAL TIME: %02ld:%02ld.%03ld (min:sec.milli)\n", minutes, seconds, milliseconds);

    return 0;
}
