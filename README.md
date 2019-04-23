# TouchFishDB KVStore 报告

> 计76 刘晓义 2017011426

`摸鱼 DB` 是一个摸鱼的 KV 数据存储。能够确保原子性以及 Crash consistency, 使用了 Write-ahead logging。

Repo 中共有三个 Branch:
- master: 提交的版本，包括 Range query
- no\_journaling: 禁用 Journal 的版本
- with\_free\_space: 复用失效数据空间的版本

## 设计
包含三部份：
- Journal, 处理 Write-ahead logging
  - 一个 Queue，每次写入的时候同时 flush 进硬盘
- Store, 处理 Value 在硬盘上的存储和读取
  - 长得和 Example 里那个很像，不断在一系列文件中的最后一个文件末尾进行添加
- Index, 存储 Key 到 Value 位置的映射。
  - 为了支持 Range 操作：一个 RB-Tree，写入的时候同时 flush 进硬盘

有一个 Journal 的原因是 Index 写入散的比较开，不容易用到操作系统的缓存。

数据库启动后会在后台启动一个线程(Sync)，不断将 Journal 里的内容刷进 Index 里。

所以写入的过程是：
- 把 Value 写进 Store 里，得到了写进去的位置。
- 把位置写进 Journal 里
  - 如果 Journal 还很空闲，写入成功了，那就直接返回
  - 如果 Journal 快满了，但是写进去了，把后台的 Sync 线程叫起来
  - 如果 Journal 已经满了，把后台的 Sync 线程叫起来，然后等他完成工作在重试

读取的过程是：
- 扫一边 Journal。因为 Journal 不是很大而且在内存里，所以直接做扫描
- Journal 里没找到的话，从 Index 找。Index 比较大，但是其实是在 mmap 内存里的 RB-Tree，所以这个也很快
- 如果还没找到，返回没找到。如果找到了，从 Store 里读出来 Value

### 修改过的设计

#### 空闲硬盘空间重用
尝试过在 Store 内维护硬盘上的空闲区，原因是可以在读取的时候用得上操作系统提供的 Cache，文件越少命中的概率越高，结果性能反而下降。

分析了一下，猜测是因为之前原来就没有命中率低的问题。如果按照 Value 每次都写在最后一个文件的末尾，那么不断更新的过程中，靠前面的文件就会自然的退出 Cache，活在 Cache 里的都是靠后的文件块，本身 Alive record 比较多。所以上面那个是解决了一个不存在的问题。

同时因为维护这个信息需要一个有序集(std::set)，常数不能说小，所以有一定耗时。启动的时候也要扫一边 Index 才能够得到。

所以这个设计被删去了。删去之后唯一存在的问题是硬盘上占据的空间不会下降会越来越高。但是在 Bench 和 Test 的时候没有注意到删去前后占据总空间有明显的区别，读了一下源码感觉是因为 Bench(isSkew = 0) 和 Test 都是随机生成 Key，而且每次覆写之后 Value 都是越来越大。我没有实现相邻 Free block 的合并，所以根本写不进可以复用的空间里...

#### 合并多次 Store 写入
这是最开始的设计， 为了做到这一点，需要把 Journal 放到 Store 之前，也就是 Journal 里面记录的是 Key & Value，而不是现在的 Key & Location。之后在 Sync 的时候批量写入。后来改成了现在的，先写 Store，再写 Journal

这个问题在于，其实没有什么本质区别... 每次 Write 操作无论如何都需要把 Key 和 Value 给写到硬盘上，不论是在 Journal 里还是 Store 里，所以先写到 Journal 里不会对硬盘延迟有什么巨大的影响，区别只是这个是存进同一个文件还是两个不同的文件里了...而写入不连续的延迟可以被文件系统的 Journaling 解决，结果就是 Write 操作的阻塞不会有显著区别。而如果把 Value 写进了 Journal 里，到时候 Sync 的时候还要再写一次，这就造成多消耗了一些时间。

还有一个问题是 Value 的可能大小高达 4MB，而在 Journal 设计里需要快速存取，最好能够在一个操作系统的 Cache 单元里，这样一次 Flush 操作会比较块，Value 太大这一点和 Journal 的设计是冲突的。而且 Journal 在硬盘上的文件也要是循环 Deque 形式，所以每个单元的空间都要留 4MB，这样 Journal 文件就会特别大。

### 实现具体细节
依赖了 Boost 和 C++17 的一些功能，特别地，std::filesystem，因此编译的时候需要安装 Boost，以及:
```bash
g++ -std=c++17 ... -lpthread -lstdc++fs
```

实现的时候有两个比较有趣的问题，下面具体讨论

#### 内存数据结构-硬盘同步
##### Journal
Journal 用的 std::deque，底层就是个线性表，选择在硬盘上建立一个环形 Queue，因此写入的时候就直接根据偏移量 Seek 过去然后一通写就好了。每个 Journal 项(JournalEntry) 不大，主要包含一个 IndexValue(Key & KeyLength) 和一个 IndexValue(File & Offset & ValueLength)。根据我设置的最大 Key 大小(2KB)这个大小大概在 2KB 多一点，一个 fwrite + fflush 就进去了。

读取的时候都在内存里，不涉及硬盘操作。

麻烦事在于 Crash recovery。如果每次完成 Sync 之后**同步地**要把硬盘文件清空或者标记成 Complete，那就有点慢。然后我又懒得写异步，因为如果 Queue 里的东西拿出来了，然后 Sync 一半系统挂了，那麻烦的事情更多，比如 Journal 要是真的满了，那我应该是让 Write 线程等待呢还是怎么样，然后开始之前涉及 Queue 的一次复制，完成之后应该只标记真的 Flush 进去的，这个对于我不足的智力实在是太大的负担。

所以偷了个懒：不标记完成，每次启动之后无论如何把 Journal 文件里的所有东西都重写一边。反正是 KV 数据库，重复最近的写操作不会产生错误行为。问题在于如何找到正确的顺序，例子:

- A
- B
- C
- D

这个硬盘上的序列可能是四种不同的写入导致的： `ABCD` `BCDA` `CDAB` `DABC`。这是可能造成问题的：

- Write 1 1
- Write 1 2

类似上面这样的输入，从哪里把环断开产生的最终状态有区别。

为了区分这一点，在每一个 Journal 项上添加一个 Ident，如果 Journal 的最大大小是 J，那么令 Ident 在 **模 J+1** 群上自增。之后读取的时候只需要找到 Ident 组成的序列在哪里有**间断**就可以了。举例：

- 0 A
- 1 B
- 3 C
- 4 D

B-C 之间有一个间断，因此写入序列一定是 `CDAB`。`0123` 这样的序列可以认为存在 `3[4]0` 这样一个间断。

这样处理之后，每次写入总共只涉及两次完整硬盘写入(flush store, flush journal)和一次写入的一部份(sync index)

##### Store
写入的时候直接打开最后一个文件，然后写。如果超过最大文件大小，就新建下一个文件。读取也是直接打开就读。

考虑过打开文件后不立即关闭，在多个请求之内重复使用，这样会用到 libc 的缓存。但是 `FILE*` 当然不是线程安全的，也没法复制，所以并发读取如果高了会有 Overhead。如果每个请求都开一个新文件，由于都是在一个 Process 里面，其实也可以用到操作系统的文件缓存，所以不会有什么明显的区别。

##### Index
Index 比较难办，需要在硬盘上存储树形结构。如果是把堆上的数据结构手动 Serialize，是需要比较多的时间的，要不然全部 Serialize 一下写进去，要不然要有非常精妙的算法做 diff。

最后摸鱼了，直接 mmap，每次写完了 sync 一下。问题在于 std::map 里面全都是指针，重启或者扩大 map 文件大小的时候，一定要维持空间位置不变。还要自己写 Allocator，还要维护剩余空闲空间大小，总之很麻烦。

最后用了 Boost: `boost::interprocess::managed_mapped_file` 和 `boost::interprocess::map`。Boost.Interprocess 这个模块最开始是设计成 IPC 用的，但是如果每次写入之后手动 flush 一下，可以持久化到硬盘上。由于是给 IPC 设计的，所以这个容器内没怎么用指针，正好适合做持久化这件事。用了这个之后节约了 Serialize/Deserlaize 的时间，以及自己写 Allocator 的麻烦事请。

这样带来的一个额外的好处是就算 Index 太大了，也可以通过把没有写入的部分自动 unmap 掉来节省内存。

可能存在的问题是会在文件系统里留下来一个巨大的文件。但就目前来开，还是 Store 占得空间比较多。

#### 锁
普遍而言的规律是，写是 RwLock.write，读取是 RwLock.read，读取之间不互斥，写和其他所有操作互斥。

##### Store
Read 和 Write 涉及 Store

按照上面对 Store 的说法， Read 是不需要锁的，因为数据永远不会被覆写。

可以改进的一点是：在 append 模式下打开的文件也不需要处理锁。但是这样需要处理新建文件的时候要锁住，和其他写入互斥，涉及一个 shared_lock -> unqiue_lock 的转换，这个写不好会死锁，就懒得动脑筋了，直接所有的写都互斥。

##### Journal
三种操作都会涉及 Journal: read, write, sync

因此 Journal 自己有一个 Mutex，read 会不互斥地锁在上面，sync 和 read 会互斥地锁上去。同时，sync 和 read 之间的调度通过一个 condition variable 实现。一共有两个 Condvar:
```
- notify_sync
- notify_writers
```

sync 循环做的事情是：
- 把自己持有的锁挂在 `notify_sync` 上面等着，有一个 Timeout，这个过程中锁会放开
- 醒过来以后执行 Sync 操作
- 通知 `notify_writers`

写操作做的事情是：
- 锁住 Journal
- 检查 Journal 是不是快满了，是的话，通知 notify_sync，这样当锁空闲的时候 Sync 循环可以执行 Sync
- 检查 Journal 是不是真的满了，是的话，把自己的锁挂在 `notify_writers` 上面，有一个更长一点的 Timeout，这个过程中锁会放开。活过来之后重新检查 Journal 的容量
- 把 JournalEntry 写进 Journal 里

这样最后的结果是：
- Sync 操作每隔一段时间会执行一次
- Journal 快满的时候 Sync 会尽快执行
- Journal 真的满了的话，写操作会等 Sync 完成

同时，所有的 Sync 和 Write 都和 Read 互斥，Read 之间不互斥。Write 和 Read 互斥的原因是线性扫描的时候如果产生了修改，STL 会产生 UB。

两个 Condvar 调度的是同一个 Mutex，所以不会有奇怪的 Race。

##### Index
Read 和 Sync 会涉及 Index。这个好像没有什么特别好的优化办法，直接加了一个锁在 Index 操作周围。

之前是在整个 Read 外面加的的这个锁，把 Journal 的读取包进去了，因此这个 Mutex 在代码中叫做 `read_lock`

这样可能会和 Sync 死锁:

- Sync 从 Condvar 醒来(notify or timeout)，持有 Journal 锁
- Read 执行，持有 Read 锁
- Read 尝试从 Journal 读取数据，被阻塞
- Sync 从 Journal 得到数据，尝试写入 Index，被阻塞

最后把 Read 对 Journal 的读取扔到了锁外面。这样不会产生 Race，因为就算 Read 可能得到正在被覆盖的数据，但是 Read 开始的时候那个写操作一定还**没有返回**，因此是满足 Linearizability 的。

## 性能测试
修改 Journal 的最大大小(JOURNAL\_SIZE)，0 表示不使用 Journal，每次写直接同步到 Index 上。不使用 Journal 时修改了锁的位置：在写入的时候直接读写 Index，并且有可能对 Index 大小作出调整，因此在 Write 外面互斥地锁住了 read_mut。同时 Store 锁可以被删去。

当 Journal 内没有被同步的项多于 JOURNAL\_SIZE - JOURNAL\_BACKOFF 时启动 Sync。在以下测试中，有:

```
JOURNAL_BACKOFF === JOURNAL_SIZE / 4
```

测试使用的是 Aliyun 的机子，硬盘是默认的`高效云盘`。配置了 6G Swap，空余内存大约 1G

下表为测试结果， 表头为 bench 运行参数，内容是 Operation per sec。因此第一列为 0% 读取，100% 写入，第二、第三列 50% 读取、写入，第四列 99% 读取， 1% 写入。

| JOURNAL\_SIZE |  bench 8 0 0 | bench 8 50 0 | bench 8 50 1 |   bench 8 99 0 | 缩小 ValLen = 8, bench 8 50 1 |
|--------------:|-------------:|-------------:|-------------:|---------------:|------------------------------:|
|             0 | 10046.416526 | 27644.768566 | 47440.234506 | 1420863.511701 |                  77029.579694 |
|            16 |  9303.881412 | 25746.350726 | 48545.575680 | 1266446.758319 |                               |
|           128 |  9266.552161 | 28145.743756 | 48900.461161 | 1286387.326780 |                 103829.923869 |
|          1024 | 11970.534012 | 27366.194299 | 48600.343791 | 1260057.552971 |                               |

读取由于都是在 mmap 的内存空间中做的，都很快，而且 Journal 大小对其不产生影响。

而当写入比较密集的时候，Journal 的大小越大，对大量随机读写数据的处理能力越好。

关注到 JOURNAL\_SIZE = 128 相比于 16，在 50% 写的时候要更优，而在 100% 写的时候表现基本相同。同时 Journal 大小 1024 时 50% 略差于 128，但是 100% 写要好于 128

推测是由于在 100% 写的时候，128 长度的 Journal 也会被频繁占满，而 1024 长度被占满的时间少一些。所以当写入并发高的时候，Journal 越长效果越好。但是写入并发不足的时候，Journal 太长对操作系统的缓存不友好，因此更长的 Journal 表现会略差。

对于分布不均匀的数据，推测由于在 Index 内覆写比新建多，修改比较密集，因此在 Index 向硬盘上写的时候更快一些，所以其实三个测试中 Journal 占满的时间都不多，结果是性能差不多一致，而且都比随机写入的要更快。

和关闭 Journal 之后的性能对比也可以证明这一点：当 Journal 足够长的时候，性能优于不用 Journal 的情况。为了更显著地证明这一点，我们缩小了 Value 的长度，让 Value 写入基本不消耗时间。结果是带有 Journal 远好于禁用 Journal。

### 关于 WSL 上的性能
大概是 WSL 的 mmap 写崩了...扩大 Index 大小的时候会有 3s 左右的延迟。推测可能是 Windows 内核对 munmap 调用没有很好的优化，所以在 mmap 的时候很快，但是 munmap 的时候非常慢。

### 关于 Journal 的讨论
从以上测试中得到的结果，我们认为 Journal 够大时可以优化写入性能，同时 Journal 太大则会让性能略有下降。相反地，Journal 不够大可能会降低写入性能。因此如果再做优化，可以着眼于根据 Load 动态调整 Journal 长度。

更进一步地，由于不论是否启动 Journal，永远只有一个单独的线程正在对 Index 进行互斥的写入操作。因此当 Journal 不够大时，造成的 Overhead 主要来自于线程之间的调度，也就是 Condvar 的唤醒操作。从这个推测以及测试结果中 `bench 8 0 0` 一列的结果，我们可以认为：
- Journal 不够大时，造成的 Overhead 和 Journal 本身的大小无关，和处理器、操作系统的调度有关
- **可能**可以在 Journal 不够大时使用 Spinlock 代替 Mutex + Condvar 减小这个 Overhead

以上结论可能存在以下一个缺陷：

在测试过程中，我们注意到进程的 M_RES 内存在 700MB - 1200MB 之间浮动，而 M_VIRT 不断上升，说明 Swap 可能对测试的效果产生了不可忽略的影响。尤其是在禁用 Journal 之后，需要对 mmap 区域地 sync，可能是 Swap 带来的一次额外的读硬盘造成了延迟。理想而言作为对比，应该在有大于 4G 空闲内存的计算机上再进行一次测试，但是我没有内存足够大的，而且是在运行真实 Linux 内核的计算机，因此这个测试无法进行。

## 思考题

如何验证/测试 KV 引擎的 Crash consistency?

直接 SIGKILL 是不太行的，操作系统会做一些善后工作，比如把 mmap 空间里的东西写进去。

一个可能的比较简单的方法是 FUSE 写个文件系统，把每个状态保留下来，然后再尝试复现之前的状态。可以通过随机采样，或者也可以造一些容易出问题的情况，比如多次并发写入之后立即进行一次采样。这样可以不断电模拟只有硬盘上的数据保留的情况。

如果担心 FUSE 的实现和真实硬盘的实现不完全一样，可以抓 SYSCALL，把所有的 write 留下来，可以在其他文件系统上达到类似的效果。
