# pt_flame

pt\_flame 是基于 Intel PT 的程序性能分析工具，使用从 Intel PT 上采集到的程序执行踪迹生成挂钟时间火焰图。

## 安装

从源码编译，需要 C++17 支持，可使用 devtoolset-7

```bash
$ mkdir build; cd build
$ cmake -DCMAKE_INSTALL_PREFIX=<prefix> -DCMAKE_BUILD_TYPE=RelWidhtDebInfo ../
$ cmake --build .
$ cmake --install .
```

## 使用

### 基本使用

一键自动采集数据并输出火焰图

```bash
pt_drawflame.sh --record "-p <pid> -- sleep 0.5" -j <parallel>
```

从已有 perf.data 产生火焰图

```bash
pt_drawflame.sh -j <parallel>
```

从已有的 perf script 输出（script_out__[0-9]{5}）产生火焰图

```bash
pt_drawflame.sh -j <parallel> --skip
```

DIY

```bash
# 采集数据
perf record -m,32M -e intel_pt/cyc/u -C 0-3
# 生成 trace
perf script --itrace=b --ns -F-event,-period,+addr,-comm,+flags,-dso > script_out
# 生成火焰图
pt_flame -j <parallel> script_out | flamegraph.pl > flame.svg
```

### pt\_flame 工具

#### 火焰图

使用 PT 数据产生的指令踪迹产生挂钟时间火焰图。PT 数据需要在 cyc 模式下采集，即 `-e intel_pt/cyc/`，支持用户态采集的数据。内核态采集仅在 5.10 内核上有限测试。指令踪迹需要使用 perf 按照下面的格式生成：

```bash
 --itrace=b --ns -F-event,-period,+addr,-comm,+flags,-dso
```

可以指定多个 trace，每个文件内部需要按时间排序（perf script 可以保证），文件之间不需要有序。trace 可以使用两种方式指定：
- 不指定顺序，直接把文件追加在命令结尾。适合少量大文件。处理时会对所有文件内的指令按时间戳归并。

```bash
$ pt_flame trace1 trace2 ... 
```

- 指定顺序，适合支持并行 perf 输出多个文件

```bash
$ pt_flame -c 0 -t trace00,trace01,trace02 -c 1 -t trace10,trace11 ...
```

表示：trace0* 从 CPU 0 上采集，并且按照 trace00 - trace01 - trace02 顺序排列，每个文件首尾相接；trace1* 从 CPU 1 上采集，并且按照 trace10 - trace11 顺序排列，每个文件首尾相接。同一个 CPU 上的 trace 按照指定的顺序处理，不同 CPU 上的 trace 按时间戳归并。

    FlameGraph Options:
    -c <num> specifies CPU number for following traces, required before -t
    -t <trace[,trace[...]]> trace files for a cpu, sequentially
        this is designed to process multiple trace files from the same CPU
        traces specified at end of command without -t is CPU-less and have no
        ordering imposed. This is designed for small number of large traces.
        do NOT mix -t trace and CPU-less trace
    -l <num> limit number of instructions to replay, defaults no limit
    -j <num> parallel worker to parse traces, this number is NOT a hard limit,
        default 0, which turns off parallel. do NOT turn on parallel in production
        if num > 0:
          for EACH cpu (-c), spawn AT LEAST one worker to parse all traces
          if only CPU-less trace is provided, spawn at least one worker to
          parse EACH trace
    -s <num> split trace files every num lines to replay, default 10000

#### Perfetto

指定 `-P <name>` ，可在回放的同时生成 [Fuchsia Trace Format](https://fuchsia.dev/fuchsia-src/reference/tracing/trace-format) 格式文件，配合 [Perfetto](https://ui.perfetto.dev/) 可视化程序执行历史

#### 指定时间打栈

```bash
pt_flame -S stack -O
```

在回放过程中输出一次某一时间点每个程序的栈

    Print Stack Options:
    -S <prefix> print stacks to files named prefix_<seq#>, OVERWRITE
        existing files. do NOT print if not set
    -W <t> warmup, start printing after t ns, default 2000000 ns
    -I <t> interval, print every t ns after warmup, default 1000000 ns
    -C <num> print num number of stack, default 1
    -E <name> print one stack to file named name at the end of replay
    -O output stack only

    Perfetto Options:
    -P <name> output ftf (fuschia trace format) for use with Perfetto
       don't output if not set

### pt\_dlfilter.so

perf script 在生成 sample 时提供 dlfilter API，可以通过自定义的 dlfilter 对 sample 过滤和处理。alikernel 5.10 的系统 perf 和并发 perf 支持 dlfilter 功能，可以在 4.19 内核系统上使用新版本 perf。

```bash
perf script --dlfilter pt_dlfilter.so
```

目前仅过滤不跨函数的 branch 指令，建议配合 --itrace=b 使用。

### pt\_drawflame.sh 脚本

一键采集 - 生成 trace - 生成火焰图。
- `-r "options"` 指定 perf record 参数, 按照这个方式调用 perf

```bash
perf record -e intel_pt/cyc/u -o <output> $options
```

所有选项使用一个引号包围，调用时会被展开追加在命令结尾，例如`-r " -- <process>"`会被展开为 `perf record -e intel_pt/cyc/u -o <output> -- <process>`。不指定 `-r` 则使用已有的 perf.data。

- 如果已经有 perf script 的输出，指定 `--skip` 直接调用 pt_flame 产生火焰图
- `-j` 并行需要 pt\_func\_perf 提供的并行 perf

```bash
Usage: pt_drawflame.sh [options]
  Make sure existing perf script output is removed or they will be reused or overwritten
  -o/--output <file>  output file name, will append .svg to flamegraph,
                      default [flame]
  -l/--log <file>     log file, default [stderr]
  -j <num>            use parallel, default [0]
  -r/--record <str>   perf record options, perf will be invoked like this:
                      "perf record -e intel_pt/cyc/u $string"
                      multiple options will be expanded, e.g.
                      -r "-p <pid> -- sleep 0.1" expands to
                      perf record -e intel_pt/cyc/u -p <pid> -- sleep 0.1
  -d/--data <file>    perf record output, default [perf.data]
                      or use existing perf data if -r not provided
  -s/--skip           skip perf script and perf record, use existing data
  -p/--perf <bin>     perf binary, defaults to [/home/sunjingyuan.sjy/.local/bin/perf] if -j 0,
                      [/usr/share/pt_func_perf/perf] if -j > 0
  -t/--pt_flame <bin> pt_flame binary, use bundled by default
  --dlfilter          use bundled pt_dlfilter.so with perf script
  --dry-run           preview perf commands only
  -h/--help           print this message
```

### pt_pstack.sh 脚本

自动采集 pt 数据、处理并且产生栈

```bash
Usage: pt_pstack.sh [options]
  -p/--pid collect PT data from pid, use existing data if unset
  -d/--data perf data name, default [perf.data]
  -o/--output output file name, will append .svg to flamegraph, default [flame]
  -t/--pt stack replay binary, required
  -c/--count number of stack to print, default [1]
  -w/--warmup time before printing first stack, default [2000000] ns
     increase warmup time if too few stack is printed
  -i/--interval time between each stack, default [1000000] ns
  -l/--log log file, default [stderr]
```
