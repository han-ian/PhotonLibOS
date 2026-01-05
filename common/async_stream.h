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
#include <photon/common/async.h>

/**
 * @brief 异步流接口
 * 
 * IAsyncStream定义了异步流操作的接口，支持异步读写操作
 * 设计说明：
 * - 继承自IAsyncBase，提供异步操作能力
 * - 支持多种I/O操作（read/write/readv/writev等）
 * - 使用宏定义简化异步方法声明，提高代码一致性
 * - 提供操作ID用于区分不同类型的异步操作
 */
class IAsyncStream : public IAsyncBase
{
public:
    /**
     * @brief 关闭异步流
     * 
     * 异步关闭流，释放相关资源
     */
    DEFINE_ASYNC0(int, close);

    /**
     * @brief 异步读取数据
     * 
     * 从流中异步读取指定数量的数据到缓冲区
     * 
     * @param buf 目标缓冲区
     * @param count 要读取的字节数
     */
    DEFINE_ASYNC(ssize_t, read, void *buf, size_t count);
    
    /**
     * @brief 异步向量读取
     * 
     * 从流中异步读取数据到向量数组
     * 
     * @param iov iovec数组，指定多个缓冲区
     * @param iovcnt iovec数组的元素个数
     */
    DEFINE_ASYNC(ssize_t, readv, const struct iovec *iov, int iovcnt);
    
    /**
     * @brief 可变向量读取（便捷函数）
     * 
     * 向量读取的便捷实现，使用可变的iovec数组
     */
    EXPAND_FUNC(ssize_t, readv_mutable, struct iovec *iov, int iovcnt)
    {
        readv(iov, iovcnt, done, timeout);
    }

    /**
     * @brief 异步写入数据
     * 
     * 向流中异步写入指定数量的数据
     * 
     * @param buf 源数据缓冲区
     * @param count 要写入的字节数
     */
    DEFINE_ASYNC(ssize_t, write, const void *buf, size_t count);
    
    /**
     * @brief 异步向量写入
     * 
     * 向流中异步写入向量数组的数据
     * 
     * @param iov iovec数组，指定多个缓冲区
     * @param iovcnt iovec数组的元素个数
     */
    DEFINE_ASYNC(ssize_t, writev, const struct iovec *iov, int iovcnt);
    
    /**
     * @brief 可变向量写入（便捷函数）
     * 
     * 向量写入的便捷实现，使用可变的iovec数组
     */
    EXPAND_FUNC(ssize_t, writev_mutable, struct iovec *iov, int iovcnt)
    {
        writev(iov, iovcnt, done, timeout);
    }

    // 异步操作ID定义，用于区分不同类型的异步操作
    const static uint32_t OPID_CLOSE   = 0;    // 关闭操作ID
    const static uint32_t OPID_READ    = 1;    // 读取操作ID
    const static uint32_t OPID_READV   = 2;    // 向量读取操作ID
    const static uint32_t OPID_WRITE   = 3;    // 写入操作ID
    const static uint32_t OPID_WRITEV  = 4;    // 向量写入操作ID

    // 函数类型定义和辅助方法，用于区分读写操作
    using FuncIO = AsyncFunc<ssize_t, IAsyncStream, void*, size_t>;
    FuncIO _and_read()  { return &IAsyncStream::read; }      // 获取读取函数指针
    FuncIO _and_write() { return (FuncIO)&IAsyncStream::write; }  // 获取写入函数指针
    bool is_readf(FuncIO f) { return f == _and_read(); }     // 检查是否为读取函数
    bool is_writef(FuncIO f) { return f == _and_write(); }   // 检查是否为写入函数

    using FuncIOV_mutable = AsyncFunc<ssize_t, IAsyncStream, struct iovec*, int>;
    FuncIOV_mutable _and_readv_mutable()  { return &IAsyncStream::readv_mutable; }      // 获取可变向量读取函数指针
    FuncIOV_mutable _and_writev_mutable() { return &IAsyncStream::writev_mutable; }     // 获取可变向量写入函数指针
    bool is_readf_mutable(FuncIOV_mutable f) { return f == _and_readv_mutable(); }     // 检查是否为可变向量读取函数
    bool is_writef_mutable(FuncIOV_mutable f) { return f == _and_writev_mutable(); }   // 检查是否为可变向量写入函数

    using FuncIOCV = AsyncFunc<ssize_t, IAsyncStream, const struct iovec*, int>;
    FuncIOCV _and_readcv()  { return &IAsyncStream::readv; }    // 获取常量向量读取函数指针
    FuncIOCV _and_writecv() { return &IAsyncStream::writev; }   // 获取常量向量写入函数指针
    bool is_readf(FuncIOCV f) { return f == _and_readcv(); }    // 检查是否为常量向量读取函数
    bool is_writef(FuncIOCV f) { return f == _and_writecv(); }  // 检查是否为常量向量写入函数
};


//////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief 异步操作示例
 * 
 * 演示如何使用异步流接口进行异步操作
 * 设计说明：
 * - 展示了异步操作的典型使用模式
 * - 包含错误处理和结果处理逻辑
 * - 说明了回调函数的使用方法
 */
class Example_of_Async_Operation
{
public:
    IAsyncStream* m_astream;
    
    /**
     * @brief 异步预读操作
     * 
     * 发起异步读取操作，指定完成后调用on_read_done回调
     */
    void do_async_pread(void *buf, size_t count)
    {
        // this->on_read_done(aop) 将在操作完成时被调用
        m_astream->read(buf, count, {this, &Example_of_Async_Operation::on_read_done});
    }

protected:
    /**
     * @brief 读取完成回调
     * 
     * 处理异步读取操作的结果，包括成功和失败情况
     * 
     * @param aop 异步操作结果对象
     * @return 0表示成功处理，-1表示处理失败
     * 
     * 设计说明：
     * - 检查操作结果和错误码
     * - 提供详细的成功/失败信息
     * - 遵循标准错误处理模式
     */
    int on_read_done(AsyncResult<ssize_t>* aop)
    {
        if (aop->result < 0)
        {
            printf("[%p].async_read() is failed, with result=%d, and errno=%d, %s\n",
                   aop->object, (int)aop->result, aop->error_number, strerror(aop->error_number));
            return -1;
        }

        printf("[%p].async_read() is successfully done, with result=%d", aop->object, (int)aop->result);
        return 0;
    }
    const char* strerror(int e) { return "some error message"; }
    void printf(...) { }
};