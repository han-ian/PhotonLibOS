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
#include <sys/types.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/common/timeout.h>

namespace photon {

// 事件类型定义，用于指定对文件描述符感兴趣的事件
const static uint32_t EVENT_READ = 1;       // 可读事件
const static uint32_t EVENT_WRITE = 2;      // 可写事件
const static uint32_t EVENT_ERROR = 4;      // 错误事件
const static uint32_t EVENT_RWE = EVENT_READ | EVENT_WRITE | EVENT_ERROR;  // 读、写、错误事件的组合
const static uint32_t EDGE_TRIGGERED = 0x4000;   // 边缘触发模式标志
const static uint32_t ONE_SHOT = 0x8000;         // 单次触发模式标志

// 事件成功标志，表示I/O操作成功完成
const static int EOK = ENXIO;   // 下一个I/O事件的标志

/**
 * @brief 事件引擎抽象
 * 
 * 事件引擎是epoll、io_uring、kqueue等底层事件机制的抽象层
 * 主事件引擎（Master event engine）是全局函数wait_for_fd_readable/writable()使用的默认引擎
 * 它也被线程调度器在空闲时用于等待事件
 * 每个处理事件的vCPU都有一个专用的主引擎
 */
class MasterEventEngine {
public:
    virtual ~MasterEventEngine() = default;

    /**
     * @brief 等待文件描述符上的指定事件
     * 
     * 等待文件描述符上的指定兴趣事件发生
     * 
     * @param fd 文件描述符
     * @param interest 事件类型（EVENT_READ、EVENT_WRITE或EVENT_ERROR）
     * @param timeout 超时时间
     * @return 0表示成功，事件在规定时间内到达
     *         -1表示失败，可能是超时或被其他线程中断
     */
    virtual int wait_for_fd(int fd, uint32_t interest, Timeout timeout) = 0;

    /**
     * @brief 等待文件描述符可读
     * 
     * 调用wait_for_fd等待读事件
     * 
     * @param fd 文件描述符
     * @param timeout 超时时间
     * @return 等待结果
     */
    int wait_for_fd_readable(int fd, Timeout timeout = {}) {
        return wait_for_fd(fd, EVENT_READ, timeout);
    }

    /**
     * @brief 等待文件描述符可写
     * 
     * 调用wait_for_fd等待写事件
     * 
     * @param fd 文件描述符
     * @param timeout 超时时间
     * @return 等待结果
     */
    int wait_for_fd_writable(int fd, Timeout timeout = {}) {
        return wait_for_fd(fd, EVENT_WRITE, timeout);
    }

    /**
     * @brief 等待文件描述符错误
     * 
     * 调用wait_for_fd等待错误事件
     * 
     * @param fd 文件描述符
     * @param timeout 超时时间
     * @return 等待结果
     */
    int wait_for_fd_error(int fd, Timeout timeout = {}) {
        return wait_for_fd(fd, EVENT_ERROR, timeout);
    }

    /**
     * @brief 等待事件并触发
     * 
     * 等待事件发生，并通过photon::thread_interrupt()触发它们
     * 
     * @param timeout 最大睡眠时间，如果发生某些事件可能会提前唤醒
     * @return 0表示睡眠良好，-1表示发生错误
     * @warning 不要在该函数中调用photon::usleep()或photon::sleep()，
     *          因为它们的实现也依赖于该函数
     */
    virtual ssize_t wait_and_fire_events(uint64_t timeout) = 0;

    /**
     * @brief 取消等待
     * 
     * 取消当前的等待操作，通常用于中断等待状态
     * 
     * @return 0表示成功，-1表示失败
     */
    virtual int cancel_wait() = 0;
};

/**
 * @brief 等待文件描述符可读（全局函数）
 * 
 * 通过当前vCPU的主事件引擎等待文件描述符变为可读
 * 
 * @param fd 文件描述符
 * @param timeout 超时时间
 * @return 等待结果
 */
inline int wait_for_fd_readable(int fd, Timeout timeout = {}) {
    return get_vcpu()->master_event_engine->wait_for_fd_readable(fd, timeout);
}

/**
 * @brief 等待文件描述符可写（全局函数）
 * 
 * 通过当前vCPU的主事件引擎等待文件描述符变为可写
 * 
 * @param fd 文件描述符
 * @param timeout 超时时间
 * @return 等待结果
 */
inline int wait_for_fd_writable(int fd, Timeout timeout = {}) {
    return get_vcpu()->master_event_engine->wait_for_fd_writable(fd, timeout);
}

/**
 * @brief 等待文件描述符错误（全局函数）
 * 
 * 通过当前vCPU的主事件引擎等待文件描述符发生错误
 * 
 * @param fd 文件描述符
 * @param timeout 超时时间
 * @return 等待结果
 */
inline int wait_for_fd_error(int fd, Timeout timeout = {}) {
    return get_vcpu()->master_event_engine->wait_for_fd_error(fd, timeout);
}

/**
 * @brief 级联事件引擎
 * 
 * 级联事件引擎用于主引擎无法处理的复杂场景，
 * 例如单次调用等待多个事件
 * 
 * 级联事件引擎不会阻塞vCPU，而只会阻塞当前线程，
 * 它借助主事件引擎实现
 */
class CascadingEventEngine {
public:
    virtual ~CascadingEventEngine() = default;

    struct Event {
        int fd;                 // 文件描述符
        uint32_t interests;     // 兴趣事件（位或运算组合的EVENT_READ、EVENT_WRITE）
        void* data;             // 用户自定义数据
    };

    /**
     * @brief 等待多个事件
     * 
     * 等待多个文件描述符上的事件
     * 
     * @param events 事件数组
     * @param nevents 事件数量
     * @param timeout 超时时间
     * @return 返回就绪的事件数量，-1表示错误
     */
    virtual ssize_t wait_and_fire_events(Event* events, size_t nevents, Timeout timeout) = 0;

    /**
     * @brief 取消等待
     * 
     * 取消当前的等待操作
     * 
     * @return 0表示成功，-1表示失败
     */
    virtual int cancel_wait() = 0;
};

/**
 * @brief 获取级联事件引擎
 * 
 * 获取当前vCPU的级联事件引擎实例
 * 
 * @return CascadingEventEngine指针，失败返回nullptr
 */
inline CascadingEventEngine* get_cascading_engine() {
    return get_vcpu()->cascading_event_engine;
}

/**
 * @brief 等待文件描述符可读（级联引擎）
 * 
 * 使用级联事件引擎等待文件描述符可读
 * 
 * @param fd 文件描述符
 * @param timeout 超时时间
 * @return 0表示成功，-1表示失败
 */
inline int wait_for_fd_readable(CascadingEventEngine* engine, int fd, Timeout timeout = {}) {
    CascadingEventEngine::Event ev{fd, EVENT_READ, nullptr};
    return engine->wait_and_fire_events(&ev, 1, timeout);
}

/**
 * @brief 等待文件描述符可写（级联引擎）
 * 
 * 使用级联事件引擎等待文件描述符可写
 * 
 * @param fd 文件描述符
 * @param timeout 超时时间
 * @return 0表示成功，-1表示失败
 */
inline int wait_for_fd_writable(CascadingEventEngine* engine, int fd, Timeout timeout = {}) {
    CascadingEventEngine::Event ev{fd, EVENT_WRITE, nullptr};
    return engine->wait_and_fire_events(&ev, 1, timeout);
}

/**
 * @brief 等待文件描述符错误（级联引擎）
 * 
 * 使用级联事件引擎等待文件描述符错误
 * 
 * @param fd 文件描述符
 * @param timeout 超时时间
 * @return 0表示成功，-1表示失败
 */
inline int wait_for_fd_error(CascadingEventEngine* engine, int fd, Timeout timeout = {}) {
    CascadingEventEngine::Event ev{fd, EVENT_ERROR, nullptr};
    return engine->wait_and_fire_events(&ev, 1, timeout);
}

/**
 * @brief 初始化fd_events模块
 * 
 * 使用指定的主事件引擎初始化fd_events模块
 * 
 * @param master 主事件引擎
 * @return 0表示成功，-1表示失败
 */
int fd_events_init(MasterEventEngine* master);

/**
 * @brief 清理fd_events模块
 * 
 * 清理fd_events模块的资源
 */
void fd_events_fini();

/**
 * @brief 获取主事件引擎
 * 
 * 获取当前vCPU的主事件引擎
 * 
 * @return MasterEventEngine指针
 */
inline MasterEventEngine* get_master_event_engine() {
    return get_vcpu()->master_event_engine;
}

}