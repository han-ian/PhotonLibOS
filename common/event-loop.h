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
#include <photon/common/object.h>

namespace photon {
struct thread;
};

/**
 * @brief 事件循环类
 * 
 * EventLoop是Photon中事件驱动的核心组件，用于等待和处理事件
 * 设计说明：
 * - 通过Wait4Events回调等待事件，OnEvents回调处理事件
 * - 支持同步和异步运行模式，适应不同使用场景
 * - 管理事件循环的生命周期和状态转换
 */
class EventLoop : public Object {
public:
    const static int STOP = 0;        // 停止状态，事件循环未运行
    const static int RUNNING = 1;     // 运行状态，事件循环正在处理事件
    const static int WAITING = 2;     // 等待状态，事件循环正在等待事件
    const static int STOPPING = -1;   // 停止中状态，事件循环正在停止

    // 返回值 > 0 表示有事件
    // 返回值 = 0 表示仍然没有事件
    // 返回值 < 0 表示被中断，将退出循环
    using Wait4Events = Callback<EventLoop*>;

    // 返回值被忽略
    using OnEvents = Callback<EventLoop*>;

    /**
     * @brief 运行事件循环
     * 
     * 运行事件循环并阻塞当前Photon线程，直到循环停止
     * 设计说明：同步运行模式，调用线程会被阻塞直到事件循环结束
     */
    virtual void run() = 0;
    
    /**
     * @brief 异步运行事件循环
     * 
     * 在新的Photon线程中运行事件循环，不会阻塞当前线程
     * 设计说明：异步运行模式，创建新线程执行事件循环，允许调用者继续执行其他任务
     */
    virtual void async_run() = 0;
    
    /**
     * @brief 停止事件循环
     * 
     * 请求停止事件循环，使其退出运行状态
     * 设计说明：优雅地停止事件循环，可能需要等待当前事件处理完成
     */
    virtual void stop() = 0;

    /**
     * @brief 获取事件循环状态
     * 
     * @return 当前事件循环的状态
     */
    int state() { return m_state; }

    /**
     * @brief 获取事件循环线程
     * 
     * @return 运行事件循环的Photon线程指针
     */
    photon::thread* loop_thread() { return m_thread; }

protected:
    EventLoop() {}  // 不允许直接构造，必须通过工厂函数创建
    photon::thread* m_thread = nullptr;  // 运行事件循环的线程
    int m_state = STOP;                  // 事件循环当前状态
};

/**
 * @brief 创建事件循环
 * 
 * 使用指定的等待和事件处理回调创建新的事件循环实例
 * 
 * @param wait 等待事件的回调函数
 * @param on_event 处理事件的回调函数
 * @return EventLoop指针，创建失败则返回nullptr
 * 
 * 设计说明：
 * - 工厂函数模式创建事件循环，确保正确初始化
 * - 通过回调函数注入事件等待和处理逻辑，提高灵活性
 */
extern "C" EventLoop* new_event_loop(EventLoop::Wait4Events wait,
                                     EventLoop::OnEvents on_event);