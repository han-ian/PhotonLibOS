/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <inttypes.h>
#include <photon/common/callback.h>

namespace photon {

// 位移宏定义，用于计算事件引擎标志
#define SHIFT(n) (1 << n)

// 事件引擎初始化标志，定义了支持的不同事件处理机制
const uint64_t INIT_EVENT_NONE = 0;                     // 不初始化任何事件引擎
const uint64_t INIT_EVENT_EPOLL = SHIFT(0);             // Linux epoll事件引擎
const uint64_t INIT_EVENT_IOURING = SHIFT(1);           // Linux io_uring事件引擎
const uint64_t INIT_EVENT_SELECT = SHIFT(2);            // 跨平台select事件引擎
const uint64_t INIT_EVENT_KQUEUE = SHIFT(3);            // macOS/FreeBSD kqueue事件引擎
const uint64_t INIT_EVENT_IOCP = SHIFT(4);              // Windows IOCP事件引擎
const uint64_t INIT_EVENT_EPOLL_NG = SHIFT(5);          // 新一代epoll事件引擎
const uint64_t INIT_EVENT_IOURING_SQPOLL = SHIFT(6);    // io_uring内核轮询模式，提交队列在内核中轮询
const uint64_t INIT_EVENT_IOURING_SQ_AFF = SHIFT(7);    // io_uring提交队列绑定CPU，提高缓存局部性
const uint64_t INIT_EVENT_IOURING_IOPOLL = SHIFT(8);    // io_uring轮询模式，直接轮询设备避免系统调用
const uint64_t INIT_EVENT_SIGNAL = SHIFT(10);           // 信号处理引擎，处理异步信号

// I/O引擎初始化标志，定义了支持的不同I/O处理机制
const uint64_t INIT_IO_NONE = 0;                        // 不初始化任何I/O引擎
const uint64_t INIT_IO_LIBAIO = SHIFT(0);               // Linux异步I/O库，提供高性能异步I/O操作
const uint64_t INIT_IO_LIBCURL = SHIFT(1);              // cURL网络库，用于HTTP/HTTPS请求
const uint64_t INIT_IO_SOCKET_EDGE_TRIGGER = SHIFT(2);  // Socket边缘触发模式，实现更高效的事件驱动网络
const uint64_t INIT_IO_EXPORTFS = SHIFT(10);            // 导出文件系统，用于将本地文件系统暴露给其他进程
const uint64_t INIT_IO_FSTACK_DPDK = SHIFT(20);         // DPDK网络框架，用于高性能网络处理

#if defined(__linux__)
// Linux平台默认事件和I/O引擎配置
// 优先使用io_uring和epoll，备选select和信号处理
const uint64_t INIT_EVENT_DEFAULT = INIT_EVENT_IOURING | INIT_EVENT_EPOLL |
                                    INIT_EVENT_SELECT  | INIT_EVENT_SIGNAL;
// Linux平台默认I/O引擎配置
const uint64_t INIT_IO_DEFAULT    = INIT_IO_LIBAIO     | INIT_IO_LIBCURL;
#else   // macOS, FreeBSD ...
// 非Linux平台（如macOS、FreeBSD）默认事件和I/O引擎配置
const uint64_t INIT_EVENT_DEFAULT = INIT_EVENT_KQUEUE | INIT_EVENT_SELECT |
                                    INIT_EVENT_SIGNAL;
const uint64_t INIT_IO_DEFAULT    = INIT_IO_LIBCURL;
#endif

#undef SHIFT

// Photon运行时配置选项结构体
struct PhotonOptions {
    int libaio_queue_depth = 32;                        // libaio队列深度，控制异步I/O操作的并发数
    uint32_t iouring_sq_thread_cpu;                     // io_uring提交队列线程绑定的CPU，优化缓存局部性
    uint32_t iouring_sq_thread_idle_ms = 1000;          // io_uring提交队列线程空闲时间（毫秒），控制资源使用
    bool use_pooled_stack_allocator = false;            // 是否使用池化栈分配器，减少栈分配开销
    bool bypass_threadpool = false;                     // 是否绕过线程池，直接使用Photon线程
};

/**
 * @brief 初始化主Photon线程和辅助线程
 * 
 * 此函数初始化Photon协程库的核心组件，包括事件引擎和I/O引擎
 * 辅助线程将在后台持续运行，处理底层事件和I/O操作
 * 
 * @param event_engine 事件引擎标志，指定要初始化的事件处理机制
 * @param io_engine I/O引擎标志，指定要初始化的I/O处理机制
 * @param options Photon运行时配置选项
 * @return 0表示成功，-1表示失败
 * 
 * 设计说明：
 * - 按推荐顺序初始化事件引擎，确保可用性
 * - 保存已初始化的引擎类型，供清理时使用
 * - 注册fork重置句柄，确保子进程状态正确
 */
int init(uint64_t event_engine = INIT_EVENT_DEFAULT,
         uint64_t io_engine = INIT_IO_DEFAULT,
         const PhotonOptions& options = {});

/**
 * @brief 销毁/连接辅助线程，并结束主协程
 * 
 * 此函数负责清理Photon协程库的所有资源，包括：
 * - 销毁所有辅助线程
 * - 清理事件引擎
 * - 清理I/O引擎
 * - 释放相关资源
 * 
 * @return 0表示成功，-1表示失败
 * 
 * 设计说明：
 * - 执行所有注册的清理钩子，确保资源正确释放
 * - 按初始化的反向顺序清理，避免依赖关系问题
 * - 重置引擎标志，防止重复清理
 */
int fini();

/**
 * @brief 获取当前线程的Photon线程ID
 * 
 * @return 当前线程的Photon线程ID，若未初始化则返回-1
 */
int get_vcpu_id();

/**
 * @brief 获取虚拟CPU数量
 * 
 * @return 已初始化的虚拟CPU数量
 */
int get_vcpu_num();

/**
 * @brief 检查当前是否在Photon协程中
 * 
 * @return true表示当前在Photon协程中，false表示不在
 * 
 * 设计说明：
 * - 用于区分原生线程和Photon协程上下文
 * - 避免在原生线程中误用Photon特定API
 */
bool is_photon_runtime();

/**
 * @brief 添加清理钩子
 * 
 * 此函数将指定的回调函数添加到清理钩子列表中
 * 在调用fini()时，这些钩子将被依次执行
 * 
 * @param handler 要添加的回调函数
 */
void fini_hook(Delegate<void> handler);

}