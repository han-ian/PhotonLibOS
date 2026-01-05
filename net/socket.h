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
#include <cinttypes>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <cstring>

#include <photon/common/stream.h>
#include <photon/common/callback.h>
#include <photon/common/object.h>
#include <photon/common/string_view.h>

#ifdef __linux__
#define _in_addr_field s6_addr32
#else // macOS
#define _in_addr_field __u6_addr.__u64_addr32
#endif

struct LogBuffer;
LogBuffer& operator << (LogBuffer& log, const in_addr& iaddr);
LogBuffer& operator << (LogBuffer& log, const sockaddr_in& addr);
LogBuffer& operator << (LogBuffer& log, const in6_addr& iaddr);
LogBuffer& operator << (LogBuffer& log, const sockaddr_in6& addr);

namespace photon {
namespace net {

    /**
     * @brief IP地址结构体
     * 
     * 支持IPv4和IPv6地址的统一表示，内部使用IPv6格式存储
     * 设计说明：
     * - 使用union结构兼容IPv4和IPv6地址
     * - IPv4地址以IPv4-mapped IPv6格式存储
     * - 提供多种构造方式，方便不同场景使用
     */
    struct __attribute__ ((packed)) IPAddr {
    public:
        union {
            in6_addr addr = {};  // IPv6地址格式，所有数据均以网络字节序存储
            // 所有数据都以网络字节序存储
            struct { uint16_t _1, _2, _3, _4, _5, _6; uint8_t a, b, c, d; };
        } __attribute__((packed));
        
        // 为兼容性，默认构造函数仍为0.0.0.0（IPv4）
        IPAddr() {
            map_v4(htonl(INADDR_ANY));
        }
        
        // IPv6构造函数（Internet Address）
        explicit IPAddr(in6_addr internet_addr) {
            addr = internet_addr;
        }
        
        // IPv6构造函数（网络字节序）
        IPAddr(uint32_t nl1, uint32_t nl2, uint32_t nl3, uint32_t nl4) {
            addr._in_addr_field[0] = nl1;
            addr._in_addr_field[1] = nl2;
            addr._in_addr_field[2] = nl3;
            addr._in_addr_field[3] = nl4;
        }
        
        // IPv4构造函数（Internet Address）
        explicit IPAddr(in_addr internet_addr) {
            map_v4(internet_addr);
        }
        
        // IPv4构造函数（网络字节序）
        explicit IPAddr(uint32_t nl) {
            map_v4(nl);
        }
        
        // 字符串构造函数
        explicit IPAddr(const char* s) {
            if (inet_pton(AF_INET6, s, &addr) > 0) {
                return;
            }
            in_addr v4_addr;
            if (inet_pton(AF_INET, s, &v4_addr) > 0) {
                map_v4(v4_addr);
                return;
            }
            // 无效字符串，设为默认值
            *this = IPAddr();
        }
        
        // 检查是否是映射在IPv6中的IPv4地址
        bool is_ipv4() const {
            return addr._in_addr_field[0] == 0 &&
                   addr._in_addr_field[1] == 0 &&
                   addr._in_addr_field[2] == htonl(0x0000ffff);
        }
        
        bool is_ipv6() const {
            return !is_ipv4();
        }
        
        // 我们将默认IPv4 0.0.0.0视为未定义
        bool undefined() const {
            return mem_equal(V4Any());
        }
        
        void reset() { *this = IPAddr(); }
        void clear() { reset(); }
        
        // 仅用于IPv4地址
        uint32_t to_nl() const {
            assert(is_ipv4());
            return addr._in_addr_field[3];
        }
        
        bool is_loopback() const {
            return is_ipv4() ? mem_equal(V4Loopback()) : mem_equal(V6Loopback());
        }
        
        bool is_localhost() const {
            return is_loopback();
        }
        
        bool is_broadcast() const {
            // IPv6不支持广播
            return is_ipv4() && mem_equal(V4Broadcast());
        }
        
        bool is_link_local() const {
            if (is_ipv4()) {
                return (to_nl() & htonl(0xffff0000)) == htonl(0xa9fe0000);
            } else {
                return (addr._in_addr_field[0] & htonl(0xffc00000)) == htonl(0xfe800000);
            }
        }
        
        bool operator==(const IPAddr& rhs) const {
            return mem_equal(rhs);
        }
        
        bool operator!=(const IPAddr& rhs) const {
            return !(*this == rhs);
        }
        
        static IPAddr V6None() {
            return IPAddr(htonl(0xffffffff), htonl(0xffffffff), htonl(0xffffffff), htonl(0xffffffff));
        }
        static IPAddr V6Any() { return IPAddr(in6addr_any); }
        static IPAddr V6Loopback() { return IPAddr(in6addr_loopback); }
        static IPAddr V4Broadcast() { return IPAddr(htonl(INADDR_BROADCAST)); }
        static IPAddr V4Any() { return IPAddr(htonl(INADDR_ANY)); }
        static IPAddr V4Loopback() { return IPAddr(htonl(INADDR_LOOPBACK)); }
        static IPAddr Localhost() { return V4Loopback(); }
        
    private:
        bool mem_equal(const IPAddr& rhs) const {
            return memcmp(this, &rhs, sizeof(rhs)) == 0;
        }
        
        void map_v4(in_addr addr_) {
            map_v4(addr_.s_addr);
        }
        
        // 将IPv4地址映射为IPv4-mapped IPv6格式
        // 设计说明：IPv4-mapped IPv6格式允许IPv4地址在IPv6系统中表示
        void map_v4(uint32_t nl) {
            addr._in_addr_field[0] = 0x00000000;
            addr._in_addr_field[1] = 0x00000000;
            addr._in_addr_field[2] = htonl(0xffff);
            addr._in_addr_field[3] = nl;
        }
    };

    static_assert(sizeof(IPAddr) == 16, "IPAddr size不正确");

    /**
     * @brief 端点结构体
     * 
     * 表示网络端点，包含IP地址和端口号
     * 设计说明：
     * - 用于标识网络连接的两端
     * - 提供多种构造方式和解析方法
     */
    struct __attribute__ ((packed)) EndPoint {
        IPAddr addr;      // IP地址
        uint16_t port = 0; // 端口号
        
        EndPoint() = default;
        explicit EndPoint(const char* ep);  // 从字符串解析端点
        EndPoint(IPAddr ip, uint16_t port) : addr(ip), port(port) {}
        EndPoint(const char* ip, uint16_t port) : addr(ip), port(port) {}
        
        /**
         * @brief 解析端点字符串
         * 
         * 从字符串解析端点，支持IP:端口格式
         * 
         * @param ep 端点字符串
         * @param default_port 默认端口号
         * @return 解析后的端点
         */
        static EndPoint parse(std::string_view ep, uint16_t default_port);
        
        bool is_ipv4() const {
            return addr.is_ipv4();
        };
        
        bool operator==(const EndPoint& rhs) const {
            return rhs.addr == addr && rhs.port == port;
        }
        
        bool operator!=(const EndPoint& rhs) const {
            return !operator==(rhs);
        }
        
        bool undefined() const {
            return addr.undefined() && port == 0;
        }
        
        void reset() {
            addr.reset();
            port = 0;
        }
        
        void clear() {
            reset();
        }
    };

    static_assert(sizeof(EndPoint) == 18, "Endpoint大小不正确");

    // 用于记录IP地址的操作符
    LogBuffer& operator << (LogBuffer& log, const IPAddr& addr);
    LogBuffer& operator << (LogBuffer& log, const EndPoint& ep);

    /**
     * @brief 套接字基础接口
     * 
     * 定义套接字的基础操作，包括套接字选项设置和获取
     * 设计说明：
     * - 提供获取底层对象和文件描述符的方法
     * - 提供类型安全的套接字选项设置和获取模板方法
     */
    class ISocketBase {
    public:
        virtual ~ISocketBase() = default;

        virtual Object* get_underlay_object(uint64_t recursion = 0) = 0;
        
        /**
         * @brief 获取底层文件描述符
         * 
         * @return 文件描述符，失败返回-1
         */
        int get_underlay_fd() {
            auto ret = get_underlay_object(-1);
            return ret ? (int) (uint64_t) ret : -1;
        }

        virtual int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) = 0;
        virtual int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) = 0;

        /**
         * @brief 设置套接字选项（类型安全模板）
         * 
         * 模板方法，提供类型安全的套接字选项设置
         * 
         * @tparam P 选项值类型
         * @tparam T 传入值类型
         * @param level 套接字级别
         * @param option_name 选项名
         * @param value 选项值
         * @return 操作结果
         */
        template<typename P, typename T> // 必须显式指定P类型！
        int setsockopt(int level, int option_name, const T& value) {
            P v = value;
            return setsockopt(level, option_name, &v, sizeof(v));
        }

        /**
         * @brief 获取套接字选项（类型安全模板）
         * 
         * 模板方法，提供类型安全的套接字选项获取
         * 
         * @tparam P 选项值类型
         * @tparam T 传出值类型
         * @param level 套接字级别
         * @param option_name 选项名
         * @param value 用于接收选项值的指针
         * @return 操作结果
         */
        template<typename P, typename T> // 必须显式指定P类型！
        int getsockopt(int level, int option_name, T* value) {
            P v;
            socklen_t len = sizeof(v);
            int ret = getsockopt(level, option_name, &v, &len);
            if (ret >= 0) *value = v;
            return ret;
        }
    };

    /**
     * @brief 套接字命名接口
     * 
     * 定义获取套接字本地和对端地址的方法
     * 设计说明：
     * - 提供获取本地和对端地址的多种方法
     * - 支持IPv4/IPv6和Unix域套接字地址获取
     */
    class ISocketName {
    public:
        virtual ~ISocketName() = default;
        virtual int getsockname(EndPoint& addr) = 0;
        virtual int getpeername(EndPoint& addr) = 0;
        virtual int getsockname(char* path, size_t count) = 0;
        virtual int getpeername(char* path, size_t count) = 0;
        
        // 便捷方法，直接返回端点
        EndPoint getsockname() { EndPoint ep; getsockname(ep); return ep; }
        EndPoint getpeername() { EndPoint ep; getpeername(ep); return ep; }
    };

    /**
     * @brief 套接字流接口
     * 
     * 继承自IStream、ISocketBase和ISocketName，提供套接字流操作
     * 设计说明：
     * - 支持收发数据的多种方式（单缓冲区、向量I/O）
     * - 提供接收至少N字节的便捷方法
     * - 支持sendfile零拷贝传输
     */
    class ISocketStream : public IStream, public ISocketBase, public ISocketName {
    public:
        static const int ZEROCOPY_FLAG = 0x4000000;  // 零拷贝标志
        
        // 从套接字接收一些字节
        // 返回实际接收的字节数，可能小于count
        // 当套接字中没有数据时，最多阻塞一次
        virtual ssize_t recv(void *buf, size_t count, int flags = 0) = 0;
        virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) = 0;

        // 接收至少`least`字节到缓冲区(buf, count)
        ssize_t recv_at_least(void* buf, size_t count, size_t least, int flags = 0);
        ssize_t recv_at_least_mutable(struct iovec *iov, int iovcnt, size_t least, int flags = 0);

        // 读取并丢弃count字节
        // 成功返回true，失败返回false
        bool skip_read(size_t count);

        // 发送一些字节到套接字
        // 返回实际发送的字节数，可能小于count
        // 当套接字内部缓冲区没有空闲空间时，最多阻塞一次
        virtual ssize_t send(const void *buf, size_t count, int flags = 0) = 0;
        virtual ssize_t send(const struct iovec *iov, int iovcnt, int flags = 0) = 0;

        virtual ssize_t sendfile(int in_fd, off_t offset, size_t count) = 0;
    };

    /**
     * @brief 套接字客户端接口
     * 
     * 提供连接到远程端点的客户端功能
     * 设计说明：
     * - 支持连接到网络端点或Unix域套接字
     * - 提供连接超时设置功能
     */
    class ISocketClient : public ISocketBase, public Object {
    public:
        // 连接到远程端点
        // 如果local端点不为空，则在连接到remote之前将绑定其地址到套接字
        virtual ISocketStream* connect(const EndPoint& remote, const EndPoint* local = nullptr) = 0;
        
        // 连接到Unix域套接字
        virtual ISocketStream* connect(const char* path, size_t count = 0) = 0;

        virtual uint64_t timeout() const = 0;
        virtual void timeout(uint64_t) = 0;
    };

    /**
     * @brief 套接字服务器接口
     * 
     * 提供监听和接受连接的服务器功能
     * 设计说明：
     * - 支持绑定到不同类型的地址（IPv4/IPv6/Unix）
     * - 提供便捷的绑定方法
     * - 支持设置连接处理器和启动事件循环
     */
    class ISocketServer : public ISocketBase, public ISocketName, public Object {
    public:
        virtual int bind(const EndPoint& ep) = 0;
        virtual int bind(const char* path, size_t count) = 0;
        int bind(uint16_t port = 0)       { return bind_v4any(port); }
        int bind(uint16_t port, IPAddr a) { return bind(EndPoint(a, port)); }
        int bind_v4any(uint16_t port = 0) { return bind(EndPoint(IPAddr::V4Any(), port)); }
        int bind_v6any(uint16_t port = 0) { return bind(EndPoint(IPAddr::V6Any(), port)); }
        int bind_v4localhost(uint16_t port = 0) { return bind(EndPoint(IPAddr::V4Loopback(), port)); }
        int bind_v6localhost(uint16_t port = 0) { return bind(EndPoint(IPAddr::V6Loopback(), port)); }
        int bind(const char* path) { return bind(path, strlen(path)); }

        virtual int listen(int backlog = 1024) = 0;
        virtual ISocketStream* accept(EndPoint* remote_endpoint = nullptr) = 0;

        using Handler = Callback<ISocketStream*>;  // 连接处理器类型
        virtual ISocketServer* set_handler(Handler handler) = 0;
        virtual int start_loop(bool block = false) = 0;
        // 关闭监听文件描述符。用户有责任关闭活动连接
        virtual void terminate() = 0;

        virtual uint64_t timeout() const = 0;
        virtual void timeout(uint64_t) = 0;
    };

    // 创建各种套接字客户端和服务器的工厂函数
    extern "C" ISocketClient* new_tcp_socket_client(IPAddr* bind_ip = nullptr, uint32_t bind_ip_n = 0);
    extern "C" ISocketServer* new_tcp_socket_server();
    extern "C" ISocketClient* new_uds_client();
    extern "C" ISocketServer* new_uds_server(bool autoremove = false);
    extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t expiration = -1UL,
                                                  bool client_ownership = false);

    extern "C" ISocketClient* new_zerocopy_tcp_client();
    extern "C" ISocketServer* new_zerocopy_tcp_server();
    extern "C" ISocketClient* new_iouring_tcp_client();
    extern "C" ISocketServer* new_iouring_tcp_server();
    extern "C" int et_poller_init();  // 边缘触发轮询器初始化
    extern "C" int et_poller_fini();  // 边缘触发轮询器清理
    extern "C" ISocketClient* new_et_tcp_socket_client();  // 边缘触发TCP套接字客户端
    extern "C" ISocketServer* new_et_tcp_socket_server();  // 边缘触发TCP套接字服务器
    extern "C" ISocketClient* new_smc_socket_client();
    extern "C" ISocketServer* new_smc_socket_server();
    extern "C" ISocketClient* new_fstack_dpdk_socket_client();
    extern "C" ISocketServer* new_fstack_dpdk_socket_server();

    // 已弃用的函数，保留向后兼容性
    [[deprecated("deprecated since v0.8; use new_tcp_socket_client() instead;")]]
    inline ISocketClient* new_tcp_socket_client_ipv6() {
        return new_tcp_socket_client();
    }

    [[deprecated("deprecated since v0.8; use new_tcp_socket_server() instead;")]]
    inline ISocketServer* new_tcp_socket_server_ipv6() {
        return new_tcp_socket_server();
    }
}
}

namespace std {

// 为EndPoint和IPAddr提供哈希函数，使其可以在哈希容器中使用
template<>
struct hash<photon::net::EndPoint> {
    size_t operator()(const photon::net::EndPoint& x) const {
        hash<std::string_view> hasher;
        return hasher(std::string_view((const char*) &x, sizeof(x)));
    }
};

template<>
struct hash<photon::net::IPAddr> {
    size_t operator()(const photon::net::IPAddr& x) const {
        hash<std::string_view> hasher;
        return hasher(std::string_view((const char*) &x, sizeof(x)));
    }
};

}