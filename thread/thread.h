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
#include <photon/common/callback.h>
#include <photon/common/timeout.h>
#include <photon/thread/stack-allocator.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <type_traits>
#ifndef __aarch64__
#include <emmintrin.h>
#endif

namespace photon
{
    // 虚拟CPU (vCPU) 标志定义
    constexpr uint8_t  VCPU_ENABLE_ACTIVE_WORK_STEALING     = 1;    // 允许此vCPU从其他vCPU窃取工作，提高负载均衡
    constexpr uint8_t  VCPU_ENABLE_PASSIVE_WORK_STEALING    = 2;    // 允许此vCPU被其他vCPU窃取工作，实现任务均衡

    // 线程标志定义
    constexpr uint32_t THREAD_JOINABLE                      = 1;    // 允许此线程被join，确保线程资源正确回收
    constexpr uint32_t THREAD_ENABLE_WORK_STEALING          = 2;    // 允许此线程被其他vCPU窃取，优化多vCPU环境下的任务分配
    constexpr uint32_t THREAD_PAUSE_WORK_STEALING           = 4;    // 暂时暂停此线程的工作窃取，用于同步关键操作

    /**
     * @brief 初始化虚拟CPU
     * 
     * 此函数初始化当前线程上下文中的虚拟CPU，是使用Photon协程库前的必要步骤
     * 设计说明：vCPU是Photon协程的执行环境，所有协程都在vCPU上调度和运行
     * 
     * @param flags 虚拟CPU初始化标志
     * @return 0表示成功，-1表示失败
     */
    int vcpu_init(uint64_t flags = 0);

    /**
     * @brief 清理虚拟CPU
     * 
     * 此函数清理当前虚拟CPU的资源，应与vcpu_init()成对调用
     * 设计说明：确保所有协程正确结束，防止资源泄漏
     * 
     * @return 0表示成功，-1表示失败
     */
    int vcpu_fini();

    /**
     * @brief 等待所有线程完成
     * 
     * 等待当前vCPU上的所有线程完成执行
     * 设计说明：确保在vCPU清理前所有协程都已终止，避免访问已释放的资源
     * 
     * @return 0表示成功，-1表示失败
     */
    int wait_all();

    /**
     * @brief 初始化时间戳更新器
     * 
     * 初始化用于定期更新全局时间戳的组件
     * 设计说明：全局时间戳用于协程调度和超时管理，需要定期更新以保持准确性
     * 
     * @return 0表示成功，-1表示失败
     */
    int timestamp_updater_init();

    /**
     * @brief 清理时间戳更新器
     * 
     * 清理时间戳更新器资源
     * 设计说明：与timestamp_updater_init()成对使用，确保资源正确释放
     * 
     * @return 0表示成功，-1表示失败
     */
    int timestamp_updater_fini();

    struct thread;
    extern __thread thread* CURRENT;  // 指向当前运行线程的指针，线程局部变量
    extern volatile uint64_t now;     // 全局时间戳，单位为微秒，粗粒度更新
    
    /**
     * @brief 时间戳结构体
     * 
     * 包含当前时间戳和秒/微秒表示
     * 设计说明：封装时间信息，便于在不同时间格式间转换
     */
    struct NowTime {
        uint64_t now, _sec_usec;
        uint32_t  sec() { return _sec_usec >> 32; }      // 获取秒部分
        uint32_t usec() { auto p = (uint32_t*)&_sec_usec; return *p; }  // 获取微秒部分
    };
    
    /**
     * @brief 更新全局时间戳
     * 
     * 更新全局时间戳变量 `now`
     * 设计说明：此函数确保所有基于时间的协程操作都使用最新时间戳，是超时机制的基础
     * 
     * @return NowTime结构体，包含更新后的时间信息
     */
    NowTime __update_now();    // 更新 `now`

    // 线程状态枚举
    // 设计说明：状态管理是协程调度的核心，各状态间转换需严格控制以保证并发安全
    enum states
    {
        READY = 0,      // 准备运行状态，线程已准备好但未在运行
        RUNNING = 1,    // 运行状态（每个vCPU同一时间只能有一个线程处于运行状态），保证线程安全
        SLEEPING = 2,   // 睡眠状态，等待事件或超时，暂时不占用CPU资源
        DONE = 4,       // 已完成生命周期状态，等待清理
        STANDBY = 8,    // 跨vCPU调度时的内部使用状态，用于工作窃取机制
    };

    // 定义线程入口函数类型
    typedef void* (*thread_entry)(void*);
    const uint64_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;  // 默认栈大小为8MB，平衡性能与内存使用
    
    /**
     * @brief 创建新线程
     * 
     * 创建一个新线程，以start(arg)为入口点，使用指定的栈大小
     * 预留空间可用于向新线程传递大参数
     * 设计说明：此函数是协程创建的入口，负责初始化协程上下文和栈
     * 
     * @param start 线程入口函数
     * @param arg 传递给入口函数的参数
     * @param stack_size 线程栈大小，默认为8MB
     * @param reserved_space 预留空间大小，必须 <= stack_size / 2
     * @param flags 线程标志
     * @return 成功返回线程指针，失败返回nullptr
     */
    thread* thread_create(thread_entry start, void* arg,
        uint64_t stack_size = DEFAULT_STACK_SIZE,
        uint32_t reserved_space = 0, uint64_t flags = 0);

    /**
     * @brief 获取预留空间地址
     * 
     * 获取线程结构体下方预留空间的地址，用于传递参数
     * 设计说明：通过预留空间可以高效地向新创建的线程传递大数据结构，避免额外的内存分配
     * 
     * @tparam T 预留空间类型
     * @param th 线程指针
     * @param reserved_size 预留空间大小
     * @return 指向预留空间的T类型指针
     */
    template<typename T = void> inline
    T* thread_reserved_space(thread* th, uint64_t reserved_size = sizeof(T)) {
        return (T*)((char*)th - reserved_size);
    }

    // 线程可通过join句柄进行join操作
    // 一旦启用join，线程将一直存在直到被join
    // 如果不进行join操作会导致资源泄漏
    // 设计说明：join机制确保线程资源正确回收，防止内存泄漏
    struct join_handle;
    
    /**
     * @brief 启用或禁用线程join
     * 
     * 使线程可以被join，或取消join能力
     * 设计说明：join功能通过引用计数管理线程生命周期，防止线程结束时资源提前释放
     * 
     * @param th 要操作的线程
     * @param flag true表示启用join，false表示禁用
     * @return join句柄指针
     */
    join_handle* thread_enable_join(thread* th, bool flag = true);
    
    /**
     * @brief 等待线程结束并获取返回值
     * 
     * 阻塞当前线程直到指定线程结束，并获取其返回值
     * 设计说明：阻塞当前线程直到目标线程完成，实现线程同步
     * 
     * @param jh join句柄
     * @return 线程的返回值
     */
    void* thread_join(join_handle* jh);

    /**
     * @brief 以指定返回值结束当前线程
     * 
     * 终止当前线程并设置返回值
     * 设计说明：提供优雅的线程退出机制，确保资源正确释放
     * 
     * @param retval 线程返回值
     */
    void thread_exit(void* retval) __attribute__((noreturn));

    /**
     * @brief 让出CPU控制权给其他线程
     * 
     * 当前线程让出CPU，但不进入睡眠队列
     * 设计说明：实现协作式调度，允许其他就绪线程运行，但当前线程保持就绪状态
     * 
     * @return 如果在让出过程中被中断则返回错误码
     */
    int thread_yield();

    /**
     * @brief 让出CPU控制权给指定线程
     * 
     * 当前线程让出CPU给指定线程，目标线程必须处于RUNNING状态
     * 设计说明：提供更精确的调度控制，允许线程间直接协作
     * 
     * @param th 要切换到的线程
     * @return 如果在让出过程中被中断则返回错误码
     */
    int thread_yield_to(thread* th);

    /**
     * @brief 使当前线程休眠指定时间
     * 
     * 挂起当前线程指定时间，同时切换到其他线程执行，
     * 并可能唤醒其他处于睡眠状态的线程
     * 设计说明：实现非阻塞休眠，提高系统并发性能
     * 
     * @param timeout 休眠超时时间
     * @return 0表示超时结束，-1表示被中断，errno设置为中断发起者的错误码
     */
    int thread_usleep(Timeout timeout);

    /**
     * @brief 使当前线程休眠指定微秒数
     * 
     * 挂起当前线程指定微秒数，同时切换到其他线程执行
     * 设计说明：便捷的休眠函数，基于thread_usleep实现
     * 
     * @param usec 休眠微秒数
     */
    inline void thread_usleep(uint64_t usec) {
        thread_usleep(Timeout(usec));
    }

    /**
     * @brief 使当前线程休眠指定秒数
     * 
     * 挂起当前线程指定秒数，同时切换到其他线程执行
     * 设计说明：便捷的休眠函数，基于thread_usleep实现
     * 
     * @param sec 休眠秒数
     */
    inline void thread_sleep(uint64_t sec) {
        thread_usleep(sec * 1000000UL);
    }

    /**
     * @brief 使当前线程休眠直到指定时间点
     * 
     * 挂起当前线程直到指定的时间点
     * 设计说明：实现基于绝对时间的休眠，适用于定时任务场景
     * 
     * @param until 绝对时间点（微秒）
     * @return 0表示到达时间点，-1表示被中断
     */
    int thread_sleep_until(uint64_t until);

    /**
     * @brief 中断指定线程
     * 
     * 通知指定线程中断其等待状态
     * 设计说明：提供线程间通信机制，允许一个线程中断另一个线程的等待状态
     * 
     * @param th 要中断的线程
     * @param errn 中断原因的错误码，默认为EOK
     * @return 0表示成功，-1表示失败
     */
    int thread_interrupt(thread* th, int errn = EOK);

    /**
     * @brief 检查线程是否可被中断
     * 
     * 检查线程是否设置了不可中断标志
     * 设计说明：允许线程在关键操作期间防止被中断，确保操作的原子性
     * 
     * @param th 要检查的线程
     * @return 0表示可中断，非0表示不可中断
     */
    int is_thread_interruptible(thread* th);

    /**
     * @brief 获取线程状态
     * 
     * 获取指定线程的当前状态
     * 设计说明：提供线程状态的只读访问，用于调试和状态管理
     * 
     * @param th 线程指针
     * @return 线程状态枚举值
     */
    int get_thread_state(thread* th);

    /**
     * @brief 设置线程状态
     * 
     * 设置指定线程的状态
     * 设计说明：提供线程状态的受控修改，确保状态转换的正确性
     * 
     * @param th 线程指针
     * @param st 新状态
     * @return 旧状态
     */
    int set_thread_state(thread* th, int st);

    /**
     * @brief 获取线程栈顶指针
     * 
     * 获取指定线程的栈顶指针，用于调试和分析
     * 设计说明：提供对线程栈的访问，便于调试和性能分析
     * 
     * @param th 线程指针
     * @return 栈顶指针
     */
    void* get_stack_top(thread* th);

    /**
     * @brief 获取线程栈底指针
     * 
     * 获取指定线程的栈底指针，用于调试和分析
     * 设计说明：提供对线程栈的访问，便于调试和性能分析
     * 
     * @param th 线程指针
     * @return 栈底指针
     */
    void* get_stack_bottom(thread* th);

    /**
     * @brief 获取当前线程
     * 
     * 获取当前正在执行的线程指针
     * 设计说明：提供对当前执行上下文的访问，是协程调度的基础
     * 
     * @return 当前线程指针
     */
    inline thread* get_current_thread() {
        return CURRENT;
    }

    /**
     * @brief 检查是否在Photon线程中运行
     * 
     * 检查当前是否在Photon协程环境中运行
     * 设计说明：区分原生线程和Photon协程上下文，避免在错误的环境中调用Photon API
     * 
     * @return true表示在Photon线程中，false表示不在
     */
    inline bool is_photon_thread() {
        return CURRENT != nullptr;
    }

    // 虚拟CPU结构体定义
    struct vcpu_base;
    extern std::vector<vcpu_base*> vcpus;  // 所有虚拟CPU的列表

    /**
     * @brief 获取当前虚拟CPU
     * 
     * 获取当前线程所在的虚拟CPU
     * 设计说明：提供对执行环境的访问，支持多vCPU架构
     * 
     * @return vcpu_base指针
     */
    vcpu_base* get_vcpu();

    /**
     * @brief 检查线程是否在当前虚拟CPU上
     * 
     * 检查指定线程是否属于当前虚拟CPU
     * 设计说明：支持多vCPU环境下的线程管理和调度
     * 
     * @param th 要检查的线程
     * @return true表示在当前vCPU，false表示不在
     */
    bool is_thread_in_current_vcpu(thread* th);

    /**
     * @brief 获取线程的虚拟CPU
     * 
     * 获取指定线程所属的虚拟CPU
     * 设计说明：提供线程与vCPU的关联信息，支持跨vCPU调度
     * 
     * @param th 线程指针
     * @return 线程所属的vcpu_base指针
     */
    vcpu_base* get_vcpu(thread* th);

    /**
     * @brief 获取线程ID
     * 
     * 获取指定线程的唯一标识符
     * 设计说明：提供线程的唯一标识，支持线程追踪和调试
     * 
     * @param th 线程指针
     * @return 线程ID
     */
    uint64_t get_thread_id(thread* th);

    /**
     * @brief 获取当前线程ID
     * 
     * 获取当前线程的唯一标识符
     * 设计说明：便捷的线程ID获取函数
     * 
     * @return 当前线程ID
     */
    inline uint64_t get_current_thread_id() {
        return get_thread_id(CURRENT);
    }

    /**
     * @brief 设置栈保护大小
     * 
     * 设置线程栈的保护页大小，用于检测栈溢出
     * 设计说明：通过保护页机制防止栈溢出导致的内存损坏，提高系统稳定性
     * 
     * @param size 保护大小
     */
    void set_stack_protector_size(uint64_t size);

    /**
     * @brief 获取栈保护大小
     * 
     * 获取当前栈保护页的大小
     * 设计说明：提供对栈保护设置的访问，便于调试和配置
     * 
     * @return 保护大小
     */
    uint64_t get_stack_protector_size();

    /**
     * @brief 等待条件变量
     * 
     * 在指定条件下等待，使用超时机制
     * 设计说明：实现条件等待机制，支持非阻塞的同步操作，避免忙等待
     * 
     * @param cond 条件判断函数
     * @param timeout 超时时间
     * @return 等待结果
     */
    template<typename Cond>
    inline int wait_for(Cond&& cond, Timeout timeout = {}) {
        while (!cond()) {
            if (timeout.expire() <= now) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (thread_usleep(std::min((uint64_t)10000, timeout.left())) < 0)
                return -1;
        }
        return 0;
    }

    /**
     * @brief 等待条件变量（以微秒为单位）
     * 
     * 在指定条件下等待，使用微秒超时
     * 设计说明：便捷的条件等待函数，基于通用wait_for实现
     * 
     * @param cond 条件判断函数
     * @param timeout_us 超时时间（微秒）
     * @return 等待结果
     */
    template<typename Cond>
    inline int wait_for(Cond&& cond, uint64_t timeout_us) {
        return wait_for(cond, Timeout(timeout_us));
    }
}