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
#include <ctype.h>
#include <stdbool.h>
#include <libgen.h>
#include <math.h>


// Define copy mode enum
typedef enum {
    SYSTEM_CP,
    MMAP,
    DIRECT_IO,
    DIRECT_IO_MEMORY_IMPACT,
    GENERATE_TEST_FILES
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


// random number generator structure and functions
typedef struct {
    uint64_t seed;
    uint64_t multiplier;
    uint64_t increment;
} RandomGenerator;

static void init_random_generator(RandomGenerator *gen) {
    gen->seed = 0x0123456789ABCDEF;  // Initial seed
    gen->multiplier = 6364136223846793005ULL;
    gen->increment = 1;
}

static void fill_buffer_with_random_data(RandomGenerator *gen, void *buffer, size_t size) {
    uint64_t *ptr = (uint64_t *)buffer;
    size_t num_elements = size / sizeof(uint64_t);
    
    for (size_t i = 0; i < num_elements; i++) {
        gen->seed = gen->seed * gen->multiplier + gen->increment;
        ptr[i] = gen->seed;
    }
    
    // Force memory barrier to ensure initialization is complete
    __sync_synchronize();
}


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

    // Replace original random data generation code
    RandomGenerator gen;
    init_random_generator(&gen);
    fill_buffer_with_random_data(&gen, src_buffer, MAX_READ_SIZE);

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

// Parse file size string
static uint64_t parse_size(const char *size_str) {
    uint64_t size;
    char unit;
    if (sscanf(size_str, "%lu%c", &size, &unit) != 2) {
        return 0;
    }
    
    switch (toupper(unit)) {
        case 'T':
            size *= 1024;
        case 'G':
            size *= 1024;
        case 'M':
            size *= 1024 * 1024;
            break;
        default:
            return 0;
    }
    
    return size;
}

// Generate test file
static int generate_test_file(const char *path, uint64_t size) {
    int fd;
    void *buf = NULL;
    const size_t buf_size = 1024 * 1024; // 1MB buffer
    uint64_t remaining = size;
    
    // Use O_DIRECT for better performance
    fd = open(path, O_WRONLY | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        // If O_DIRECT fails, try without it
        fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            perror("open");
            return -1;
        }
    }
    
    // Allocate aligned memory
    if (posix_memalign(&buf, 512, buf_size)) {
        close(fd);
        return -1;
    }
    
    // Replace original random data generation code
    RandomGenerator gen;
    init_random_generator(&gen);
    fill_buffer_with_random_data(&gen, buf, buf_size);
    
    // Write to file
    while (remaining > 0) {
        size_t to_write = (remaining < buf_size) ? remaining : buf_size;
        // Ensure write size is multiple of 512 (for O_DIRECT)
        to_write = (to_write / 512) * 512;
        
        ssize_t written = write(fd, buf, to_write);
        if (written < 0) {
            perror("write");
            free(buf);
            close(fd);
            return -1;
        }
        remaining -= written;
    }
    
    // Cleanup
    free(buf);
    if (fsync(fd) < 0) {
        perror("fsync");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

// Thread function for generating test files
typedef struct {
    char *path;
    uint64_t size;
    int index;
    double duration;
} GenerateTask;

void* generate_file_thread(void *arg) {
    GenerateTask *task = (GenerateTask *)arg;
    struct timeval start, end;
    
    gettimeofday(&start, NULL);
    int result = generate_test_file(task->path, task->size);
    gettimeofday(&end, NULL);
    
    task->duration = (end.tv_sec - start.tv_sec) + 
                    (end.tv_usec - start.tv_usec) / 1000000.0;
    
    return (void*)(long)result;
}

// New function: handle generate test files mode
static int handle_generate_test_files(int argc, char *argv[]) {
    if (argc < 7) {
        printf("Missing parameters for generate_test_files mode\n");
        return 1;
    }
    
    uint64_t file_size = 0;
    int num_files = 0;
    char *output_dir = ".";  // Default to current directory
    
    // Parse arguments
    for (int i = 3; i < argc; i += 2) {
        if (strcmp(argv[i], "--size") == 0) {
            file_size = parse_size(argv[i+1]);
        } else if (strcmp(argv[i], "--num") == 0) {
            num_files = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--dir") == 0) {
            output_dir = argv[i+1];
        }
    }
    
    if (file_size == 0 || num_files <= 0) {
        printf("Invalid size or number of files\n");
        return 1;
    }
    
    // Create and execute generation tasks
    GenerateTask *tasks = malloc(sizeof(GenerateTask) * num_files);
    pthread_t *threads = malloc(sizeof(pthread_t) * num_files);
    
    printf("Generating %d test files of size %luB each in %s\n", 
           num_files, file_size, output_dir);
    
    for (int i = 0; i < num_files; i++) {
        tasks[i].path = malloc(strlen(output_dir) + 32);
        sprintf(tasks[i].path, "%s/test_file_%d", output_dir, i + 1);
        tasks[i].size = file_size;
        tasks[i].index = i;
        
        pthread_create(&threads[i], NULL, generate_file_thread, &tasks[i]);
    }
    
    // Wait for all threads to complete
    bool all_success = true;
    for (int i = 0; i < num_files; i++) {
        void *thread_result;
        pthread_join(threads[i], &thread_result);
        if ((long)thread_result != 0) {
            all_success = false;
        }
    }
    
    // Print results
    printf("\nGeneration Results:\n");
    printf("%-10s %-30s %-15s %-12s\n", 
           "File #", "Path", "Size", "Duration (s)");
    printf("------------------------------------------------------------\n");
    
    double total_duration = 0;
    for (int i = 0; i < num_files; i++) {
        printf("%-10d %-30s %-15lu %11.2f\n",
               i + 1, tasks[i].path, file_size, tasks[i].duration);
        total_duration = (tasks[i].duration > total_duration) ? 
                        tasks[i].duration : total_duration;
    }
    
    printf("\nTotal Statistics:\n");
    printf("Total Size: %.2f GiB\n", 
           (file_size * num_files) / (1024.0 * 1024.0 * 1024.0));
    printf("Total Duration: %.2f seconds\n", total_duration);
    printf("Average Speed: %.2f MiB/s\n", 
           (file_size * num_files) / (1024.0 * 1024.0) / total_duration);
    
    // Cleanup resources
    for (int i = 0; i < num_files; i++) {
        free(tasks[i].path);
    }
    free(tasks);
    free(threads);
    
    return all_success ? 0 : 1;
}

// Add benchmark result structure
typedef struct {
    char *filename;
    double size_mib;
    double memory_duration;
    double memory_speed;
    double disk_duration;
    double disk_speed;
} BenchmarkResult;

// New function to handle benchmark mode
static int handle_benchmark(int argc, char *argv[]) {
    uint64_t file_size = 0;
    int num_files = 0;
    char *from_dir = NULL;
    char *to_dir = NULL;
    
    // Parse arguments
    for (int i = 3; i < argc; i += 2) {
        if (strcmp(argv[i], "--size") == 0) {
            file_size = parse_size(argv[i+1]);
        } else if (strcmp(argv[i], "--num") == 0) {
            num_files = atoi(argv[i+1]);
        } else if (strcmp(argv[i], "--from") == 0) {
            from_dir = argv[i+1];
        } else if (strcmp(argv[i], "--to") == 0) {
            to_dir = argv[i+1];
        }
    }
    
    if (file_size == 0 || num_files <= 0 || !from_dir || !to_dir) {
        printf("Invalid parameters for benchmark mode\n");
        return 1;
    }

    // Generate test files first
    printf("Generating test files...\n");
    GenerateTask *gen_tasks = malloc(sizeof(GenerateTask) * num_files);
    pthread_t *gen_threads = malloc(sizeof(pthread_t) * num_files);
    
    for (int i = 0; i < num_files; i++) {
        gen_tasks[i].path = malloc(strlen(from_dir) + 32);
        sprintf(gen_tasks[i].path, "%s/test_file_%d", from_dir, i + 1);
        gen_tasks[i].size = file_size;
        gen_tasks[i].index = i;
        pthread_create(&gen_threads[i], NULL, generate_file_thread, &gen_tasks[i]);
    }
    
    for (int i = 0; i < num_files; i++) {
        pthread_join(gen_threads[i], NULL);
    }

    // Prepare benchmark results array
    BenchmarkResult *results = malloc(sizeof(BenchmarkResult) * num_files);
    
    // Run memory impact tests using existing function
    printf("\nRunning memory copy tests...\n");
    for (int i = 0; i < num_files; i++) {
        CopyTask task;
        task.src_path = gen_tasks[i].path;
        task.dst_path = malloc(strlen(to_dir) + 32);
        sprintf(task.dst_path, "%s/test_file_%d", to_dir, i + 1);
        task.mode = DIRECT_IO_MEMORY_IMPACT;

        copy_file_thread(&task);

        results[i].filename = strdup(basename(task.src_path));
        results[i].size_mib = task.size_mib;
        results[i].memory_duration = task.duration;
        results[i].memory_speed = task.speed;

        free(task.dst_path);
    }

    // Run disk copy tests using direct_io mode
    printf("\nRunning disk copy tests...\n");
    for (int i = 0; i < num_files; i++) {
        CopyTask task;
        task.src_path = gen_tasks[i].path;
        task.dst_path = malloc(strlen(to_dir) + 32);
        sprintf(task.dst_path, "%s/test_file_%d_disk", to_dir, i + 1);
        task.mode = DIRECT_IO;

        copy_file_thread(&task);

        results[i].disk_duration = task.duration;
        results[i].disk_speed = task.speed;

        free(task.dst_path);
    }

    // Calculate total statistics
    double total_size = 0, total_memory_duration = 0, total_disk_duration = 0;
    for (int i = 0; i < num_files; i++) {
        total_size += results[i].size_mib;
        total_memory_duration = fmax(total_memory_duration, results[i].memory_duration);
        total_disk_duration = fmax(total_disk_duration, results[i].disk_duration);
    }
    double avg_memory_speed = total_size / total_memory_duration;
    double avg_disk_speed = total_size / total_disk_duration;

    // Print results
    printf("\nBenchmark Results:\n");
    printf("%-10s %-20s %-12s %-20s %-20s %-20s %-20s\n",
           "Thread ID", "Filename", "Size (MiB)",
           "Memory Copy (s)", "Memory Speed (MiB/s)",
           "Disk Copy (s)", "Disk Speed (MiB/s)");
    printf("--------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < num_files; i++) {
        printf("%-10d %-20s %11.2f %19.2f %19.2f %19.2f %19.2f\n",
               i, results[i].filename, results[i].size_mib,
               results[i].memory_duration, results[i].memory_speed,
               results[i].disk_duration, results[i].disk_speed);
    }

    printf("\nTotal Statistics:\n");
    printf("Total Size: %.2f MiB\n", total_size);
    printf("Memory Copy - Total Duration: %.2f seconds, Average Speed: %.2f MiB/s\n",
           total_memory_duration, avg_memory_speed);
    printf("Disk Copy   - Total Duration: %.2f seconds, Average Speed: %.2f MiB/s\n",
           total_disk_duration, avg_disk_speed);

    double speed_ratio = avg_disk_speed / avg_memory_speed;
    if (speed_ratio >= 0.95) {
        printf("\033[41m\033[37mYou may hit the memory bandwidth wall\033[0m\n");
    }

    // Cleanup
    for (int i = 0; i < num_files; i++) {
        free(gen_tasks[i].path);
        free(results[i].filename);
    }
    free(gen_tasks);
    free(gen_threads);
    free(results);

    return 0;
}

// Print usage information
static void print_usage(const char *program_name) {
    printf("Usage:\n");
    printf("  Copy files:\n");
    printf("    %s --mode [cp|mmap|direct_io|direct_io_memory_impact] --from file1 [file2 ...] --to dest_dir\n", program_name);
    printf("  Generate test files:\n");
    printf("    %s --mode generate_test_files --size <size>[M|G|T] --num <number> [--dir <output_dir>]\n", program_name);
    printf("  Benchmark:\n");
    printf("    %s --mode benchmark --size <size>[M|G|T] --num <number> --from <source_dir> --to <dest_dir>\n", program_name);
}

// Parse copy mode from command line argument
static CopyMode parse_copy_mode(const char *mode_str) {
    if (strcmp(mode_str, "cp") == 0) return SYSTEM_CP;
    if (strcmp(mode_str, "mmap") == 0) return MMAP;
    if (strcmp(mode_str, "direct_io") == 0) return DIRECT_IO;
    if (strcmp(mode_str, "direct_io_memory_impact") == 0) return DIRECT_IO_MEMORY_IMPACT;
    return -1;
}

// Print copy results
static void print_copy_results(CopyTask *tasks, int num_files) {
    printf("\nDetailed Results:\n");
    printf("%-10s %-30s %-12s %-12s %-12s\n", 
           "Thread ID", "Filename", "Size (MiB)", "Duration (s)", "Speed (MiB/s)");
    printf("--------------------------------------------------------------------------------\n");

    double total_size = 0, total_duration = 0;
    for (int i = 0; i < num_files; i++) {
        printf("%-10d %-30s %11.2f %11.2f %11.2f\n", 
               i, basename(tasks[i].src_path), 
               tasks[i].size_mib, tasks[i].duration, tasks[i].speed);
        total_size += tasks[i].size_mib;
        total_duration = (tasks[i].duration > total_duration) ? 
                        tasks[i].duration : total_duration;
    }

    printf("\nTotal Statistics:\n");
    printf("Total Size: %.2f MiB\n", total_size);
    printf("Total Duration: %.2f seconds\n", total_duration);
    printf("Average Speed: %.2f MiB/s\n", total_size / total_duration);
}

// Handle file copy mode
static int handle_copy_files(int argc, char *argv[], CopyMode mode) {
    int num_files = (argc - 6);
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

    // Wait for completion
    for (int i = 0; i < num_files; i++) {
        pthread_join(threads[i], NULL);
    }

    // Print results and cleanup
    print_copy_results(tasks, num_files);

    for (int i = 0; i < num_files; i++) {
        free(tasks[i].dst_path);
    }
    free(tasks);
    free(threads);

    return 0;
}

// Update main function to include benchmark mode
int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    // Handle generate test files mode
    if (strcmp(argv[2], "generate_test_files") == 0) {
        return handle_generate_test_files(argc, argv);
    }
    
    // Handle benchmark mode
    if (strcmp(argv[2], "benchmark") == 0) {
        return handle_benchmark(argc, argv);
    }
    
    // Handle copy mode
    CopyMode mode = parse_copy_mode(argv[2]);
    if (mode == -1) {
        printf("Invalid mode\n");
        return 1;
    }

    return handle_copy_files(argc, argv, mode);
}
