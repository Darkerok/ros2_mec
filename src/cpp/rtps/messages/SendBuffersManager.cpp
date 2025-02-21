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
 * @file SendBuffersManager.cpp
 */

#include "SendBuffersManager.hpp"
#include "../participant/RTPSParticipantImpl.h"

namespace eprosima {
namespace fastrtps {
namespace rtps {
/**
 * size_t reserved_size：
 * 这是一个无符号整型参数，用于指定在创建SendBuffersManager对象时，其内部数据结构（pool_）应预留的空间大小。
 * 这有助于减少后续插入元素时的内存重新分配次数，提高性能。
 * bool allow_growing：
 * 布尔类型的参数，决定pool_是否可以在超过预留空间后继续动态增长。
 * 如果true，表示允许增长；如果false，则在达到预留空间后，插入新元素可能会失败或抛出异常。
 * 是否允许pool_容器在需要时扩展其容量。
 * 
 * 取参与者最大消息大小作为payload->对其之后赋值给advance->advance *= 2->创建common_buffer_，大小为advance * (pool_.capacity() - n_created_)->
 * 在缓冲区池pool_中添加RTPSMessageGroup_t（大小是advance）
*/
SendBuffersManager::SendBuffersManager(
        size_t reserved_size,
        bool allow_growing)
    : allow_growing_(allow_growing)
{
    //预先分配足够的内存来存储reserved_size个元素。
    //reserve是vector容器的方法，用于预先分配足够的内存来存储元素。
    pool_.reserve(reserved_size);
}

//接收一个指向RTPSParticipantImpl实例的指针作为参数，这个实例代表了实时发布/订阅传输协议（RTPS）中的一个参与者。
void SendBuffersManager::init(
        const RTPSParticipantImpl* participant)
{
    // std::cout<<"---初始化发送缓冲区池---"<<std::endl;
    //使用std::lock_guard和内部维护的互斥锁mutex_确保初始化过程的线程安全，防止多线程同时访问导致的数据竞争问题。
    std::lock_guard<std::mutex> guard(mutex_);
    //capacity()返回当前容量，n_created_是已创建的缓冲区数量。
    if (n_created_ < pool_.capacity())
    {
        // std::cout<<"n_created_:"<<n_created_<<std::endl<<"pool_.capacity():"<<pool_.capacity()<<std::endl;
        //guidPrefix 是GUID的一部分，用于标识特定的实体，如参与者、主题或发布者等
        const GuidPrefix_t& guid_prefix = participant->getGuid().guidPrefix;

        // Single allocation for the data of all the buffers.
        // We align the payload size to the size of a pointer, so all buffers will
        // be aligned as if directly allocated. 
        //处理对齐
        constexpr size_t align_size = sizeof(octet*) - 1;
        //获取参与者最大消息大小作为payload_size
        uint32_t payload_size = participant->getMaxMessageSize();
        assert(payload_size > 0u);
        //将payload_size对齐到align_size的倍数
        payload_size = (payload_size + align_size) & ~align_size;
        size_t advance = payload_size;
#if HAVE_SECURITY
        bool secure = participant->is_secure();
        advance *= secure ? 3 : 2;
#else
        advance *= 2;
#endif
        size_t data_size = advance * (pool_.capacity() - n_created_);
        //common_buffer_的大小调整为data_size，并将所有元素的值设置为0
        common_buffer_.assign(data_size, 0);
        //获取common_buffer_中数据的原始指针
        octet* raw_buffer = common_buffer_.data();
        while(n_created_ < pool_.capacity())
        {
            pool_.emplace_back(new RTPSMessageGroup_t(
                raw_buffer,
#if HAVE_SECURITY
                secure,
#endif
                payload_size, guid_prefix
            ));
            raw_buffer += advance;
            ++n_created_;
        }
    }
}

std::unique_ptr<RTPSMessageGroup_t> SendBuffersManager::get_buffer(
        const RTPSParticipantImpl* participant)
{
    std::unique_lock<std::mutex> lock(mutex_);

    std::unique_ptr<RTPSMessageGroup_t> ret_val;

    while (pool_.empty())
    {
        if (allow_growing_ || n_created_ < pool_.capacity())
        {
            add_one_buffer(participant);
        }
        else
        {
            logInfo(RTPS_PARTICIPANT, "Waiting for send buffer");
            available_cv_.wait(lock);
        }
    }
    //从池中获取并移除最后一个缓冲区，并将其返回
    //注意：这个函数假设pool_容器中至少有一个元素
    ret_val = std::move(pool_.back());
    pool_.pop_back();

    return ret_val;
}

void SendBuffersManager::return_buffer(
        std::unique_ptr <RTPSMessageGroup_t>&& buffer)
{
    std::lock_guard<std::mutex> guard(mutex_);
    pool_.push_back(std::move(buffer));
    available_cv_.notify_one();
}

void SendBuffersManager::add_one_buffer(
        const RTPSParticipantImpl* participant)
{
    RTPSMessageGroup_t* new_item = new RTPSMessageGroup_t(
#if HAVE_SECURITY
        participant->is_secure(),
#endif
        participant->getMaxMessageSize(), participant->getGuid().guidPrefix);
    pool_.emplace_back(new_item);
    ++n_created_;
}

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */
