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

#include <photon/photon.h>
#include <inttypes.h>

#include "io/fd-events.h"
#include "io/signal.h"
#include "io/aio-wrapper.h"
#include "thread/thread.h"
#include "thread/thread-pool.h"
#include "thread/stack-allocator.h"
#ifdef ENABLE_FSTACK_DPDK
#include "io/fstack-dpdk.h"
#endif
#include "io/reset_handle.h"
#ifdef ENABLE_CURL
#include "net/curl.h"
#endif
#include "net/socket.h"
#include "fs/exportfs.h"
#include "common/alog.h"
#include "common/callback.h"
#include <vector>

namespace photon {

using namespace fs;
using namespace net;

// 全局标志，记录重置句柄是否已注册
static bool reset_handle_registed = false;
// 线程局部变量，记录当前使用的事件引擎和I/O引擎
static thread_local uint64_t g_event_engine = 0, g_io_engine = 0;

// 宏定义：根据引擎类型初始化I/O子系统
// 如果指定的I/O引擎被启用，则调用相应的初始化函数
#define INIT_IO(name, prefix, ...) if (INIT_IO_##name & io_engine)   { if (prefix##_init(__VA_ARGS__) < 0) return -1; }
// 宏定义：根据引擎类型清理I/O子系统
// 如果指定的I/O引擎曾被启用，则调用相应的清理函数
#define FINI_IO(name, prefix)      if (INIT_IO_##name & g_io_engine) {     prefix##_fini(); }

class Shift {
public:
    uint8_t _n;
    // 将输入的标志转换为对应的位移值
    constexpr Shift(uint64_t x) : _n(__builtin_ctz(x)) { }
    operator uint64_t() { return 1UL << _n; }
};

// 尝试按推荐顺序初始化主事件引擎
// Linux平台推荐顺序：epoll -> io_uring -> epoll-ng -> select
// 非Linux平台推荐顺序：kqueue -> select
static const Shift recommended_order[] = {
#if defined(__linux__)
    INIT_EVENT_EPOLL, INIT_EVENT_IOURING, INIT_EVENT_EPOLL_NG, INIT_EVENT_SELECT};
#else   // macOS, FreeBSD ...
    INIT_EVENT_KQUEUE, INIT_EVENT_SELECT};
#endif

// 根据标志和选项创建io_uring参数结构
inline iouring_args mkargs(uint64_t flags, const PhotonOptions& opt) {
    return {
    .is_master          = true,                         // 标记为主事件引擎
    .setup_sqpoll       = bool(flags & INIT_EVENT_IOURING_SQPOLL),  // 启用内核轮询
    .setup_sq_aff       = bool(flags & INIT_EVENT_IOURING_SQ_AFF),  // 绑定提交队列到CPU
    .setup_iopoll       = bool(flags & INIT_EVENT_IOURING_IOPOLL),  // 启用I/O轮询模式
    .sq_thread_cpu      = opt.iouring_sq_thread_cpu,    // 提交队列线程绑定的CPU
    .sq_thread_idle_ms  = opt.iouring_sq_thread_idle_ms, // 提交队列线程空闲时间
};   }

/**
 * @brief 初始化指定的事件引擎
 * 
 * 此函数根据事件引擎类型创建相应的主事件引擎实例
 * 在支持io_uring的平台上，对io_uring引擎使用专门的初始化方法
 * 
 * @param engine 要初始化的事件引擎类型
 * @param flags 事件引擎标志
 * @param opt Photon运行时选项
 * @return 0表示成功，-1表示失败
 */
static int init_event_engine(uint64_t engine, uint64_t flags, const PhotonOptions& opt) {
#ifdef PHOTON_URING
    auto mee = (engine != INIT_EVENT_IOURING) ?
        new_master_event_engine(engine) :  // 非io_uring引擎使用通用创建方法
        new_iouring_master_engine(mkargs(flags, opt));  // io_uring引擎使用专用创建方法
#else
    auto mee = new_master_event_engine(engine);
#endif
    // 使用创建的主事件引擎初始化fd_events模块
    return fd_events_init(mee);
}

/**
 * @brief Photon内部初始化函数
 * 
 * 此函数是Photon库的核心初始化函数，负责：
 * 1. 配置栈分配器和线程池选项
 * 2. 初始化虚拟CPU (vCPU)
 * 3. 按推荐顺序初始化事件引擎
 * 4. 初始化信号处理
 * 5. 初始化各种I/O引擎
 * 6. 注册进程fork时的重置句柄
 * 
 * @param event_engine 事件引擎标志
 * @param io_engine I/O引擎标志
 * @param options Photon运行时配置选项
 * @return 0表示成功，-1表示失败
 */
int __photon_init(uint64_t event_engine, uint64_t io_engine, const PhotonOptions& options) {
    // 根据选项配置栈分配器
    if (options.use_pooled_stack_allocator) {
        use_pooled_stack_allocator();
    }
    // 根据选项配置是否绕过线程池
    if (options.bypass_threadpool) {
        set_bypass_threadpool(true);
    }

    // 初始化虚拟CPU，这是Photon运行的基础
    if (vcpu_init() < 0)
        return -1;

    // 定义所有可能的事件引擎类型
    const uint64_t ALL_ENGINES =
            INIT_EVENT_EPOLL   | INIT_EVENT_EPOLL_NG |
            INIT_EVENT_IOURING | INIT_EVENT_KQUEUE |
            INIT_EVENT_SELECT  | INIT_EVENT_IOCP;
    // 如果需要初始化任何事件引擎
    if (event_engine & ALL_ENGINES) {
        // 按推荐顺序尝试初始化事件引擎
        for (auto x : recommended_order) {
            // 如果当前引擎被启用且初始化成功，则跳转到下一步
            if ((x & event_engine) && init_event_engine(x, event_engine, options) == 0) {
                goto next;
            }
        }
        // 如果所有推荐的引擎都初始化失败，记录错误并返回
        LOG_ERROR_RETURN(0, -1, "All master engines init failed");
    }
next:
    // 初始化同步信号处理机制
    if ((INIT_EVENT_SIGNAL & event_engine) && sync_signal_init() < 0)
        return -1;

    // 根据编译选项初始化各种I/O引擎
#ifdef ENABLE_FSTACK_DPDK
    INIT_IO(FSTACK_DPDK, fstack_dpdk);
#endif
    INIT_IO(EXPORTFS, exportfs)
#ifdef ENABLE_CURL
    INIT_IO(LIBCURL, libcurl)
#endif
#ifdef __linux__
    // Linux平台初始化异步I/O和边缘触发轮询器
    INIT_IO(LIBAIO, libaio_wrapper, options.libaio_queue_depth)
    INIT_IO(SOCKET_EDGE_TRIGGER, et_poller)
#endif
    // 保存已初始化的引擎类型，供清理时使用
    g_event_engine = event_engine;
    g_io_engine = io_engine;
    // 注册进程fork时的重置句柄，确保子进程中的状态正确
    if (!reset_handle_registed) {
        pthread_atfork(nullptr, nullptr, &reset_all_handle);
        LOG_DEBUG("reset_all_handle registed ", VALUE(getpid()));
        reset_handle_registed = true;
    }
    return 0;
}

/**
 * @brief Photon初始化函数
 * 
 * 此函数是用户调用的初始化入口点，内部调用__photon_init
 * 
 * @param event_engine 事件引擎标志
 * @param io_engine I/O引擎标志
 * @param options Photon运行时配置选项
 * @return 0表示成功，-1表示失败
 */
int init(uint64_t event_engine, uint64_t io_engine, const PhotonOptions& options) {
    return __photon_init(event_engine, io_engine, options);
}

// 获取当前线程的清理钩子向量
// 使用线程局部存储确保每个线程有独立的钩子列表
static std::vector<Delegate<void>>& get_hook_vector() {
    thread_local std::vector<Delegate<void>> hooks;
    return hooks;
}

/**
 * @brief 添加清理钩子
 * 
 * 此函数将指定的回调函数添加到清理钩子列表中
 * 在调用fini()时，这些钩子将被依次执行
 * 
 * @param handler 要添加的回调函数
 */
void fini_hook(Delegate<void> handler) {
    get_hook_vector().emplace_back(handler);
}

/**
 * @brief Photon清理函数
 * 
 * 此函数负责清理和释放Photon库的所有资源，包括：
 * 1. 执行所有注册的清理钩子
 * 2. 清理各种I/O引擎
 * 3. 清理信号处理
 * 4. 清理fd_events模块
 * 5. 清理虚拟CPU
 * 
 * @return 0表示成功，-1表示失败
 */
int fini() {
    // 执行所有注册的清理钩子
    for (auto h : get_hook_vector()) {
        h.fire();
    }
    // 清空钩子列表
    get_hook_vector().clear();
#ifdef __linux__
    // Linux平台清理异步I/O和边缘触发轮询器
    FINI_IO(LIBAIO, libaio_wrapper)
    FINI_IO(SOCKET_EDGE_TRIGGER, et_poller)
#endif
#ifdef ENABLE_CURL
    FINI_IO(LIBCURL, libcurl)
#endif
    FINI_IO(EXPORTFS, exportfs)
#ifdef ENABLE_FSTACK_DPDK
    FINI_IO(FSTACK_DPDK, fstack_dpdk)
#endif

    // 如果启用了信号事件引擎，则清理信号处理
    if (INIT_EVENT_SIGNAL & g_event_engine)
        sync_signal_fini();
    // 清理fd_events模块
    fd_events_fini();
    // 清理虚拟CPU
    vcpu_fini();
    // 重置引擎标志
    g_event_engine = g_io_engine = 0;
    return 0;
}

}