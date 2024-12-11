# KCORES Copy Performance Tester (KCPS)

一个用于测试和比较不同文件复制方法性能的多线程工具。该工具支持多种复制模式，包括系统CP命令、内存映射(mmap)、直接I/O以及测试内存带宽是否限制了拷贝速度。

创建该工具旨在揭示在现代 NVMe 存储设备上，传统的拷贝方式已经达到了性能瓶颈，我们需要多线程工具才能达到理论最大带宽。

同时，现代 NVMe 设备的带宽已经相当巨大，在多个 NVMe 组成的"全闪"阵列的情况，其最大读写带宽甚至可能超过了内存带宽，所以该工具也旨在揭示在全闪阵列上，内存带宽是否限制了拷贝速度。


## 功能特点

- 多线程并行文件复制
- 支持多种复制模式：
  - 系统CP命令 (`cp`)
  - 内存映射 (`mmap`)
  - 直接I/O (`direct_io`)
  - 测试内存最大带宽是否会限制拷贝速度 (`direct_io_memory_impact`)
- 详细的性能统计报告
- 支持批量文件复制

## 编译要求

- linux 版本
  - Linux操作系统
  - GCC编译器
  - pthread库

## 编译方法

```bash
make linux
```

## 使用方法

```bash
./parallel_copy --mode [cp|mmap|direct_io|direct_io_memory_impact] --from file1 [file2 ...] --to dest_dir
```

### 参数说明

- `--mode`: 指定复制模式
  - `cp`: 使用系统CP命令
  - `mmap`: 使用内存映射
  - `direct_io`: 使用直接I/O
  - `direct_io_memory_impact`: 使用直接I/O模式测试内存带宽对拷贝速度的影响, 这个模式下只测试最大内存带宽, 并不真实复制文件
- `--from`: 指定源文件（支持多个文件）
- `--to`: 指定目标目录

### 使用示例

```bash
# 使用cp模式复制多个文件
./parallel_copy --mode cp --from file1.dat file2.dat file3.dat --to /destination/path

# 使用mmap模式复制多个文件
./parallel_copy --mode mmap --from file1.dat file2.dat file3.dat --to /destination/path

# 使用直接I/O模式复制文件
./parallel_copy --mode direct_io --from file1.dat file2.dat file3.dat --to /destination/path
```

## 输出示例

程序会输出详细的复制性能统计信息：

```
Detailed Results:
Thread ID Filename                       Size (MiB)  Duration (s) Speed (MiB/s)
--------------------------------------------------------------------------------
0         file1.dat                         100.00        1.20       83.33
1         file2.dat                         200.00        2.40       83.33

Total Statistics:
Total Size: 300.00 MiB
Total Duration: 2.40 seconds
Average Speed: 125.00 MiB/s
```

## 技术细节

- 使用POSIX线程实现并行复制
- 支持大文件处理（块大小可配置）
- 针对不同存储设备优化的复制策略
- 内存对齐和直接I/O支持

## 注意事项

- 直接I/O模式需要root权限或适当的文件系统权限
- 目标目录必须具有写入权限
- 建议在测试大文件时使用直接I/O模式
- 内存映射模式可能受系统内存限制影响

## 许可证

[KCORES Lincese v1.0](LICENSE_zh-CN)

## 贡献

欢迎提交Issue和Pull Request来帮助改进这个项目。

