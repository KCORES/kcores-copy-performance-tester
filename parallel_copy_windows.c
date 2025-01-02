#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

// Define copy mode enum
typedef enum {
    SYSTEM_CP,
    MMAP,
    DIRECT_IO,
    DIRECT_IO_MEMORY_IMPACT,
    GENERATE_TEST_FILES,
    BENCHMARK
} CopyMode;

// Define file copy task structure
typedef struct {
    wchar_t *src_path;  // Use wide char to support Unicode
    wchar_t *dst_path;
    CopyMode mode;
    double size_mib;
    double duration;
    double speed;
    uint64_t test_file_size;  // Add test file size parameter
} CopyTask;

// Constants definition
#define BLOCK_SIZE 512
#define MAX_READ_SIZE (1024 * 1024 * 1024)  // 1GB
#define MMAP_CHUNK_SIZE (512 * 1024 * 1024) // 512MB 

// Random number generator structure
typedef struct {
    uint64_t seed;
    uint64_t multiplier;
    uint64_t increment;
} RandomGenerator;

// Add benchmark result structure
typedef struct {
    wchar_t *filename;
    double size_mib;
    double memory_duration;
    double memory_speed;
    double disk_duration;
    double disk_speed;
} BenchmarkResult;

// Helper function to get high-resolution time in seconds
static double get_time() {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

// Random generator functions
static void init_random_generator(RandomGenerator *gen) {
    gen->seed = 0x0123456789ABCDEF;
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
    
    // Memory barrier
    MemoryBarrier();
}

// System copy function using Windows API with CopyFile2
static int copy_using_system_cp(const wchar_t *src, const wchar_t *dst) {
    COPYFILE2_EXTENDED_PARAMETERS params = {0};
    params.dwSize = sizeof(COPYFILE2_EXTENDED_PARAMETERS);
    params.dwCopyFlags = 0; // You can set specific flags here if needed

    HRESULT result = CopyFile2(src, dst, &params);
    if (FAILED(result)) {
        return HRESULT_CODE(result);
    }
    return 0;
}

// Memory-mapped copy function using Windows API
static int copy_using_mmap(const wchar_t *src, const wchar_t *dst, size_t file_size) {
    HANDLE src_handle = CreateFileW(src, 
        GENERIC_READ, 
        FILE_SHARE_READ, 
        NULL, 
        OPEN_EXISTING, 
        FILE_ATTRIBUTE_NORMAL, 
        NULL);

    if (src_handle == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    HANDLE dst_handle = CreateFileW(dst,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (dst_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(src_handle);
        return GetLastError();
    }

    // Set the size of destination file first
    LARGE_INTEGER li;
    li.QuadPart = file_size;
    if (!SetFilePointerEx(dst_handle, li, NULL, FILE_BEGIN) ||
        !SetEndOfFile(dst_handle)) {
        CloseHandle(src_handle);
        CloseHandle(dst_handle);
        return GetLastError();
    }

    int result = 0;
    size_t remaining = file_size;
    size_t offset = 0;

    while (remaining > 0 && result == 0) {
        size_t chunk_size = (remaining < MMAP_CHUNK_SIZE) ? remaining : MMAP_CHUNK_SIZE;

        // Create file mappings with proper size parameters
        HANDLE src_map = CreateFileMappingW(src_handle, 
            NULL, 
            PAGE_READONLY, 
            (DWORD)(file_size >> 32),
            (DWORD)file_size,
            NULL);

        HANDLE dst_map = CreateFileMappingW(dst_handle,
            NULL,
            PAGE_READWRITE,
            (DWORD)(file_size >> 32),
            (DWORD)file_size,
            NULL);

        if (src_map == NULL || dst_map == NULL) {
            result = GetLastError();
            break;
        }

        // Map views with correct offset
        LPVOID src_view = MapViewOfFile(src_map,
            FILE_MAP_READ,
            (DWORD)(offset >> 32),
            (DWORD)offset,
            chunk_size);

        LPVOID dst_view = MapViewOfFile(dst_map,
            FILE_MAP_WRITE,
            (DWORD)(offset >> 32),
            (DWORD)offset,
            chunk_size);

        if (src_view == NULL || dst_view == NULL) {
            result = GetLastError();
        } else {
            memcpy(dst_view, src_view, chunk_size);
            FlushViewOfFile(dst_view, chunk_size);
        }

        // Cleanup mappings
        if (src_view) UnmapViewOfFile(src_view);
        if (dst_view) UnmapViewOfFile(dst_view);
        if (src_map) CloseHandle(src_map);
        if (dst_map) CloseHandle(dst_map);

        remaining -= chunk_size;
        offset += chunk_size;
    }

    CloseHandle(src_handle);
    CloseHandle(dst_handle);
    return result;
}

// Direct I/O copy function using Windows API
static int copy_using_direct_io(const wchar_t *src, const wchar_t *dst, size_t file_size) {
    HANDLE src_handle = CreateFileW(src, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    HANDLE dst_handle = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING, NULL);

    if (src_handle == INVALID_HANDLE_VALUE || dst_handle == INVALID_HANDLE_VALUE) {
        if (src_handle != INVALID_HANDLE_VALUE) CloseHandle(src_handle);
        if (dst_handle != INVALID_HANDLE_VALUE) CloseHandle(dst_handle);
        return GetLastError();
    }

    void *buffer = _aligned_malloc(MAX_READ_SIZE, BLOCK_SIZE);
    if (!buffer) {
        CloseHandle(src_handle);
        CloseHandle(dst_handle);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    DWORD bytes_read, bytes_written;
    size_t remaining = file_size;
    while (remaining > 0) {
        size_t to_read = (remaining < MAX_READ_SIZE) ? remaining : MAX_READ_SIZE;
        to_read = (to_read / BLOCK_SIZE) * BLOCK_SIZE;  // Align to block size
        DWORD bytes_to_read = (to_read > MAXDWORD) ? MAXDWORD : (DWORD)to_read;  // Explicit cast to DWORD

        if (!ReadFile(src_handle, buffer, bytes_to_read, &bytes_read, NULL) || bytes_read == 0) break;
        if (!WriteFile(dst_handle, buffer, bytes_read, &bytes_written, NULL) || bytes_written != bytes_read) break;

        remaining -= bytes_read;
    }

    _aligned_free(buffer);
    CloseHandle(src_handle);
    CloseHandle(dst_handle);
    return (remaining == 0) ? 0 : GetLastError();
}

// Memory impact copy function (simulated)
static int copy_using_direct_io_memory_impact(const wchar_t *src, const wchar_t *dst, size_t file_size) {
    // Get Windows system page size
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    const size_t page_size = sys_info.dwPageSize;
    
    // Use 2MB as DMA transfer block size (same as Linux version)
    const size_t dma_block_size = 2 * 1024 * 1024;  // 2MB
    
    // Allocate page-aligned memory using Windows API
    void *src_buffer = VirtualAlloc(NULL, MAX_READ_SIZE, 
                                  MEM_COMMIT | MEM_RESERVE, 
                                  PAGE_READWRITE);
    void *dst_buffer = VirtualAlloc(NULL, MAX_READ_SIZE, 
                                  MEM_COMMIT | MEM_RESERVE, 
                                  PAGE_READWRITE);
    
    if (!src_buffer || !dst_buffer) {
        if (src_buffer) VirtualFree(src_buffer, 0, MEM_RELEASE);
        if (dst_buffer) VirtualFree(dst_buffer, 0, MEM_RELEASE);
        return GetLastError();
    }

    // Generate random data
    RandomGenerator gen;
    init_random_generator(&gen);
    fill_buffer_with_random_data(&gen, src_buffer, MAX_READ_SIZE);

    // Force memory barrier using Windows API
    MemoryBarrier();

    // Simulate DMA transfer process
    size_t remaining = file_size;
    char *current_src = (char *)src_buffer;
    char *current_dst = (char *)dst_buffer;
    volatile UINT64 checksum = 0;

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
                checksum ^= *(volatile UINT64 *)(chunk_dst - dma_block_size + i);
            }
        }
        
        // Handle remaining bytes
        if (chunk_remaining > 0) {
            // Ensure remaining portion is page-aligned
            size_t aligned_remaining = (chunk_remaining + page_size - 1) & ~(page_size - 1);
            chunk_dst = (char *)memcpy(chunk_dst, chunk_src, aligned_remaining) + aligned_remaining;
            
            // Verify remaining portion
            for (size_t i = 0; i < aligned_remaining; i += page_size) {
                checksum ^= *(volatile UINT64 *)(chunk_dst - aligned_remaining + i);
            }
        }
        
        remaining -= current_chunk;
    }

    // Cleanup
    VirtualFree(src_buffer, 0, MEM_RELEASE);
    VirtualFree(dst_buffer, 0, MEM_RELEASE);
    
    return (checksum != 0) ? 0 : GetLastError();
}

// Generate test file function
static int generate_test_file(const wchar_t *path, uint64_t size) {
    HANDLE file_handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    void *buffer = _aligned_malloc(512, 512);  // 512 bytes aligned buffer
    if (!buffer) {
        CloseHandle(file_handle);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    RandomGenerator gen;
    init_random_generator(&gen);
    fill_buffer_with_random_data(&gen, buffer, 512);

    DWORD bytes_written;
    uint64_t remaining = size;
    while (remaining > 0) {
        DWORD to_write = (remaining < 512) ? (DWORD)remaining : 512;  // Explicit cast to DWORD
        if (!WriteFile(file_handle, buffer, to_write, &bytes_written, NULL) || bytes_written != to_write) {
            _aligned_free(buffer);
            CloseHandle(file_handle);
            return GetLastError();
        }
        remaining -= to_write;
    }

    _aligned_free(buffer);
    CloseHandle(file_handle);
    return 0;
}

// Thread function prototype
DWORD WINAPI copy_file_thread(LPVOID arg) {
    CopyTask *task = (CopyTask *)arg;
    double start_time = get_time();

    // For generate test files mode, directly call generate_test_file
    if (task->mode == GENERATE_TEST_FILES) {
        return generate_test_file(task->src_path, task->test_file_size);
    }

    // Get file size
    WIN32_FILE_ATTRIBUTE_DATA attr_data;
    if (!GetFileAttributesExW(task->src_path, GetFileExInfoStandard, &attr_data)) {
        return GetLastError();
    }

    LARGE_INTEGER file_size;
    file_size.HighPart = attr_data.nFileSizeHigh;
    file_size.LowPart = attr_data.nFileSizeLow;
    task->size_mib = file_size.QuadPart / (1024.0 * 1024.0);

    // Execute copy based on mode
    int result = -1;
    switch (task->mode) {
        case SYSTEM_CP:
            result = copy_using_system_cp(task->src_path, task->dst_path);
            break;
        case MMAP:
            result = copy_using_mmap(task->src_path, task->dst_path, file_size.QuadPart);
            break;
        case DIRECT_IO:
            result = copy_using_direct_io(task->src_path, task->dst_path, file_size.QuadPart);
            break;
        case DIRECT_IO_MEMORY_IMPACT:
            result = copy_using_direct_io_memory_impact(task->src_path, task->dst_path, file_size.QuadPart);
            break;
    }

    // Calculate duration and speed
    task->duration = get_time() - start_time;
    task->speed = task->size_mib / task->duration;

    return result;
}

// Helper function to print error messages
static void print_last_error(const wchar_t *message) {
    DWORD error_code = GetLastError();
    wchar_t *error_message = NULL;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&error_message,
        0,
        NULL
    );
    wprintf(L"%s: %s\n", message, error_message);
    LocalFree(error_message);
}

// Ensure all paths are properly formatted for Windows
static void format_path(wchar_t *path) {
    for (wchar_t *p = path; *p; ++p) {
        if (*p == L'/') *p = L'\\';
    }
}

// Helper function to parse file size with unit
static uint64_t parse_size_with_unit(const wchar_t *size_str) {
    uint64_t size = 0;
    wchar_t unit;
    
    if (swscanf_s(size_str, L"%llu%lc", &size, &unit, 1) != 2) {
        return 0;
    }
    
    switch (towupper(unit)) {
        case L'T':
            size *= 1024;
        case L'G':
            size *= 1024;
        case L'M':
            size *= 1024 * 1024;
            break;
        default:
            return 0;
    }
    
    return size;
}

// New function to handle benchmark mode
static int handle_benchmark(int argc, wchar_t *argv[]) {
    uint64_t file_size = 0;
    int num_files = 0;
    wchar_t *from_dir = NULL;
    wchar_t *to_dir = NULL;
    
    // Parse arguments
    for (int i = 3; i < argc; i += 2) {
        if (wcscmp(argv[i], L"--size") == 0) {
            file_size = parse_size_with_unit(argv[i+1]);
        } else if (wcscmp(argv[i], L"--num") == 0) {
            num_files = (int)wcstol(argv[i+1], NULL, 10);
        } else if (wcscmp(argv[i], L"--from") == 0) {
            from_dir = argv[i+1];
        } else if (wcscmp(argv[i], L"--to") == 0) {
            to_dir = argv[i+1];
        }
    }
    
    if (file_size == 0 || num_files <= 0 || !from_dir || !to_dir) {
        wprintf(L"Invalid parameters for benchmark mode:\n");
        if (file_size == 0) {
            wprintf(L"  - File size must be greater than 0\n");
        }
        if (num_files <= 0) {
            wprintf(L"  - Number of files must be greater than 0\n");
        }
        if (!from_dir) {
            wprintf(L"  - Source directory (--from) is required\n");
        }
        if (!to_dir) {
            wprintf(L"  - Destination directory (--to) is required\n");
        }
        wprintf(L"\nUsage: --mode benchmark --size <size> --num <number> --from <source_dir> --to <dest_dir>\n");
        return 1;
    }

    // Generate test files first
    wprintf(L"Generating test files...\n");
    CopyTask *gen_tasks = malloc(sizeof(CopyTask) * num_files);
    HANDLE *gen_threads = malloc(sizeof(HANDLE) * num_files);
    
    for (int i = 0; i < num_files; i++) {
        gen_tasks[i].src_path = malloc(sizeof(wchar_t) * (wcslen(from_dir) + 32));
        swprintf(gen_tasks[i].src_path, wcslen(from_dir) + 32, L"%s\\test_file_%d", from_dir, i + 1);
        gen_tasks[i].mode = GENERATE_TEST_FILES;
        gen_tasks[i].test_file_size = file_size;
    }

    // Create thread array
    HANDLE *active_handles = malloc(sizeof(HANDLE) * num_files);

    // Start all threads
    for (int i = 0; i < num_files; i++) {
        active_handles[i] = CreateThread(
            NULL,
            0,
            copy_file_thread,
            &gen_tasks[i],
            0,
            NULL
        );

        if (active_handles[i] == NULL) {
            wprintf(L"Failed to create generation thread %d\n", i);
            return 1;
        }
    }

    // Wait for all threads to complete and check results
    DWORD wait_result = WaitForMultipleObjects(
        num_files,
        active_handles,
        TRUE,  // Wait for all objects
        INFINITE
    );

    // Check wait result
    if (wait_result == WAIT_FAILED) {
        wprintf(L"Failed to wait for threads: %lu\n", GetLastError());
        return 1;
    }

    // Check execution result of each thread
    bool all_success = true;
    for (int i = 0; i < num_files; i++) {
        DWORD exit_code;
        if (GetExitCodeThread(active_handles[i], &exit_code)) {
            if (exit_code != 0) {
                wprintf(L"Thread %d failed with error code: %lu\n", i, exit_code);
                all_success = false;
            }
        } else {
            wprintf(L"Failed to get exit code for thread %d: %lu\n", i, GetLastError());
            all_success = false;
        }
        CloseHandle(active_handles[i]);
    }
    free(active_handles);

    if (!all_success) {
        // Clean up resources
        for (int i = 0; i < num_files; i++) {
            free(gen_tasks[i].src_path);
        }
        free(gen_tasks);
        return 1;
    }

    // Prepare benchmark results array
    BenchmarkResult *results = malloc(sizeof(BenchmarkResult) * num_files);
    if (!results) {
        wprintf(L"Failed to allocate memory for results\n");
        return 1;
    }

    // Run memory impact tests using existing function
    wprintf(L"\nStarting memory copy tests...\n");
    fflush(stdout);

    HANDLE *threads = malloc(sizeof(HANDLE) * num_files);  // Allocate memory for thread handles

    if (!threads) {
        wprintf(L"Failed to allocate memory for thread handles\n");
        return 1;
    }


    CopyTask *tasks = malloc(sizeof(CopyTask) * num_files);
    if (!tasks) {
        free(threads);
        wprintf(L"Failed to allocate memory for tasks\n");
        return 1;
    }


    // Create all tasks
    for (int i = 0; i < num_files; i++) {
        tasks[i].src_path = malloc(sizeof(wchar_t) * MAX_PATH);
        tasks[i].dst_path = malloc(sizeof(wchar_t) * MAX_PATH);

        
        if (!tasks[i].src_path || !tasks[i].dst_path) {
            // Clean up allocated resources
            for (int j = 0; j < i; j++) {
                free(tasks[j].src_path);
                free(tasks[j].dst_path);
            }
            free(tasks);
            free(threads);
            wprintf(L"Failed to allocate memory for paths\n");
            return 1;
        }

        
        swprintf(tasks[i].src_path, MAX_PATH, L"%s\\test_file_%d", from_dir, i + 1);
        swprintf(tasks[i].dst_path, MAX_PATH, L"%s\\test_file_%d", to_dir, i + 1);
        tasks[i].mode = DIRECT_IO_MEMORY_IMPACT;
        
        threads[i] = CreateThread(NULL, 0, copy_file_thread, &tasks[i], 0, NULL);

        if (threads[i] == NULL) {
            // Clean up resources
            for (int j = 0; j <= i; j++) {
                free(tasks[j].src_path);
                free(tasks[j].dst_path);
            }
            free(tasks);
            free(threads);
            wprintf(L"Failed to create thread %d\n", i);
            return 1;
        }

    }

    // Wait for all threads to complete
    WaitForMultipleObjects(num_files, threads, TRUE, INFINITE);

    // Collect results and clean up
    for (int i = 0; i < num_files; i++) {
        // Retrieve results from each task
        CopyTask *task;
        
        GetExitCodeThread(threads[i], (LPDWORD)&task);
        results[i].filename = _wcsdup(tasks[i].src_path);
        results[i].size_mib = tasks[i].size_mib;
        results[i].memory_duration = tasks[i].duration;
        results[i].memory_speed = tasks[i].speed;
        // Clean up
        CloseHandle(threads[i]);
        free(tasks[i].src_path);
        free(tasks[i].dst_path);

    }
    free(tasks);
    free(threads);

    // Run disk copy tests using direct_io mode
    wprintf(L"\nRunning disk copy tests...\n");
    
    // Create task and thread arrays
    CopyTask *disk_tasks = malloc(sizeof(CopyTask) * num_files);
    HANDLE *disk_threads = malloc(sizeof(HANDLE) * num_files);
    
    // Start all threads in parallel
    for (int i = 0; i < num_files; i++) {
        // Allocate path memory for each task
        disk_tasks[i].src_path = malloc(sizeof(wchar_t) * MAX_PATH);
        disk_tasks[i].dst_path = malloc(sizeof(wchar_t) * MAX_PATH);
        
        swprintf(disk_tasks[i].src_path, MAX_PATH, L"%s\\test_file_%d", from_dir, i + 1);
        swprintf(disk_tasks[i].dst_path, MAX_PATH, L"%s\\test_file_%d_disk", to_dir, i + 1);
        
        disk_tasks[i].mode = DIRECT_IO;
        
        disk_threads[i] = CreateThread(NULL, 0, copy_file_thread, &disk_tasks[i], 0, NULL);
        if (!disk_threads[i]) {
            wprintf(L"Failed to create disk copy thread %d\n", i);
            // Error handling...
        }
    }

    // Wait for all threads to complete
    WaitForMultipleObjects(num_files, disk_threads, TRUE, INFINITE);

    // Collect results and clean up
    for (int i = 0; i < num_files; i++) {
        results[i].disk_duration = disk_tasks[i].duration;
        results[i].disk_speed = disk_tasks[i].speed;
        
        // Clean up resources
        CloseHandle(disk_threads[i]);
        free(disk_tasks[i].src_path);
        free(disk_tasks[i].dst_path);
    }

    // Free array memory
    free(disk_tasks);
    free(disk_threads);

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
    wprintf(L"\nBenchmark Results:\n");
    wprintf(L"%-10s %-20s %-12s %-20s %-20s %-20s %-20s\n",
           L"Thread ID", L"Filename", L"Size (MiB)",
           L"Memory Copy (s)", L"Memory Speed (MiB/s)",
           L"Disk Copy (s)", L"Disk Speed (MiB/s)");
    wprintf(L"--------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < num_files; i++) {
        wprintf(L"%-10d %-20s %11.2f %19.2f %19.2f %19.2f %19.2f\n",
               i, results[i].filename, results[i].size_mib,
               results[i].memory_duration, results[i].memory_speed,
               results[i].disk_duration, results[i].disk_speed);
    }

    wprintf(L"\nTotal Statistics:\n");
    wprintf(L"Total Size: %.2f MiB\n", total_size);
    wprintf(L"Memory Copy - Total Duration: %.2f seconds, Total Speed: %.2f MiB/s\n",
           total_memory_duration, avg_memory_speed);
    wprintf(L"Disk Copy   - Total Duration: %.2f seconds, Total Speed: %.2f MiB/s\n",
           total_disk_duration, avg_disk_speed);

    double speed_ratio = avg_disk_speed / avg_memory_speed;
    if (speed_ratio >= 0.95) {
        wprintf(L"\033[41m\033[37mYou may hit the memory bandwidth wall\033[0m\n");
    }

    // Cleanup
    for (int i = 0; i < num_files; i++) {
        free(results[i].filename);
    }
    free(results);

    return 0;
}

// Handle generate test files mode
static int handle_generate_test_files(int argc, wchar_t *argv[]) {
    if (argc < 7) {
        wprintf(L"Missing parameters for generate_test_files mode\n");
        return 1;
    }
    
    uint64_t file_size = 0;
    int num_files = 0;
    wchar_t *output_dir = L".";  // Default to current directory
    
    // Parse arguments
    for (int i = 3; i < argc; i += 2) {
        if (wcscmp(argv[i], L"--size") == 0) {
            file_size = parse_size_with_unit(argv[i+1]);
        } else if (wcscmp(argv[i], L"--num") == 0) {
            num_files = (int)wcstol(argv[i+1], NULL, 10);
        } else if (wcscmp(argv[i], L"--dir") == 0) {
            output_dir = argv[i+1];
        }
    }
    
    if (file_size == 0 || num_files <= 0) {
        wprintf(L"Invalid size or number of files\n");
        return 1;
    }
    
    // Create and execute generation tasks
    CopyTask *tasks = malloc(sizeof(CopyTask) * num_files);
    HANDLE *threads = malloc(sizeof(HANDLE) * num_files);
    
    wprintf(L"Generating %d test files of size %lluB each in %s\n", 
           num_files, file_size, output_dir);
    
    for (int i = 0; i < num_files; i++) {
        tasks[i].src_path = malloc(sizeof(wchar_t) * (wcslen(output_dir) + 32));
        swprintf(tasks[i].src_path, wcslen(output_dir) + 32, L"%s\\test_file_%d", output_dir, i + 1);
        tasks[i].mode = GENERATE_TEST_FILES;
        tasks[i].test_file_size = file_size;
        
        threads[i] = CreateThread(
            NULL,                   // Default security attributes
            0,                      // Default stack size
            copy_file_thread,       // Thread function
            &tasks[i],              // Thread function argument
            0,                      // Default creation flags
            NULL                    // Do not receive thread ID
        );
        
        if (threads[i] == NULL) {
            wprintf(L"Failed to create thread %d\n", i);
            // Clean up already created threads and resources
            for (int j = 0; j < i; j++) {
                CloseHandle(threads[j]);
                free(tasks[j].src_path);
            }
            free(tasks);
            free(threads);
            return 1;
        }
    }
    
    // Wait for all threads to complete
    bool all_success = true;
    for (int i = 0; i < num_files; i++) {
        DWORD result;
        WaitForSingleObject(threads[i], INFINITE);
        GetExitCodeThread(threads[i], &result);
        if (result != 0) {
            all_success = false;
        }
        CloseHandle(threads[i]);
    }
    
    // Print results
    wprintf(L"\nGeneration Results:\n");
    wprintf(L"%-10s %-30s %-15s %-12s\n", 
           L"File #", L"Path", L"Size", L"Duration (s)");
    wprintf(L"------------------------------------------------------------\n");
    
    double total_duration = 0;
    for (int i = 0; i < num_files; i++) {
        wprintf(L"%-10d %-30s %-15llu %11.2f\n",
               i + 1, tasks[i].src_path, file_size, tasks[i].duration);
        total_duration = (tasks[i].duration > total_duration) ? 
                        tasks[i].duration : total_duration;
    }
    
    wprintf(L"\nTotal Statistics:\n");
    wprintf(L"Total Size: %.2f GiB\n", 
           (file_size * num_files) / (1024.0 * 1024.0 * 1024.0));
    wprintf(L"Total Duration: %.2f seconds\n", total_duration);
    wprintf(L"Average Speed: %.2f MiB/s\n", 
           (file_size * num_files) / (1024.0 * 1024.0) / total_duration);
    
    // Clean up resources
    for (int i = 0; i < num_files; i++) {
        free(tasks[i].src_path);
    }
    free(tasks);
    free(threads);
    fflush(stdout);

    
    return all_success ? 0 : 1;
}

// Parse copy mode from command line argument
static CopyMode parse_copy_mode(const wchar_t *mode_str) {
    if (wcscmp(mode_str, L"cp") == 0) return SYSTEM_CP;
    if (wcscmp(mode_str, L"mmap") == 0) return MMAP;
    if (wcscmp(mode_str, L"direct_io") == 0) return DIRECT_IO;
    if (wcscmp(mode_str, L"direct_io_memory_impact") == 0) return DIRECT_IO_MEMORY_IMPACT;
    if (wcscmp(mode_str, L"generate_test_files") == 0) return GENERATE_TEST_FILES;
    if (wcscmp(mode_str, L"benchmark") == 0) return BENCHMARK;
    return -1;
}

// Print copy results
static void print_copy_results(CopyTask *tasks, int num_files) {
    wprintf(L"\nDetailed Results:\n");
    wprintf(L"%-10s %-30s %-12s %-12s %-12s\n", 
           L"Thread ID", L"Filename", L"Size (MiB)", L"Duration (s)", L"Speed (MiB/s)");
    wprintf(L"--------------------------------------------------------------------------------\n");

    double total_size = 0, total_duration = 0;
    for (int i = 0; i < num_files; i++) {
        wprintf(L"%-10d %-30s %11.2f %11.2f %11.2f\n", 
               i, wcsrchr(tasks[i].src_path, L'\\') ? wcsrchr(tasks[i].src_path, L'\\') + 1 : tasks[i].src_path,
               tasks[i].size_mib, tasks[i].duration, tasks[i].speed);
        total_size += tasks[i].size_mib;
        total_duration = (tasks[i].duration > total_duration) ? 
                        tasks[i].duration : total_duration;
    }

    wprintf(L"\nTotal Statistics:\n");
    wprintf(L"Total Size: %.2f MiB\n", total_size);
    wprintf(L"Total Duration: %.2f seconds\n", total_duration);
    wprintf(L"Average Speed: %.2f MiB/s\n", total_size / total_duration);
    fflush(stdout);

}

// Handle file copy mode
static int handle_copy_files(int argc, wchar_t *argv[], CopyMode mode) {
    if (argc < 6) {
        wprintf(L"Invalid number of arguments for copy mode\n");
        return 1;
    }

    // Find --from and --to parameters
    wchar_t **src_files = NULL;
    wchar_t *dst_dir = NULL;
    int num_files = 0;
    int from_index = -1;
    int to_index = -1;

    // Locate the positions of --from and --to
    for (int i = 3; i < argc; i++) {
        if (wcscmp(argv[i], L"--from") == 0) {
            from_index = i;
        } else if (wcscmp(argv[i], L"--to") == 0) {
            to_index = i;
            break;
        }
    }

    // Validate parameters
    if (from_index == -1 || to_index == -1 || from_index >= to_index) {
        wprintf(L"Invalid parameters for copy mode\n");
        return 1;
    }

    // Calculate the number of files and set the source file array
    src_files = &argv[from_index + 1];
    num_files = to_index - (from_index + 1);
    dst_dir = argv[to_index + 1];

    if (num_files <= 0) {
        wprintf(L"No source files specified\n");
        return 1;
    }

    // Print debug information
    wprintf(L"Number of files to copy: %d\n", num_files);
    for (int i = 0; i < num_files; i++) {
        wprintf(L"File %d: %s\n", i + 1, src_files[i]);
    }

    // Create tasks and threads
    CopyTask *tasks = malloc(sizeof(CopyTask) * num_files);
    HANDLE *threads = malloc(sizeof(HANDLE) * num_files);

    // Start all copy threads
    for (int i = 0; i < num_files; i++) {
        tasks[i].src_path = src_files[i];
        tasks[i].dst_path = malloc(sizeof(wchar_t) * (wcslen(dst_dir) + wcslen(wcsrchr(src_files[i], L'\\') ? 
                           wcsrchr(src_files[i], L'\\') + 1 : src_files[i]) + 2));
        swprintf(tasks[i].dst_path, wcslen(dst_dir) + 32, L"%s\\%s", 
                dst_dir, 
                wcsrchr(src_files[i], L'\\') ? wcsrchr(src_files[i], L'\\') + 1 : src_files[i]);
        tasks[i].mode = mode;
        
        threads[i] = CreateThread(
            NULL,                   // Default security attributes
            0,                      // Default stack size
            copy_file_thread,       // Thread function
            &tasks[i],              // Thread function argument
            0,                      // Default creation flags
            NULL                    // Do not receive thread ID
        );

        if (threads[i] == NULL) {
            wprintf(L"Failed to create thread %d\n", i);
            // Clean up created threads and resources
            for (int j = 0; j < i; j++) {
                CloseHandle(threads[j]);
                free(tasks[j].dst_path);
            }
            free(tasks);
            free(threads);
            return 1;
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_files; i++) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }

    // Print results and clean up
    print_copy_results(tasks, num_files);

    for (int i = 0; i < num_files; i++) {
        free(tasks[i].dst_path);
    }
    free(tasks);
    free(threads);

    return 0;
}

// Update main function to include benchmark mode
int wmain(int argc, wchar_t *argv[]) {
    if (argc < 3) {
        wprintf(L"Usage:\n");
        wprintf(L"  Copy files:\n");
        wprintf(L"    %s --mode [cp|mmap|direct_io|direct_io_memory_impact] --from src --to dst\n", argv[0]);
        wprintf(L"  Generate test files:\n");
        wprintf(L"    %s --mode generate_test_files --size <size> --num <number> [--dir <output_dir>]\n", argv[0]);
        wprintf(L"  Benchmark:\n");
        wprintf(L"    %s --mode benchmark --size <size> --num <number> --from <source_dir> --to <dest_dir>\n", argv[0]);
        return 1;
    }

    // Handle generate test files mode
    if (wcscmp(argv[2], L"generate_test_files") == 0) {
        return handle_generate_test_files(argc, argv);
    }
    
    // Handle benchmark mode
    if (wcscmp(argv[2], L"benchmark") == 0) {
        return handle_benchmark(argc, argv);
    }
    
    // Handle copy mode
    CopyMode mode = parse_copy_mode(argv[2]);
    if (mode == -1) {
        wprintf(L"Invalid mode\n");
        return 1;
    }

    return handle_copy_files(argc, argv, mode);
}

