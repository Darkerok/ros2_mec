// Copyright 2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file SendBuffersManager.hpp
 */

#ifndef RTPS_MESSAGES_SENDBUFFERSMANAGER_HPP
#define RTPS_MESSAGES_SENDBUFFERSMANAGER_HPP
#ifndef DOXYGEN_SHOULD_SKIP_THIS_PUBLIC

#include "RTPSMessageGroup_t.hpp"
#include <fastdds/rtps/common/GuidPrefix_t.hpp>

#include <vector>              // std::vector
#include <memory>              // std::unique_ptr
#include <mutex>               // std::mutex
#include <condition_variable>  // std::condition_variable


namespace eprosima {
namespace fastrtps {
namespace rtps {

class RTPSParticipantImpl;

/**
 * Manages a pool of send buffers.
 * @ingroup WRITER_MODULE
 */
//SendBuffersManager通过精细管理缓冲区的生命周期，旨在提升数据发送过程中的性能和资源利用率，特别是在高并发实时通信场景下。
class SendBuffersManager
{
public:

    /**
     * Construct a SendBuffersManager.
     * @param reserved_size Initial size for the pool.
     * 池的初始容量，即预先分配的缓冲区数量。
     * 
     * @param allow_growing Whether we allow creation of more than reserved_size elements.
     * 布尔值，指示是否允许缓冲区池在达到初始容量后继续扩容。
     */
    SendBuffersManager(
            size_t reserved_size,
            bool allow_growing);

        //确保所有创建的缓冲区都已正确归还到池中，通过断言检查
    ~SendBuffersManager()
    {
        assert(pool_.size() == n_created_);
    }

    /**
     * Initialization of pool.
     * Fills the pool to its reserved capacity.
     * @param participant Pointer to the participant creating the pool.
     * 指向创建缓冲区池的参与者的指针，可能用于日志记录或资源跟踪。
     */
    //根据reserved_size填充缓冲区池，为每个缓冲区分配资源，并可能利用common_buffer_来实现数据共享或优化内存使用。
    void init(
            const RTPSParticipantImpl* participant);

    /**
     * Get one buffer from the pool.
     * @param participant Pointer to the participant asking for a buffer.
     * 请求缓冲区的参与者指针，同样用于日志或跟踪
     * @return unique pointer to a send buffer.
     * 指向RTPSMessageGroup_t类型的唯一指针，表示一个可发送的数据包或消息组。
     */
    //从缓冲区池中取出一个可用的缓冲区供发送操作使用。若缓冲区不足且允许扩容，调用add_one_buffer增加新的缓冲区
    std::unique_ptr<RTPSMessageGroup_t> get_buffer(
            const RTPSParticipantImpl* participant);

    /**
     * Return one buffer to the pool.
     * @param buffer unique pointer to the buffer being returned.
     * 待归还的缓冲区的唯一指针。
     */
    //将使用完毕的缓冲区归还至池中，使其可供后续发送操作重用。采用右值引用实现高效转移所有权。
    void return_buffer(
            std::unique_ptr <RTPSMessageGroup_t>&& buffer);

private:
        //内部使用，当需要时向缓冲区池添加一个新的缓冲区。
        //participant，用于标识添加缓冲区的参与者。
    void add_one_buffer(
            const RTPSParticipantImpl* participant);

    //!Protects all data
    //互斥锁，保护并发访问的线程安全。
    std::mutex mutex_;
    //!Send buffers pool
    //存储RTPSMessageGroup_t类型智能指针的向量，代表缓冲区池。
    std::vector<std::unique_ptr<RTPSMessageGroup_t>> pool_;
    //!Raw buffer shared by the buffers created inside init()
    //一个共享的字节向量，可能用于减少内存分配，提高效率。
    std::vector<octet> common_buffer_;
    //!Creation counter
    //记录创建的缓冲区总数。
    std::size_t n_created_ = 0;
    //!Whether we allow n_created_ to grow beyond the pool_ capacity.
    //标记是否允许缓冲区池动态扩容。
    bool allow_growing_ = true;
    //!To wait for a buffer to be returned to the pool.
    //条件变量，用于同步，当缓冲区池为空时，可以让等待的线程在此等待直到有缓冲区被归还。
    std::condition_variable available_cv_;
};

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */

#endif

#endif // RTPS_MESSAGES_SENDBUFFERSMANAGER_HPP
