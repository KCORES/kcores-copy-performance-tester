#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>

// Define copy mode enum
typedef enum {
    SYSTEM_CP,
    MMAP,
    DIRECT_IO,
    DIRECT_IO_MEMORY_IMPACT
} CopyMode;

// Define file copy task structure
typedef struct {
    char *src_path;
    char *dst_path;
    CopyMode mode;
    double size_mib;
    double duration;
    double speed;
} CopyTask;

// Constants definition
#define BLOCK_SIZE 512
#define MAX_READ_SIZE (1024 * 1024 * 1024)  // 1GB
#define MMAP_CHUNK_SIZE (512 * 1024 * 1024) // 512MB 


// Simplified system cp command copy function
static int copy_using_cp(const char *src, const char *dst) {
    char command[1024];
    snprintf(command, sizeof(command), "cp %s %s", src, dst);
    return system(command);
}

// Simplified mmap copy function
static int copy_using_mmap(const char *src, const char *dst, size_t file_size) {
    int src_fd = open(src, O_RDONLY);
    int dst_fd = open(dst, O_RDWR | O_CREAT, 0644);
    if (src_fd < 0 || dst_fd < 0) {
        return -1;
    }

    if (ftruncate(dst_fd, file_size) != 0) {
        close(src_fd);
        close(dst_fd);
        return -1;
    }

    size_t remaining = file_size;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk_size = (remaining < MMAP_CHUNK_SIZE) ? remaining : MMAP_CHUNK_SIZE;
        
        void *src_map = mmap(NULL, chunk_size, PROT_READ, MAP_PRIVATE, src_fd, offset);
        void *dst_map = mmap(NULL, chunk_size, PROT_WRITE, MAP_SHARED, dst_fd, offset);
        
        if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
            close(src_fd);
            close(dst_fd);
            return -1;
        }

        memcpy(dst_map, src_map, chunk_size);
        msync(dst_map, chunk_size, MS_SYNC);
        
        munmap(src_map, chunk_size);
        munmap(dst_map, chunk_size);
        
        remaining -= chunk_size;
        offset += chunk_size;
    }

    close(src_fd);
    close(dst_fd);
    return 0;
}

// Simplified direct I/O copy function
static int copy_using_direct_io(const char *src, const char *dst, size_t file_size) {
    int src_fd = open(src, O_RDONLY | O_DIRECT);
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_DIRECT, 0644);
    
    if (src_fd < 0 || dst_fd < 0) {
        return -1;
    }

    // Allocate aligned buffer
    void *buffer = NULL;
    if (posix_memalign(&buffer, BLOCK_SIZE, MAX_READ_SIZE) != 0) {
        close(src_fd);
        close(dst_fd);
        return -1;
    }

    size_t remaining = file_size;
    while (remaining > 0) {
        size_t to_read = (remaining < MAX_READ_SIZE) ? remaining : MAX_READ_SIZE;
        to_read = (to_read / BLOCK_SIZE) * BLOCK_SIZE;  // Align to block size
        
        ssize_t bytes_read = read(src_fd, buffer, to_read);
        if (bytes_read <= 0) break;
        
        ssize_t bytes_written = write(dst_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) break;
        
        remaining -= bytes_read;
    }

    free(buffer);
    close(src_fd);
    close(dst_fd);
    return (remaining == 0) ? 0 : -1;
}

// Add new copy function
static int copy_using_direct_io_memory_impact(const char *src, const char *dst, size_t file_size) {
    // Use system page size as base alignment unit
    const size_t page_size = sysconf(_SC_PAGESIZE);
    // Use 2MB as DMA transfer block size
    const size_t dma_block_size = 2 * 1024 * 1024;  // 2MB
    
    void *src_buffer = NULL;
    void *dst_buffer = NULL;
    
    // Ensure buffers are page-aligned
    if (posix_memalign(&src_buffer, page_size, MAX_READ_SIZE) != 0 ||
        posix_memalign(&dst_buffer, page_size, MAX_READ_SIZE) != 0) {
        free(src_buffer);
        free(dst_buffer);
        return -1;
    }

    // Use linear congruential generator to fill source buffer with random data
    uint64_t *src_ptr = (uint64_t *)src_buffer;
    uint64_t random_value = 0x0123456789ABCDEF;  // Initial seed
    const uint64_t multiplier = 6364136223846793005ULL;
    const uint64_t increment = 1;
    
    for (size_t i = 0; i < MAX_READ_SIZE / sizeof(uint64_t); i++) {
        random_value = random_value * multiplier + increment;
        src_ptr[i] = random_value;
    }

    // Force memory barrier to ensure initialization is complete
    __sync_synchronize();

    // Simulate DMA transfer process
    size_t remaining = file_size;
    char *current_src = (char *)src_buffer;
    char *current_dst = (char *)dst_buffer;
    volatile uint64_t checksum = 0;

    while (remaining > 0) {
        size_t current_chunk = (remaining < MAX_READ_SIZE) ? remaining : MAX_READ_SIZE;
        size_t chunk_remaining = current_chunk;
        
        char *chunk_src = current_src;
        char *chunk_dst = current_dst;
        
        // Transfer by DMA block size
        while (chunk_remaining >= dma_block_size) {
            chunk_dst = (char *)memcpy(chunk_dst, chunk_src, dma_block_size) + dma_block_size;
            chunk_src += dma_block_size;
            chunk_remaining -= dma_block_size;
            
            // Verify after each DMA block transfer
            for (size_t i = 0; i < dma_block_size; i += page_size) {
                checksum ^= *(volatile uint64_t *)(chunk_dst - dma_block_size + i);
            }
        }
        
        // Handle remaining bytes
        if (chunk_remaining > 0) {
            // Ensure remaining portion is also page-aligned
            size_t aligned_remaining = (chunk_remaining + page_size - 1) & ~(page_size - 1);
            chunk_dst = (char *)memcpy(chunk_dst, chunk_src, aligned_remaining) + aligned_remaining;
            
            // Verify remaining portion
            for (size_t i = 0; i < aligned_remaining; i += page_size) {
                checksum ^= *(volatile uint64_t *)(chunk_dst - aligned_remaining + i);
            }
        }
        
        remaining -= current_chunk;
    }

    free(src_buffer);
    free(dst_buffer);
    
    return (checksum != 0) ? 0 : -1;
}

// Thread copy function
void* copy_file_thread(void *arg) {
    CopyTask *task = (CopyTask *)arg;
    struct timeval start, end;
    gettimeofday(&start, NULL);

    struct stat st;
    stat(task->src_path, &st);
    task->size_mib = st.st_size / (1024.0 * 1024.0);

    int result = -1;
    switch (task->mode) {
        case SYSTEM_CP:
            result = copy_using_cp(task->src_path, task->dst_path);
            break;
        case MMAP:
            result = copy_using_mmap(task->src_path, task->dst_path, st.st_size);
            break;
        case DIRECT_IO:
            result = copy_using_direct_io(task->src_path, task->dst_path, st.st_size);
            break;
        case DIRECT_IO_MEMORY_IMPACT:
            result = copy_using_direct_io_memory_impact(task->src_path, task->dst_path, st.st_size);
            break;
    }

    gettimeofday(&end, NULL);
    task->duration = (end.tv_sec - start.tv_sec) + 
                    (end.tv_usec - start.tv_usec) / 1000000.0;
    task->speed = task->size_mib / task->duration;

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: %s --mode [cp|mmap|direct_io|direct_io_memory_impact] --from file1 [file2 ...] --to dest_dir\n", argv[0]);
        return 1;
    }

    // Parse command line arguments...
    // Simplified example assumes fixed argument format

    CopyMode mode;
    if (strcmp(argv[2], "cp") == 0) mode = SYSTEM_CP;
    else if (strcmp(argv[2], "mmap") == 0) mode = MMAP;
    else if (strcmp(argv[2], "direct_io") == 0) mode = DIRECT_IO;
    else if (strcmp(argv[2], "direct_io_memory_impact") == 0) mode = DIRECT_IO_MEMORY_IMPACT;
    else {
        printf("Invalid mode\n");
        return 1;
    }

    // Create and execute copy tasks
    int num_files = (argc - 6);  // Subtract fixed argument count
    CopyTask *tasks = malloc(sizeof(CopyTask) * num_files);
    pthread_t *threads = malloc(sizeof(pthread_t) * num_files);

    // Start all copy threads
    for (int i = 0; i < num_files; i++) {
        tasks[i].src_path = argv[i + 4];
        tasks[i].dst_path = malloc(strlen(argv[argc-1]) + strlen(argv[i + 4]) + 2);
        sprintf(tasks[i].dst_path, "%s/%s", argv[argc-1], basename(argv[i + 4]));
        tasks[i].mode = mode;
        
        pthread_create(&threads[i], NULL, copy_file_thread, &tasks[i]);
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_files; i++) {
        pthread_join(threads[i], NULL);
    }

    // Print results
    printf("\nDetailed Results:\n");
    printf("%-10s %-30s %-12s %-12s %-12s\n", "Thread ID", "Filename", "Size (MiB)", "Duration (s)", "Speed (MiB/s)");
    printf("--------------------------------------------------------------------------------\n");

    double total_size = 0, total_duration = 0;
    for (int i = 0; i < num_files; i++) {
        printf("%-10d %-30s %11.2f %11.2f %11.2f\n", 
               i,  // Thread ID
               basename(tasks[i].src_path), 
               tasks[i].size_mib, 
               tasks[i].duration, 
               tasks[i].speed);
        total_size += tasks[i].size_mib;
        total_duration = (tasks[i].duration > total_duration) ? tasks[i].duration : total_duration;
    }

    printf("\nTotal Statistics:\n");
    printf("Total Size: %.2f MiB\n", total_size);
    printf("Total Duration: %.2f seconds\n", total_duration);
    printf("Average Speed: %.2f MiB/s\n", total_size / total_duration);

    // Cleanup resources
    for (int i = 0; i < num_files; i++) {
        free(tasks[i].dst_path);
    }
    free(tasks);
    free(threads);

    return 0;
}
