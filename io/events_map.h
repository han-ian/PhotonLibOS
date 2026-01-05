#pragma once

#include <photon/io/fd-events.h>

namespace photon {

using EVENT_TYPE = int;

/**
 * @brief 事件组基础模板
 * 
 * 定义读、写、错误事件的常量值，并提供编译时检查确保事件值的唯一性
 * 
 * @tparam EV_READ_ 读事件值
 * @tparam EV_WRITE_ 写事件值
 * @tparam EV_ERROR_ 错误事件值
 * 
 * 设计说明：
 * - 使用模板参数定义事件值，提供类型安全的事件定义
 * - 静态断言确保不同事件值不相同，防止逻辑错误
 * - 静态断言确保事件值非零，保证事件有效性
 */
template <EVENT_TYPE EV_READ_, EVENT_TYPE EV_WRITE_, EVENT_TYPE EV_ERROR_>
struct EVGroupBase {
    static constexpr EVENT_TYPE EV_READ = EV_READ_;      // 读事件常量
    static constexpr EVENT_TYPE EV_WRITE = EV_WRITE_;    // 写事件常量
    static constexpr EVENT_TYPE EV_ERR = EV_ERROR_;      // 错误事件常量

    // 静态断言确保事件值的唯一性
    static_assert(EV_READ != EV_WRITE, "读写事件值不能相同");
    static_assert(EV_READ != EV_ERR, "读错误事件值不能相同");
    static_assert(EV_ERR != EV_WRITE, "错误写事件值不能相同");
    // 静态断言确保事件值非零
    static_assert(EV_READ, "读事件值不能为0");
    static_assert(EV_WRITE, "写事件值不能为0");
    static_assert(EV_ERR, "错误事件值不能为0");
};

// 事件类型标记结构体
struct EVUBase {};    // 底层事件标记
struct EVKBase {};    // 事件键值标记

/**
 * @brief 底层事件定义模板
 * 
 * 继承自EVGroupBase和EVUBase，用于定义底层事件类型
 * 
 * 设计说明：
 * - EVUBase标记表示这是一个底层事件类型
 * - 用于在事件映射中区分底层事件和键值事件
 */
template <EVENT_TYPE EV_READ_, EVENT_TYPE EV_WRITE_, EVENT_TYPE EV_ERROR_>
struct EVUnderlay : EVGroupBase<EV_READ_, EV_WRITE_, EV_ERROR_>, EVUBase {};

/**
 * @brief 事件键值定义模板
 * 
 * 继承自EVGroupBase和EVKBase，用于定义事件键值类型
 * 
 * 设计说明：
 * - EVKBase标记表示这是一个事件键值类型
 * - 用于在事件映射中作为键值进行事件转换
 */
template <EVENT_TYPE EV_READ_, EVENT_TYPE EV_WRITE_, EVENT_TYPE EV_ERROR_>
struct EVKey : EVGroupBase<EV_READ_, EV_WRITE_, EV_ERROR_>, EVKBase {};

/**
 * @brief 事件映射模板
 * 
 * 提供不同事件类型之间的转换功能
 * 
 * @tparam EV_UNDERLAY 底层事件类型
 * @tparam EV_KEY 事件键值类型，默认为标准读写错误事件
 * 
 * 设计说明：
 * - 静态断言确保EV_UNDERLAY是EVUnderlay类型，EV_KEY是EVKey类型
 * - 支持按位转换和按值转换两种事件转换方式
 * - 允许不同事件系统之间的适配和转换
 */
template <typename EV_UNDERLAY,
          typename EV_KEY = EVKey<EVENT_READ, EVENT_WRITE, EVENT_ERROR>>
struct EventsMap {
    // 静态断言确保类型正确性
    static_assert(std::is_base_of<EVUBase, EV_UNDERLAY>::value,
                  "EV_UNDERLAY应该是一个EVUnderlay类型");
    static_assert(std::is_base_of<EVKBase, EV_KEY>::value,
                  "EV_KEY应该是一个EVKey类型");

    // 定义底层事件常量
    static constexpr EVENT_TYPE UNDERLAY_EVENT_READ = EV_UNDERLAY::EV_READ;
    static constexpr EVENT_TYPE UNDERLAY_EVENT_WRITE = EV_UNDERLAY::EV_WRITE;
    static constexpr EVENT_TYPE UNDERLAY_EVENT_ERROR = EV_UNDERLAY::EV_ERR;

    /**
     * @brief 按位转换事件
     * 
     * 将键值事件转换为底层事件，支持组合事件的转换
     * 
     * @param events 要转换的事件组合
     * @return 转换后的底层事件组合
     * 
     * 设计说明：
     * - 支持同时转换多个事件标志
     * - 保持事件的组合特性
     * - 用于将标准事件转换为特定事件系统支持的事件
     */
    EVENT_TYPE translate_bitwisely(EVENT_TYPE events) const {
        EVENT_TYPE ret = 0;
        if (events & EV_KEY::EV_READ) ret |= EV_UNDERLAY::EV_READ;
        if (events & EV_KEY::EV_WRITE) ret |= EV_UNDERLAY::EV_WRITE;
        if (events & EV_KEY::EV_ERR) ret |= EV_UNDERLAY::EV_ERR;
        return ret;
    }

    /**
     * @brief 按值转换事件
     * 
     * 将单个键值事件转换为对应的底层事件
     * 
     * @param event 要转换的单个事件
     * @return 转换后的底层事件
     * 
     * 设计说明：
     * - 仅转换单个事件值
     * - 用于一对一事件映射
     * - 在某些事件系统中可能更高效
     */
    EVENT_TYPE translate_byval(EVENT_TYPE event) const {
        if (event == EV_KEY::EV_READ) return EV_UNDERLAY::EV_READ;
        if (event == EV_KEY::EV_WRITE) return EV_UNDERLAY::EV_WRITE;
        if (event == EV_KEY::EV_ERR) return EV_UNDERLAY::EV_ERR;
        // 如果event不是已知事件类型，这里没有返回值会导致未定义行为
        // 在实际使用中应确保event是有效的事件类型
    }
};

}  // namespace photon