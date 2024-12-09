#ifndef _RTPS_FLOWCONTROL_FLOWCONTROLLERIMPL_HPP_
#define _RTPS_FLOWCONTROL_FLOWCONTROLLERIMPL_HPP_

#include "FlowController.hpp"
#include <fastdds/rtps/common/Guid.h>
#include <fastdds/rtps/writer/RTPSWriter.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <random>

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace eprosima {
namespace fastdds {
namespace rtps {


/** Auxiliary classes **/

struct FlowQueue
{
    FlowQueue() noexcept = default;

    ~FlowQueue() noexcept
    {
        assert(new_interested_.is_empty());
        assert(old_interested_.is_empty());
    }

    FlowQueue(
            FlowQueue&& old) noexcept
    {
        swap(std::move(old));
    }

    FlowQueue& operator =(
            FlowQueue&& old) noexcept
    {
        swap(std::move(old));
        return *this;
    }

    void swap(
            FlowQueue&& old) noexcept
    {
        new_interested_.swap(old.new_interested_);
        old_interested_.swap(old.old_interested_);

        new_ones_.swap(old.new_ones_);
        old_ones_.swap(old.old_ones_);
    }

    bool is_empty() const noexcept
    {
        return new_ones_.is_empty() && old_ones_.is_empty();
    }

    ///////////////////////////////////////////////
    /**
     * Return whether the time-base queue is empty
    */

   //常量成员函数，用于检查时间队列是否为空。
    bool is_time_queue_empty() const noexcept
    {
        return new_ones_.is_time_empty() && old_ones_.is_time_empty();
    }

    /**
     * The new_ones of this FlowQueue are added to time_queue in ascending order of 
     * remaining transmission time
    */

    //成员函数，用于将当前队列的新元素添加到另一个时间队列中
    void add_into_time_queue(FlowQueue& time_queue)
    {
        new_ones_.add_into_time_list(time_queue.new_ones_);
    }

    ///////////////////////////////////////////////

    void add_new_sample(
            fastrtps::rtps::CacheChange_t* change) noexcept
    {
        new_interested_.add_change(change);
    }

    void add_old_sample(
            fastrtps::rtps::CacheChange_t* change) noexcept
    {
        old_interested_.add_change(change);
    }

    fastrtps::rtps::CacheChange_t* get_next_change() noexcept
    {
        // std::cout<<"原始的fastrtps::rtps::CacheChange_t* get_next_change()"<<std::endl;
        if (!is_empty())
        {
            return !new_ones_.is_empty() ?
                   new_ones_.head.writer_info.next : old_ones_.head.writer_info.next;
        }

        return nullptr;
    }

     ///////////////////////////////////////////////
    /**
     * Get the next change of the time-based queue
    */
   //用于获取时间队列中的下一个change
    fastrtps::rtps::CacheChange_t* get_down_change() noexcept
    {
        if (!is_time_queue_empty())
        {
            return !new_ones_.is_time_empty() ?
                new_ones_.earlier.writer_info.later : old_ones_.earlier.writer_info.later;
        }

        return nullptr;
    }
    ///////////////////////////////////////////////

    void add_interested_changes_to_queue() noexcept
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        new_ones_.add_list(new_interested_);
        old_ones_.add_list(old_interested_);
    }

    ///////////////////////////////////////////////
    /**
     * Get size of the time-based queue
    */
    //获取时间队列的大小
    int get_time_size()
    {
        return new_ones_.list_time_size();
    }

    /**
     * Get size of the priority-based queue
    */
   //获取队列的大小
    int get_size()
    {
        return new_ones_.list_size();
    }
    int get_old_size()
    {
        return old_ones_.list_size();
    }
    //打印时间队列元素的相关信息
    void print_time_size()
    {
        new_ones_.print_time_list();
    }
    //计算链表中所有节点的平均传输时间
    long double calc_ave_trans_time()
    {
        return new_ones_.calc_list_ave_trans_time();
    }
    //计算时间敏感链表中的所有节点的平均传输时间
    long double calc_time_sensitive_ave_trans_time()
    {
        return new_ones_.calc_time_sensitive_ave_trans_time();
    }
    //将头指向尾，将尾指向头，将优先队列和时间队列都置为空
    void clear_time()
    {
        new_ones_.clear();
    }
    ///////////////////////////////////////////////

private:

    struct ListInfo
    {
        ListInfo() noexcept
        {
            clear();
        }

        void swap(
                ListInfo& other) noexcept
        {
            if (other.is_empty())
            {
                clear();
            }
            else
            {
                head.writer_info.next = other.head.writer_info.next;
                tail.writer_info.previous = other.tail.writer_info.previous;
                other.clear();
                head.writer_info.next->writer_info.previous = &head;
                tail.writer_info.previous->writer_info.next = &tail;
            }
        }

        void clear() noexcept
        {
            head.writer_info.next = &tail;
            tail.writer_info.previous = &head;
            ////////////////////////////////////
            // clear time-based list
            earlier.writer_info.later = &later;
            later.writer_info.earlier = &earlier;
            ////////////////////////////////////
        }

        bool is_empty() const noexcept
        {
            assert((&tail == head.writer_info.next && &head == tail.writer_info.previous) ||
                    (&tail != head.writer_info.next && &head != tail.writer_info.previous));
            return &tail == head.writer_info.next;
        }

        ////////////////////////////////////
        /**
         * Return whether the time-base list is empty
        */
        //检查当前 ListInfo 对象的时间队列是否为空
        bool is_time_empty() const noexcept
        {
            assert((&later == earlier.writer_info.later && &earlier == later.writer_info.earlier) ||
                (&later != earlier.writer_info.later && &earlier != later.writer_info.earlier));
            return &later == earlier.writer_info.later;
        }
        ////////////////////////////////////

        void add_change(
                fastrtps::rtps::CacheChange_t* change) noexcept
        {
            change->writer_info.previous = tail.writer_info.previous;
            change->writer_info.previous->writer_info.next = change;
            tail.writer_info.previous = change;
            change->writer_info.next = &tail;
        }

        void add_list(
                ListInfo& list) noexcept
        {
            if (!list.is_empty())
            {
                fastrtps::rtps::CacheChange_t* first = list.head.writer_info.next;
                fastrtps::rtps::CacheChange_t* last = list.tail.writer_info.previous;

                first->writer_info.previous = tail.writer_info.previous;
                first->writer_info.previous->writer_info.next = first;
                last->writer_info.next = &tail;
                tail.writer_info.previous = last;

                list.clear();
            }
        }

        long double calculateScore(long double p, long double t, long double k, long double epsilon)
        {
            p = ((10 - p) / 20.0);
            // p = std::max(0.0, std::min(p, 1.0)) + epsilon;
            // t = std::max(t, 0.0) + epsilon;
            p = p + epsilon;
            t = t + epsilon;

            long double logisticcinput = k * (std::log(p) + 1/t);

            long double u = 1.0/(1.0 + std::exp(-logisticcinput));

            return u;
        }

        ////////////////////////////////////
        /**
         * Inserts the elements of the current list into the passed argument list 
         * in ascending order of time remaining
        */
        void add_into_time_list(ListInfo& list)
        {
            // 从head到tail遍历，插入排序，添加到list中
            fastrtps::rtps::CacheChange_t* temp = head.writer_info.next;
            while (temp != &tail)
            {
                // auto t = eprosima::fastrtps::rtps::Time_t(0.6);
                // if(temp->priority==1) temp->sourceTimestamp = temp->sourceTimestamp - t;
                // std::cout << "... adding msg no." << temp->sequenceNumber << "\t ts: " << temp->sourceTimestamp 
				//     << "\t remain+now: " << temp->sourceTimestamp + temp->maxTransportTime << std::endl;
                // list.earlier.writer_info.later
                // 时间优先队列空队列
                // long double now = 10020; // 获取当前时间
                eprosima::fastrtps::rtps::Time_t now; 
                //使用 Time_t::now() 方法设置当前时间到 now 变量
                eprosima::fastrtps::rtps::Time_t::now(now);
                // long double t_remain = (temp->maxTransportTime + (now - temp->sourceTimestamp)).to_ns() * 1000000;
                // long double urgent_score = calculateScore(temp->priority, t_remain, 1.0 , 1e-6);
                // temp->urgent_score = urgent_score;
                if(temp->priority == 1)
                {
                    temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns() * 0.6;
                }
                else if(temp->priority == 0)
                {
                    temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns() * 0.6429;
                }
                else if(temp->priority == 6)
                {
                    temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns() * 0.8571;
                }
                // temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns();
                if (list.earlier.writer_info.later == &list.later && list.later.writer_info.earlier == &list.earlier)
                {
                    temp->writer_info.earlier = list.later.writer_info.earlier;
                    temp->writer_info.earlier->writer_info.later = temp;
                    list.later.writer_info.earlier = temp;
                    temp->writer_info.later = &list.later;
                }
                else
                {
                    fastrtps::rtps::CacheChange_t* time_temp = list.earlier.writer_info.later;
                    // change找比它Treamin小的，并且temp不是最后一结点
                    //如果temp的时间属性大于等于time_temp的时间属性且不是最后一个节点，那么就一直向后推
                    while((temp->remain_time) >=
                            (time_temp->remain_time) && time_temp->writer_info.later != &list.later)
                    {
                        time_temp = time_temp->writer_info.later;
                    }
                    //直到找到一个time_temp符合条件，跳出上面的while循环，将temp节点加在这个time_temp节点的前面
                    if ((temp->remain_time) < 
                            (time_temp->remain_time))
                    {
                        temp->writer_info.earlier = time_temp->writer_info.earlier;
                        temp->writer_info.earlier->writer_info.later = temp;
                        time_temp->writer_info.earlier = temp;
                        temp->writer_info.later = time_temp;
                    }
                    // while((temp->urgent_score) <=
                    //         (time_temp->urgent_score) && time_temp->writer_info.later != &list.later)
                    // {
                    //     time_temp = time_temp->writer_info.later;
                    // }
                    // //直到找到一个time_temp符合条件，跳出上面的while循环，将temp节点加在这个time_temp节点的前面
                    // if ((temp->urgent_score) > 
                    //         (time_temp->urgent_score))
                    // {
                    //     temp->writer_info.earlier = time_temp->writer_info.earlier;
                    //     temp->writer_info.earlier->writer_info.later = temp;
                    //     time_temp->writer_info.earlier = temp;
                    //     temp->writer_info.later = time_temp;
                    // }
                    //没找到符合time_temp的temp节点，就插入到末尾的位置
                    else if (time_temp->writer_info.later == &list.later)
                    {
                        temp->writer_info.earlier = list.later.writer_info.earlier;
                        temp->writer_info.earlier->writer_info.later = temp;
                        list.later.writer_info.earlier = temp;
                        temp->writer_info.later = &list.later;
                    }
                    else
                    {
                        std::cout << "**** ERROR add_change_by_remain_time ****" << std::endl;
                    }
                }
                // list.print_time_list();
                // std::cout << "..........................." << std::endl;
                temp = temp->writer_info.next;
            }
        }

        /**
         * Get length of the priority-based list
        */
        int list_size()
        {
            int size = 0;
            fastrtps::rtps::CacheChange_t* temp = &head;
            while (temp->writer_info.next != &tail)
            {
                temp = temp->writer_info.next;
                size++;
            }
            return size;
        }

        /**
         * Get length of the time-based list
        */
        int list_time_size()
        {
            int size = 0;
            fastrtps::rtps::CacheChange_t* temp = &earlier;
            while (temp->writer_info.later != &later)
            {
                temp = temp->writer_info.later;
                size++;
            }
            return size;
        }

        /**
         * Print change of time-based list
        */
        void print_time_list()
        {
            int size = 0;
            fastrtps::rtps::CacheChange_t* temp = &earlier;
            while (temp->writer_info.later != &later)
            {
                temp = temp->writer_info.later;
                std::cout << "msg no." << temp->sequenceNumber << "\t ts: " << temp->sourceTimestamp 
				    << "\t remain+now: " << temp->sourceTimestamp + temp->maxTransportTime << std::endl;
                size++;
            }
        }

        /**
         * Returns the average waiting time for the current queue
        */
        long double calc_list_ave_trans_time()
        {
            int len = list_time_size();
            if (len == 0)
            {
                return 0;
            }
            else
            {
                long double trans_time_sum = 0.0;
                fastrtps::rtps::CacheChange_t* temp = &earlier;
                // long double now = 10020; // 获取当前时间
                eprosima::fastrtps::rtps::Time_t now; 
                //使用 Time_t::now() 方法设置当前时间到 now 变量
                eprosima::fastrtps::rtps::Time_t::now(now);
                while (temp->writer_info.later != &later)
                {
                    temp = temp->writer_info.later;
                    //(now - temp->sourceTimestamp).to_ns()：计算当前时间与节点时间戳的差值，并转换为纳秒
                    trans_time_sum += ((8 - temp->priority) / 8.0) * (now - temp->sourceTimestamp).to_ns(); 
                }
                return trans_time_sum / len;
            }
        }
        long double calc_time_sensitive_ave_trans_time()
        {
            int len = list_size();
            if(len == 0)
            {
                return 0;
            }
            else
            {
                long double trans_time_sum = 0.0;
                fastrtps::rtps::CacheChange_t* temp = &head;
                // long double now = 10020; // 获取当前时间
                eprosima::fastrtps::rtps::Time_t now;
                //使用 Time_t::now() 方法设置当前时间到 now 变量
                eprosima::fastrtps::rtps::Time_t::now(now);
                while(temp->writer_info.next != &tail)
                {
                    temp = temp->writer_info.next;
                    trans_time_sum += (now - temp->sourceTimestamp).to_ns();
                }
                return trans_time_sum / len;
            }
        }
        ////////////////////////////////////

        fastrtps::rtps::CacheChange_t head;
        fastrtps::rtps::CacheChange_t tail;
        fastrtps::rtps::CacheChange_t earlier;
        fastrtps::rtps::CacheChange_t later;
    };

    //! List of interested new changes to be included.
    //! Should be protected with changes_interested_mutex.
    ListInfo new_interested_;

    //! List of interested old changes to be included.
    //! Should be protected with changes_interested_mutex.
    ListInfo old_interested_;

    //! List of new changes
    //! Should be protected with mutex_.
    ListInfo new_ones_;

    //! List of old changes
    //! Should be protected with mutex_.
    ListInfo old_ones_;
};

/** Classes used to specify FlowController's publication model **/

//! Only sends new samples synchronously. There is no mechanism to send old ones.
struct FlowControllerPureSyncPublishMode
{

    FlowControllerPureSyncPublishMode(
            fastrtps::rtps::RTPSParticipantImpl*,
            const FlowControllerDescriptor*)
    {
    }

};

//! Sends new samples asynchronously. Old samples are sent also asynchronously */
struct FlowControllerAsyncPublishMode
{
    FlowControllerAsyncPublishMode(
            fastrtps::rtps::RTPSParticipantImpl* participant,
            const FlowControllerDescriptor* descriptor)
        : group(participant, true)
    {
        std::cout<<"FlowControllerAsyncPublishMode"<<std::endl;
        assert(nullptr != descriptor);
        
        assert(0 < descriptor->max_bytes_per_period);
        
        // max_bytes_per_period = descriptor->max_bytes_per_period;
        max_bytes_per_period = 8*1024*1024;
        
        // period_ms = std::chrono::milliseconds(descriptor->period_ms);
        period_ms = std::chrono::milliseconds(1000);
        group.set_sent_bytes_limitation(static_cast<uint32_t>(max_bytes_per_period));
        // std::cout<<max_bytes_per_period<<std::endl;
        // std::cout<<period_ms<<std::endl;
    }

    virtual ~FlowControllerAsyncPublishMode()
    {
        if (running)
        {
            {
                std::unique_lock<std::mutex> lock(changes_interested_mutex);
                running = false;
                cv.notify_one();
            }
            thread.join();
        }
    }

    bool fast_check_is_there_slot_for_change(
            fastrtps::rtps::CacheChange_t* change)
    {
        // return true;
        uint32_t size_to_check = change->serializedPayload.length;

        if (0 != change->getFragmentCount())
        {
            // For fragmented sample, the fast check is the minor fragments fit.
            size_to_check = change->serializedPayload.length % change->getFragmentSize();

            if (0 == size_to_check)
            {
                size_to_check = change->getFragmentSize();
            }


        }

        bool ret = (max_bytes_per_period - group.get_current_bytes_processed()) > size_to_check;

        if (!ret)
        {
            force_wait_ = true;
        }

        return ret;
    }

    bool wait(
            std::unique_lock<std::mutex>& lock)
    {
        // cv.wait(lock);
        // return false;
        auto lapse = std::chrono::steady_clock::now() - last_period_;
        bool reset_limit = true;

        if (lapse < period_ms)
        {
            if (std::cv_status::no_timeout == cv.wait_for(lock, period_ms - lapse))
            {
                reset_limit = false;
            }
        }

        if (reset_limit)
        {
            last_period_ = std::chrono::steady_clock::now();
            force_wait_ = false;
            group.reset_current_bytes_processed();
        }

        return reset_limit;
    }

    bool force_wait() const
    {
        // return false;
        return force_wait_;
    }

    void process_deliver_retcode(
            const fastrtps::rtps::DeliveryRetCode& ret_value)
    {
        if (fastrtps::rtps::DeliveryRetCode::EXCEEDED_LIMIT == ret_value)
        {
            force_wait_ = true;
        }
    }

    std::thread thread;

    std::atomic_bool running {false};

    std::condition_variable cv;

    fastrtps::rtps::RTPSMessageGroup group;

    //! Mutex for interested samples to be added.
    std::mutex changes_interested_mutex;

    //! Used to warning async thread a writer wants to remove a sample.
    std::atomic<uint32_t> writers_interested_in_remove = {0};

    int32_t max_bytes_per_period = 0;

    std::chrono::milliseconds period_ms;

private:

    bool force_wait_ = false;

    std::chrono::steady_clock::time_point last_period_ = std::chrono::steady_clock::now();
};

//! Sends new samples synchronously. Old samples are sent asynchronously */
struct FlowControllerSyncPublishMode : public FlowControllerPureSyncPublishMode, public FlowControllerAsyncPublishMode
{

    FlowControllerSyncPublishMode(
            fastrtps::rtps::RTPSParticipantImpl* participant,
            const FlowControllerDescriptor* descriptor)
        : FlowControllerPureSyncPublishMode(participant, descriptor)
        , FlowControllerAsyncPublishMode(participant, descriptor)
    {
    }

};

//! Sends all samples asynchronously but with bandwidth limitation.
struct FlowControllerLimitedAsyncPublishMode : public FlowControllerAsyncPublishMode
{
    FlowControllerLimitedAsyncPublishMode(
            fastrtps::rtps::RTPSParticipantImpl* participant,
            const FlowControllerDescriptor* descriptor)
        : FlowControllerAsyncPublishMode(participant, descriptor)
    {
        assert(nullptr != descriptor);
        assert(0 < descriptor->max_bytes_per_period);

        max_bytes_per_period = descriptor->max_bytes_per_period;
        period_ms = std::chrono::milliseconds(descriptor->period_ms);
        group.set_sent_bytes_limitation(static_cast<uint32_t>(max_bytes_per_period));
    }

    bool fast_check_is_there_slot_for_change(
            fastrtps::rtps::CacheChange_t* change)
    {
        // Not fragmented sample, the fast check is if the serialized payload fit.
        uint32_t size_to_check = change->serializedPayload.length;

        if (0 != change->getFragmentCount())
        {
            // For fragmented sample, the fast check is the minor fragments fit.
            size_to_check = change->serializedPayload.length % change->getFragmentSize();

            if (0 == size_to_check)
            {
                size_to_check = change->getFragmentSize();
            }


        }

        bool ret = (max_bytes_per_period - group.get_current_bytes_processed()) > size_to_check;

        if (!ret)
        {
            force_wait_ = true;
        }

        return ret;
    }

    /*!
     * Wait until there is a new change added (notified by other thread) or there is a timeout (period was excedded and
     * the bandwidth limitation has to be reset.
     *
     * @return false if the condition_variable was awaken because a new change was added. true if the condition_variable was awaken because the bandwidth limitation has to be reset.
     */
    bool wait(
            std::unique_lock<std::mutex>& lock)
    {
        auto lapse = std::chrono::steady_clock::now() - last_period_;
        bool reset_limit = true;

        if (lapse < period_ms)
        {
            if (std::cv_status::no_timeout == cv.wait_for(lock, period_ms - lapse))
            {
                reset_limit = false;
            }
        }

        if (reset_limit)
        {
            last_period_ = std::chrono::steady_clock::now();
            force_wait_ = false;
            group.reset_current_bytes_processed();
        }

        return reset_limit;
    }

    bool force_wait() const
    {
        return force_wait_;
    }

    void process_deliver_retcode(
            const fastrtps::rtps::DeliveryRetCode& ret_value)
    {
        if (fastrtps::rtps::DeliveryRetCode::EXCEEDED_LIMIT == ret_value)
        {
            force_wait_ = true;
        }
    }

    int32_t max_bytes_per_period = 0;

    std::chrono::milliseconds period_ms;

private:

    bool force_wait_ = false;

    std::chrono::steady_clock::time_point last_period_ = std::chrono::steady_clock::now();
};

//测量rtt
int getPingTime(const std::string& ip)
{
    std::ostringstream commandStream;
    commandStream << "ping -c 1 " << ip << " > ping_tmp.txt";
    int systemResult = std::system(commandStream.str().c_str());

    // 检查system命令是否成功执行
    if (systemResult != 0) {
        std::cerr << "Ping command failed with return code: " << systemResult << std::endl;
        return -1; // 或者根据你的需要返回其他错误码
    }

    std::ifstream file("ping_tmp.txt");
    if (!file.good()) {
        std::cerr << "Failed to open ping result file" << std::endl;
        return 0;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("time=") != std::string::npos) {
            int startIndex = line.find("time=") + 5;
            int endIndex = line.find(" ms", startIndex);
            // std::cout << startIndex << " " << endIndex << std::endl;
            return std::stoi(line.substr(startIndex, endIndex - startIndex));
        }
    }

    return 0;
}

float getPingTime1(const std::string& ip) {
    // 运行 ping 命令并捕获输出结果
    std::array<char, 128> buffer;
    std::string result;
    std::string command = "ping -c 1 " + ip + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");

    if (!pipe) {
        std::cerr << "无法运行 ping 命令。" << std::endl;
        return 0.0f;
    }

    // 读取 ping 命令的输出
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int returnCode = pclose(pipe);
    if (returnCode != 0) {
        std::cerr << "ping 命令失败，返回码：" << returnCode << std::endl;
        return 0.0f;
    }

    // 使用正则表达式提取 RTT 值
    // std::regex regex(R"(时间[=<](\d+\.?\d*)\s*毫秒)");
    std::regex regex("time=(\\d+\\.\\d+)\\s*ms");
    std::smatch match;
    if (std::regex_search(result, match, regex)) {
        return std::stof(match[1]);  // 提取并返回 RTT 值（单位：毫秒）
    } else {
        std::cerr << "未找到 RTT 值。" << std::endl;
        return 0.0f;
    }
}

/** Classes used to specify FlowController's sample scheduling **/

// ! Fifo scheduling
struct FlowControllerFifoSchedule
{
    //根据writer的优先级创建相应的优先级队列
    void register_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        assert(nullptr != writer);
        // std::cout<<"发布者的id为:"<<writer->getGuid().entityId<<std::endl;
        int priority = 10;
        // if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        // if(writer->getGuid().entityId.to_string()=="00.00.12.03")//仿真
        if(writer->getGuid().entityId.to_string()=="00.00.11.03")//部署
        {
            priority = 0;
        }
        // else if(writer->getGuid().entityId.to_string()=="00.00.12.03")
        // else if(writer->getGuid().entityId.to_string()=="00.00.13.03")//仿真
        else if(writer->getGuid().entityId.to_string()=="00.00.14.03")//部署
        {
            priority = 1;
        }
        // else if(writer->getGuid().entityId.to_string()=="00.00.11.03")//仿真
        else if(writer->getGuid().entityId.to_string()=="00.00.13.03")//部署
        {
            priority = 6;
        }
        //将writer和其优先级作为键值对插入到 priorities_ 映射中
        auto ret = priorities_.insert({writer, priority});
        (void)ret;
        assert(ret.second);

        // Ensure the priority is created.
        //这行代码还使用了引用（&），这意味着 queue 是对 writers_queue_[priority] 的引用，而不是它的副本。
        //这样做的好处是任何对 queue 的修改都会直接反映在 writers_queue_ 映射中存储的相应元素上。
        FlowQueue& queue = writers_queue_[priority];
        (void)queue;
    }

    void unregister_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        auto it = priorities_.find(writer);
        assert(it != priorities_.end());
        priorities_.erase(it);
    }

    void work_done() const
    {
        // std::cout<<"FlowControllerImpl::run()：流控制器调度完成"<<std::endl;
        // Do nothing
    }
    //向对应优先级的writer队列中添加change，并返回对应队列的长度
    void add_new_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        assert(nullptr != writer);
        int priority = 10;
        // if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        // if(writer->getGuid().entityId.to_string()=="00.00.12.03")//仿真
        if(writer->getGuid().entityId.to_string()=="00.00.11.03")//部署
        {
            priority = 0;
        }
        // else if(writer->getGuid().entityId.to_string()=="00.00.12.03")
        // else if(writer->getGuid().entityId.to_string()=="00.00.13.03")//仿真
        else if(writer->getGuid().entityId.to_string()=="00.00.14.03")//部署
        {
            priority = 1;
        }
        // else if(writer->getGuid().entityId.to_string()=="00.00.11.03")//仿真
        else if(writer->getGuid().entityId.to_string()=="00.00.13.03")//部署
        {
            priority = 6;
        }
        const char* freq_key_map = std::getenv("PUBLISH_key_map");
        if(freq_key_map)
        {
            std::cout<<"6优先级数据底层获取的发布周期:"<<std::stod(freq_key_map)<<std::endl;
        }
        const char* freq_time_sensitive = std::getenv("PUBLISH_time_sensitive");
        if(freq_time_sensitive)
        {
            std::cout<<"1优先级数据底层获取的发布周期:"<<std::stod(freq_time_sensitive)<<std::endl;
        }
        const char* freq_edge_output = std::getenv("PUBLISH_edge_output");
        if(freq_edge_output)
        {
            std::cout<<"0优先级数据底层获取的发布周期:"<<std::stod(freq_edge_output)<<std::endl;
        }
        // // const char* freq_edge_output_transmid = std::getenv("PUBLISH_edge_output_transmid");
        const char* freq_edge_time = std::getenv("PUBLISH_edge_time");
        // // if(freq_edge_output_transmid)
        // // {
        //     // std::cout<<"0优先级数据底层获取的推理数据传输要求时间:"<<std::stod(freq_edge_output_transmid)<<std::endl;
        // // }
        long double key_map_pubtime = std::stod(freq_key_map);
        long double time_sensitive_pubime = std::stod(freq_time_sensitive);
        long double edge_output_pubtime = std::stod(freq_edge_output);
        long double edge_time= std::stod(freq_edge_time);
        // long double edge_output_maxTransportTime = edge_output_pubtime - edge_time;
        long double edge_output_maxTransportTime = edge_output_pubtime;
        long double key_map_maxTransportTime = key_map_pubtime;
        // long double time_sensitive_maxTransportTime = std::min(time_sensitive_pubime, std::min(edge_output_maxTransportTime, key_map_maxTransportTime));
        long double time_sensitive_maxTransportTime = time_sensitive_pubime;
        // std::cout<< "优先级为"<<priority<<"的writer"<<writer->getGuid().entityId<<"向其FlowQueue中添加change seq_num: " << change->sequenceNumber << std::endl;
        //赋值change的最大传输时间和优先级(change的优先级就是所在writer优先级队列的优先级)
        switch (priority)
        {
            //共享地图数据
        case 6:
            change->maxTransportTime = eprosima::fastrtps::rtps::Time_t(key_map_maxTransportTime);
            std::cout<<"6优先级最大传输时间:"<<change->maxTransportTime<<std::endl;
            change->priority = 6;
            break;
            //时间敏感
        case 1:
            change->maxTransportTime = eprosima::fastrtps::rtps::Time_t(edge_output_maxTransportTime);
            std::cout<<"1优先级最大传输时间:"<<change->maxTransportTime<<std::endl;
            change->priority = 1;
            break;
            //关键任务
        case 0:
            change->maxTransportTime = eprosima::fastrtps::rtps::Time_t(time_sensitive_maxTransportTime);
            std::cout<<"0优先级最大传输时间:"<<change->maxTransportTime<<std::endl;
            change->priority = 0;
            break;
        default:
            break;
        }
        find_queue(writer).add_new_sample(change);
        //加入change的时候返回对应writer的队列长度
        if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        {
            int qulen = find_queue(writer).get_size();
            // std::cout<<"writer"<<writer->getGuid().entityId<<"的队列长度(fxq):"<<qulen<<std::endl;
        }
        // int qulen = find_queue(writer).get_size();
        // std::cout<<"writer"<<writer->getGuid().entityId<<"的队列长度(fxq):"<<qulen<<std::endl;
    }

    //没怎么用到
    void add_old_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        find_queue(writer).add_old_sample(change);
    }

    /*!
     * Returns the first sample in the queue.
     * Default behaviour.
     * Expects the queue is ordered.
     *
     * @return Pointer to next change to be sent. nullptr implies there is no sample to be sent or is forbidden due to
     * bandwidth exceeded.
     */
    //获取队列中的下一个change
    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        /*
            TODO：
            优先级优先队列已经通过writers_queue_排序好；
            仅需对当前所有的change按剩余时间排序即可，放入queue_中（仅保存排好序队列的top和bottom指针）
        */
        // 清空时间优先队列
        queue_.clear_time();

        //将优先级队列当中的数据添加（复制）到时间优先队列当中
        if (0 < writers_queue_.size())
        {
            // 优先获取并发送优先级较大的writer的消息，int越小对应writer优先级越大
            for (auto it = writers_queue_.begin(); it != writers_queue_.end(); ++it)
            {
                // 遍历second（flowqueue）中元素，添加到时间优先队列，将所有消息添加到时间优先队列中
                // std::cout<<"0优先级writer队列长度:"<<writers_queue_[0].get_size()<<std::endl;
                // std::cout<<"1优先级writer队列长度:"<<writers_queue_[1].get_size()<<std::endl;
                // std::cout<<"6优先级writer队列长度:"<<writers_queue_[6].get_size()<<std::endl;
                it->second.add_into_time_queue(queue_);
            }
        }
        //时间队列的长度
        int time_queue_len = queue_.get_time_size();
        // std::cout<<"时间队列的长度:"<<time_queue_len<<std::endl;
        fastrtps::rtps::CacheChange_t* temp = queue_.get_down_change();
        while(time_queue_len--)
        {
            std::cout<<"数据的优先级为："<<temp->priority<<std::endl;
            std::cout<<"数据进入队列时间+最大传输时间："<<temp->remain_time<<std::endl;
            temp = temp->writer_info.later;
        }
        //计算rtt，作为参数传入judge_switch()函数，函数中需要计算tmax，tmin等需要rtt
        // std::string ipAddress = "127.0.0.1";
        // long double pingTime = getPingTime(ipAddress);
        // std::cout<<"网络延迟RTT的大小为:"<<pingTime<<std::endl;
        //转换为ns
        long double pingTime = 0.045;
        // pingTime = getPingTime1("127.0.0.1");
        // std::cout<<"计算的RTT的值为:"<<pingTime<<"ms"<<std::endl;
        pingTime = pingTime * 1000000;
        judge_switch(pingTime); // 判断是否切换子系统
        /*
            TODO: 根据队列元素判断是否切换子系统/切换优先队列
            T, Tmax, Tmin通过计算所得，Tmax和Tmin 与当前的RTT有关，即当时网络延迟
            时间优先->优先级优先：false
                (T<Tmin切换为优先级优先子系统，但是如果时间敏感队列中的数据都还来得及，那么就会先传输关键任务数据，再传输时间敏感数据，再传输其他数据)
            优先级优先->时间优先：true
                （存在即将超时的消息，平均已传输时间T>Tmax）
        */
        fastrtps::rtps::CacheChange_t* ret_change = nullptr;
        //从时间优先队列中输出队首元素
        if (is_switch)
        {
            ret_change = queue_.get_down_change();
            is_time_max = false;
            // std::cout << "时间优先队列获得change" << std::endl;
        }
        else // 从优先级优先队列中输出队首元素
        {
            //关键任务队列中的change剩余传输时间不是很多，所以就优先级队列顺序去传输即可
            if (0 < writers_queue_.size() && is_keytask == false)
            {
                // 优先获取并发送优先级较大的writer的消息，int越小对应writer优先级越大
                for (auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
                {
                    ret_change = it->second.get_next_change();
                    // std::cout << "优先级优先队列获得change(0->1->6)"<< std::endl;
                }
                is_keytask = true;
            }
            //关键任务队列中的change剩余传输时间很充足，所以可以先传输时间敏感队列中的change，然后传输关键任务change，最后其他change
            else if(0 < writers_queue_.size() && is_keytask == true)
            {
                for(auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
                {
                    if(it->first == 1)
                    {
                        ret_change = it->second.get_next_change();
                        // std::cout << "优先级优先队列获得change(优先获得时间敏感(1->0->6))"<< std::endl;
                        is_keytask = true;
                    }
                }
                //如果关键任务队列中的数据是空的，就重新按优先级顺序从队列中获取change
                if(ret_change == nullptr)
                {
                    for(auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
                    {
                        ret_change = it->second.get_next_change();
                        // std::cout << "优先级优先队列获得change(优先获得关键任务(0->6))"<< std::endl;
                        is_keytask = true;
                    }
                }
            }
        }
        return ret_change;
    }

    /*!
     * Store the sample at the end of the list.
     *
     * @return true if there is added changes.
     */
    void add_interested_changes_to_queue_nts()
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        // std::cout<<"fifo策略：add_interested_changes_to_queue"<<std::endl;
        // queue_.add_interested_changes_to_queue();
        for (auto& queue : writers_queue_)
        {
            queue.second.add_interested_changes_to_queue();
        }
    }

    void switchQueue()
    {
        is_switch = !is_switch;
    }
    //计算tmax和tmin
    void calc_t_max_min(long double rtt)
    {
        fastrtps::rtps::CacheChange_t* ret_change = nullptr;
        long double tmp = 0;
        t_max = 0, t_min = 0;
        if (0 < writers_queue_.size())
        {
            for (auto it = writers_queue_.begin(); it != writers_queue_.end(); ++it)
            {
                ret_change = it->second.get_next_change();
                // std::cout<<it->first<<std::endl;
                if (ret_change != nullptr && it->first == 0)
                {
                    tmp = (1.0 * ret_change->maxTransportTime.to_ns() - rtt / 2)
                            * (1.0 * it->second.get_size() / queue_.get_time_size());
                    t_max += r_max_0 * tmp;
                    t_min += r_min_0 * tmp;
                }
                else if(ret_change != nullptr && it->first == 1)
                {
                    tmp = (1.0 * ret_change->maxTransportTime.to_ns() - rtt / 2)
                            * (1.0 * it->second.get_size() / queue_.get_time_size());
                    t_max += r_max_2 * tmp;
                    t_min += r_min_2 * tmp;
                }
                else if(ret_change != nullptr && it->first == 6)
                {
                    tmp = (1.0 * ret_change->maxTransportTime.to_ns() - rtt / 2)
                            * (1.0 * it->second.get_size() / queue_.get_time_size());
                    t_max += r_max_4 * tmp;
                    t_min += r_min_4 * tmp;
                }
            }
        }
    }
    //系统切换判断条件
    void judge_switch(long double rtt)
    {
        int time_queue_len = queue_.get_time_size();
        // std::cout<<"时间队列的长度:"<<time_queue_len<<std::endl;
        //时间队列长度为0,说明没有数据，就默认按照优先级优先的流程走一次
        //也是排除了ave_trans_time >= t_min && ave_trans_time =< t_max的可能，凑全集，因为只有等于0的时候才会相等
        if(time_queue_len == 0)
        {
            is_switch = false;
            return;
        }
        ave_trans_time = queue_.calc_ave_trans_time();
        calc_t_max_min(rtt);
        t_max = 0.0,t_min = 1000000000.0;
        // std::cout << "当前待发送队列的平均已传输时间为: " << ave_trans_time << std::endl;
        // std::cout << "当前待发送队列的t_max: " << t_max << ", t_min: " << t_min << std::endl;
        if (is_switch)
        {
            //当前是时间优先子系统，判断是否切换为优先级优先子系统
            //平均已等待时间 t小于剩余等待时间下界 tmin
            if (ave_trans_time > t_min && ave_trans_time < t_max)
            {
                is_switch = !is_switch;
                is_keytask = false;
                // std::cout << "### 时间优先子系统 ---> 优先级优先子系统 ###(tmin<avg<tmax)" << std::endl;
                return;
            }
            else if (ave_trans_time < t_min)
            {
                is_switch = !is_switch;
                // std::cout << "### 时间优先子系统 ---> 优先级优先子系统 ###(avg<tmin)" << std::endl;

                //继续判断关键任务剩余传输时间是不是剩的很多
                eprosima::fastrtps::rtps::Time_t now; 
                eprosima::fastrtps::rtps::Time_t::now(now);
                int temp_priority = 0;
                auto queue_0 = writers_queue_.find(temp_priority);
                int len = queue_0->second.get_size();
                // std::cout<<"0优先级的writer的队列长度:"<<len<<std::endl;
                fastrtps::rtps::CacheChange_t* queue_0_change = queue_0->second.get_next_change();
                while(len--)
                {
                    // std::cout<<"queue_0_change->sourceTimestamp:"<<(queue_0_change->sourceTimestamp).to_ns()<<" "<<"1.0 * queue_0_change->maxTransportTime.to_ns():"<<1.0 * queue_0_change->maxTransportTime.to_ns() - rtt/2<<std::endl;
                    //寻找是否有大于0.1倍的
                    if((now - queue_0_change->sourceTimestamp).to_ns() > r_1 * (1.0 * queue_0_change->maxTransportTime.to_ns() - rtt/2))
                    {
                        // std::cout<<"change"<<queue_0_change->writerGUID<<" "<<"的剩余传输时间:"<<(now - queue_0_change->sourceTimestamp).to_ns()<<"最大传输时间:"<<1.0 * queue_0_change->maxTransportTime.to_ns()<<std::endl;
                        is_keytask = false;
                        break;
                    }
                    queue_0_change = queue_0_change->writer_info.next;
                }
		        return;
            }
        }
        else
        {
            //判断平均已等待时间 t 大于剩余等待时间上界tmax
	        if (ave_trans_time > t_max)
            {
                is_switch = !is_switch;
                is_time_max = true;
                std::cout << "*** 优先级优先子系统 ---> 时间优先子系统***(avg>tmax) " << std::endl;
		        return;
            }
            //判断时间队列存在即将超时的消息
            else if(queue_.get_time_size() != 0)
            {
                fastrtps::rtps::CacheChange_t* temp = queue_.get_down_change();
                // std::cout<<"时间队列大小："<<queue_.get_time_size()<<std::endl;
                int time_len = queue_.get_time_size();
                eprosima::fastrtps::rtps::Time_t now; 
                eprosima::fastrtps::rtps::Time_t::now(now);
                //判断时间队列中是否存在即将超时的数据
                while(time_len--)
                {
                    // std::cout<<"(temp->sourceTimestamp).to_ns():"<<(temp->sourceTimestamp).to_ns()<<" "<<"1.0 * temp->maxTransportTime.to_ns():"<<1.0 * temp->maxTransportTime.to_ns() - rtt/2<<std::endl;
                    // std::cout<<"---------------------------------"<<temp<<std::endl;
                    if(temp->priority == 0)
                    {
                        if((now - temp->sourceTimestamp).to_ns() > r_0_0 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                        {
                            // std::cout<<"(now - temp->sourceTimestamp).to_ns():"<<(temp->sourceTimestamp).to_ns()<<" "<<"1.0 * temp->maxTransportTime.to_ns():"<<1.0 * temp->maxTransportTime.to_ns()<<std::endl;
                            is_switch = !is_switch;
                            std::cout << "*** 优先级优先子系统 ---> 时间优先子系统 ***(0优先级消息即将超时)" << std::endl;
                            return;
                        }
                    }
                    else if(temp->priority == 1)
                    {
                        if((now - temp->sourceTimestamp).to_ns() > r_0_2 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                        {
                            // std::cout<<"(now - temp->sourceTimestamp).to_ns():"<<(temp->sourceTimestamp).to_ns()<<" "<<"1.0 * temp->maxTransportTime.to_ns():"<<1.0 * temp->maxTransportTime.to_ns()<<std::endl;
                            is_switch = !is_switch;
                            std::cout << "*** 优先级优先子系统 ---> 时间优先子系统 ***(1优先级消息即将超时)" << std::endl;
                            return;
                        }
                    }
                    else if(temp->priority == 6)
                    {
                        if((now - temp->sourceTimestamp).to_ns() > r_0_4 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                        {
                            // std::cout<<"(now - temp->sourceTimestamp).to_ns():"<<(temp->sourceTimestamp).to_ns()<<" "<<"1.0 * temp->maxTransportTime.to_ns():"<<1.0 * temp->maxTransportTime.to_ns()<<std::endl;
                            is_switch = !is_switch;
                            std::cout << "*** 优先级优先子系统 ---> 时间优先子系统 ***(6优先级消息即将超时)" << std::endl;
                            return;
                        }
                    }
                    temp = temp->writer_info.later;
                }
            }
        }
    }

    void set_bandwith_limitation(
            uint32_t) const
    {
    }

    void trigger_bandwidth_limit_reset() const
    {
    }

private:
    FlowQueue& find_queue(fastrtps::rtps::RTPSWriter* writer)
    {
        // Find priority.
        auto priority_it = priorities_.find(writer);
        assert(priority_it != priorities_.end());
        auto queue_it = writers_queue_.find(priority_it->second);
        assert(queue_it != writers_queue_.end());
        return queue_it->second;
    }
    //! Scheduler queue. FIFO scheduler only has one queue.
    FlowQueue queue_;

    std::map<int, FlowQueue> writers_queue_;
    std::unordered_map<fastrtps::rtps::RTPSWriter*, int> priorities_;

    bool is_switch = false;
    bool is_keytask = true;
    bool is_time_max = false;
    long double ave_trans_time = 0.0;
    long double t_max = 0.0;
    long double t_min = 0.0;
    const double r_max_0 = 0.6429;
    const double r_min_0 = 0.3571;
    const double r_max_2 = 0.6;
    const double r_min_2 = 0.4;
    const double r_max_4 = 0.8571;
    const double r_min_4 = 0.1429;
    const double r_0_0 = 0.6429;//即将超时系数
    const double r_0_2 = 0.6;//即将超时系数
    const double r_0_4 = 0.8571;//即将超时系数
    const double r_1 = 0.3571;//时间敏感是否传送的评估系数
};

// ! Fifo scheduling
// struct FlowControllerFifoSchedule
// {
//     void register_writer(
//             fastrtps::rtps::RTPSWriter*) const
//     {
//         std::cout<<"先进先出策略"<<std::endl;
//     }

//     void unregister_writer(
//             fastrtps::rtps::RTPSWriter*) const
//     {
//     }

//     void work_done() const
//     {
//         // Do nothing
//     }

//     void add_new_sample(
//             fastrtps::rtps::RTPSWriter*,
//             fastrtps::rtps::CacheChange_t* change)
//     {
//         // std::cout << "==FlowControllerFifoSchedule::add_new_sample(): 向FlowQueue中添加change==" << std::endl;
//         queue_.add_new_sample(change);
//         int len = queue_.get_size();
//         std::cout<<"队列长度（fxq）："<<len<<std::endl;

//     }

//     void add_old_sample(
//             fastrtps::rtps::RTPSWriter*,
//             fastrtps::rtps::CacheChange_t* change)
//     {
//         queue_.add_old_sample(change);
//     }

//     /*!
//      * Returns the first sample in the queue.
//      * Default behaviour.
//      * Expects the queue is ordered.
//      *
//      * @return Pointer to next change to be sent. nullptr implies there is no sample to be sent or is forbidden due to
//      * bandwidth exceeded.
//      */
//     fastrtps::rtps::CacheChange_t* get_next_change_nts()
//     {
//         int len = queue_.get_size();
//         std::cout<<"队列长度（fxq）："<<len<<std::endl;
//         return queue_.get_next_change();
//     }

//     /*!
//      * Store the sample at the end of the list.
//      *
//      * @return true if there is added changes.
//      */
//     void add_interested_changes_to_queue_nts()
//     {
//         // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
//         queue_.add_interested_changes_to_queue();
//     }

//     void set_bandwith_limitation(
//             uint32_t) const
//     {
//     }

//     void trigger_bandwidth_limit_reset() const
//     {
//     }

// private:

//     //! Scheduler queue. FIFO scheduler only has one queue.
//     FlowQueue queue_;
// };

// ! Round Robin scheduling
// struct FlowControllerFifoSchedule
// {
//     using element = std::tuple<fastrtps::rtps::RTPSWriter*, FlowQueue>;
//     using container = std::vector<element>;
//     using iterator = container::iterator;

//     FlowControllerFifoSchedule()
//     {
//         std::cout<<"轮询调度策略"<<std::endl;
//         next_writer_ = writers_queue_.begin();
//     }

//     void register_writer(
//             fastrtps::rtps::RTPSWriter* writer)
//     {
//         fastrtps::rtps::RTPSWriter* current_writer = nullptr;

//         if (writers_queue_.end() != next_writer_)
//         {
//             current_writer = std::get<0>(*next_writer_);
//         }

//         assert(writers_queue_.end() == find(writer));
//         writers_queue_.emplace_back(writer, FlowQueue());

//         if (nullptr == current_writer)
//         {
//             next_writer_ = writers_queue_.begin();
//         }
//         else
//         {
//             next_writer_ = find(current_writer);
//         }
//     }

//     void unregister_writer(
//             fastrtps::rtps::RTPSWriter* writer)
//     {
//         // Queue cannot be empty, as writer should be present
//         assert(writers_queue_.end() != next_writer_);
//         fastrtps::rtps::RTPSWriter* current_writer = std::get<0>(*next_writer_);
//         assert(nullptr != current_writer);

//         auto it = find(writer);
//         assert(it != writers_queue_.end());
//         assert(std::get<1>(*it).is_empty());

//         // Go to the next writer when unregistering the current one
//         if (it == next_writer_)
//         {
//             set_next_writer();
//             current_writer = std::get<0>(*next_writer_);
//         }

//         writers_queue_.erase(it);
//         if (writer == current_writer)
//         {
//             next_writer_ = writers_queue_.begin();
//         }
//         else
//         {
//             next_writer_ = find(current_writer);
//         }
//     }

//     void work_done()
//     {
//         assert(0 < writers_queue_.size());
//         assert(writers_queue_.end() != next_writer_);
//         set_next_writer();
//     }

//     iterator set_next_writer()
//     {
//         iterator next = std::next(next_writer_);
//         next_writer_ = writers_queue_.end() == next ? writers_queue_.begin() : next;
//         return next_writer_;
//     }

//     void add_new_sample(
//             fastrtps::rtps::RTPSWriter* writer,
//             fastrtps::rtps::CacheChange_t* change)
//     {
//         auto it = find(writer);
//         assert(it != writers_queue_.end());
//         std::get<1>(*it).add_new_sample(change);
//     }

//     void add_old_sample(
//             fastrtps::rtps::RTPSWriter* writer,
//             fastrtps::rtps::CacheChange_t* change)
//     {
//         auto it = find(writer);
//         assert(it != writers_queue_.end());
//         std::get<1>(*it).add_old_sample(change);
//     }

//     fastrtps::rtps::CacheChange_t* get_next_change_nts()
//     {
//         fastrtps::rtps::CacheChange_t* ret_change = nullptr;

//         if (0 < writers_queue_.size())
//         {
//             auto starting_it = next_writer_;     // For avoid loops.

//             do
//             {
//                 ret_change = std::get<1>(*next_writer_).get_next_change();
//             } while (nullptr == ret_change && starting_it != set_next_writer());
//         }

//         return ret_change;
//     }

//     void add_interested_changes_to_queue_nts()
//     {
//         // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
//         for (auto& queue : writers_queue_)
//         {
//             std::get<1>(queue).add_interested_changes_to_queue();
//         }
//     }

//     void set_bandwith_limitation(
//             uint32_t) const
//     {
//     }

//     void trigger_bandwidth_limit_reset() const
//     {
//     }

// private:

//     iterator find(
//             const fastrtps::rtps::RTPSWriter* writer)
//     {
//         return std::find_if(writers_queue_.begin(), writers_queue_.end(),
//                        [writer](const element& current_writer) -> bool
//                        {
//                            return writer == std::get<0>(current_writer);
//                        });
//     }

//     container writers_queue_;
//     iterator next_writer_;
// };
// // HighPriority
// struct FlowControllerFifoSchedule
// {
//     void register_writer(
//             fastrtps::rtps::RTPSWriter* writer)
//     {
//         assert(nullptr != writer);
//         int priority = 10;
//         // if(writer->getGuid().entityId.to_string()=="00.00.13.03")
//         if(writer->getGuid().entityId.to_string()=="00.00.12.03")
//         {
//             priority = 0;
//         }
//         // else if(writer->getGuid().entityId.to_string()=="00.00.12.03")
//         else if(writer->getGuid().entityId.to_string()=="00.00.13.03")
//         {
//             priority = 1;
//         }
//         else if(writer->getGuid().entityId.to_string()=="00.00.11.03")
//         {
//             priority = 6;
//         }
//         //将writer和其优先级作为键值对插入到 priorities_ 映射中
//         auto ret = priorities_.insert({writer, priority});
//         (void)ret;
//         assert(ret.second);

//         // Ensure the priority is created.
//         //这行代码还使用了引用（&），这意味着 queue 是对 writers_queue_[priority] 的引用，而不是它的副本。
//         //这样做的好处是任何对 queue 的修改都会直接反映在 writers_queue_ 映射中存储的相应元素上。
//         FlowQueue& queue = writers_queue_[priority];
//         (void)queue;
//     }

//     void unregister_writer(
//             fastrtps::rtps::RTPSWriter* writer)
//     {
//         auto it = priorities_.find(writer);
//         assert(it != priorities_.end());
//         priorities_.erase(it);
//     }

//     void work_done() const
//     {
//         // Do nothing
//     }

//     void add_new_sample(
//             fastrtps::rtps::RTPSWriter* writer,
//             fastrtps::rtps::CacheChange_t* change)
//     {
//         assert(nullptr != writer);
//         int priority = 10;
//         // if(writer->getGuid().entityId.to_string()=="00.00.13.03")
//         if(writer->getGuid().entityId.to_string()=="00.00.12.03")
//         {
//             priority = 0;
//         }
//         // else if(writer->getGuid().entityId.to_string()=="00.00.12.03")
//         else if(writer->getGuid().entityId.to_string()=="00.00.13.03")
//         {
//             priority = 1;
//         }
//         else if(writer->getGuid().entityId.to_string()=="00.00.11.03")
//         {
//             priority = 6;
//         }
//         find_queue(writer).add_new_sample(change);
//     }

//     void add_old_sample(
//             fastrtps::rtps::RTPSWriter* writer,
//             fastrtps::rtps::CacheChange_t* change)
//     {
//         find_queue(writer).add_old_sample(change);
//     }

//     fastrtps::rtps::CacheChange_t* get_next_change_nts()
//     {
//         fastrtps::rtps::CacheChange_t* ret_change = nullptr;

//         if (0 < writers_queue_.size())
//         {
//             for (auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
//             {
//                 ret_change = it->second.get_next_change();
//             }
//         }

//         return ret_change;
//     }

//     void add_interested_changes_to_queue_nts()
//     {
//         // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
//         for (auto& queue : writers_queue_)
//         {
//             queue.second.add_interested_changes_to_queue();
//         }
//     }

//     void set_bandwith_limitation(
//             uint32_t) const
//     {
//     }

//     void trigger_bandwidth_limit_reset() const
//     {
//     }

// private:

//     FlowQueue& find_queue(
//             fastrtps::rtps::RTPSWriter* writer)
//     {
//         // Find priority.
//         auto priority_it = priorities_.find(writer);
//         assert(priority_it != priorities_.end());
//         auto queue_it = writers_queue_.find(priority_it->second);
//         assert(queue_it != writers_queue_.end());
//         return queue_it->second;
//     }

//     std::map<int32_t, FlowQueue> writers_queue_;

//     std::unordered_map<fastrtps::rtps::RTPSWriter*, int32_t> priorities_;
// };
//! Round Robin scheduling
struct FlowControllerRoundRobinSchedule
{
    using element = std::tuple<fastrtps::rtps::RTPSWriter*, FlowQueue>;
    using container = std::vector<element>;
    using iterator = container::iterator;

    FlowControllerRoundRobinSchedule()
    {
        next_writer_ = writers_queue_.begin();
    }

    void register_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        fastrtps::rtps::RTPSWriter* current_writer = nullptr;

        if (writers_queue_.end() != next_writer_)
        {
            current_writer = std::get<0>(*next_writer_);
        }

        assert(writers_queue_.end() == find(writer));
        writers_queue_.emplace_back(writer, FlowQueue());

        if (nullptr == current_writer)
        {
            next_writer_ = writers_queue_.begin();
        }
        else
        {
            next_writer_ = find(current_writer);
        }
    }

    void unregister_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        // Queue cannot be empty, as writer should be present
        assert(writers_queue_.end() != next_writer_);
        fastrtps::rtps::RTPSWriter* current_writer = std::get<0>(*next_writer_);
        assert(nullptr != current_writer);

        auto it = find(writer);
        assert(it != writers_queue_.end());
        assert(std::get<1>(*it).is_empty());

        // Go to the next writer when unregistering the current one
        if (it == next_writer_)
        {
            set_next_writer();
            current_writer = std::get<0>(*next_writer_);
        }

        writers_queue_.erase(it);
        if (writer == current_writer)
        {
            next_writer_ = writers_queue_.begin();
        }
        else
        {
            next_writer_ = find(current_writer);
        }
    }

    void work_done()
    {
        assert(0 < writers_queue_.size());
        assert(writers_queue_.end() != next_writer_);
        set_next_writer();
    }

    iterator set_next_writer()
    {
        iterator next = std::next(next_writer_);
        next_writer_ = writers_queue_.end() == next ? writers_queue_.begin() : next;
        return next_writer_;
    }

    void add_new_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        auto it = find(writer);
        assert(it != writers_queue_.end());
        std::get<1>(*it).add_new_sample(change);
    }

    void add_old_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        auto it = find(writer);
        assert(it != writers_queue_.end());
        std::get<1>(*it).add_old_sample(change);
    }

    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        fastrtps::rtps::CacheChange_t* ret_change = nullptr;

        if (0 < writers_queue_.size())
        {
            auto starting_it = next_writer_;     // For avoid loops.

            do
            {
                ret_change = std::get<1>(*next_writer_).get_next_change();
            } while (nullptr == ret_change && starting_it != set_next_writer());
        }

        return ret_change;
    }

    void add_interested_changes_to_queue_nts()
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        for (auto& queue : writers_queue_)
        {
            std::get<1>(queue).add_interested_changes_to_queue();
        }
    }

    void set_bandwith_limitation(
            uint32_t) const
    {
    }

    void trigger_bandwidth_limit_reset() const
    {
    }

private:

    iterator find(
            const fastrtps::rtps::RTPSWriter* writer)
    {
        return std::find_if(writers_queue_.begin(), writers_queue_.end(),
                       [writer](const element& current_writer) -> bool
                       {
                           return writer == std::get<0>(current_writer);
                       });
    }

    container writers_queue_;
    iterator next_writer_;

};

//! High priority scheduling
struct FlowControllerHighPrioritySchedule
{
    void register_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        assert(nullptr != writer);
        int32_t priority = 10;
        auto property = fastrtps::rtps::PropertyPolicyHelper::find_property(
            writer->getAttributes().properties, "fastdds.sfc.priority");

        if (nullptr != property)
        {
            char* ptr = nullptr;
            priority = strtol(property->c_str(), &ptr, 10);

            if (property->c_str() != ptr)     // A valid integer was read.
            {
                if (-10 > priority || 10 < priority)
                {
                    priority = 10;
                    logError(RTPS_WRITER,
                            "Wrong value for fastdds.sfc.priority property. Range is [-10, 10]. Priority set to lowest (10)");
                }
            }
            else
            {
                priority = 10;
                logError(RTPS_WRITER,
                        "Not numerical value for fastdds.sfc.priority property. Priority set to lowest (10)");
            }
        }

        auto ret = priorities_.insert({writer, priority});
        (void)ret;
        assert(ret.second);

        // Ensure the priority is created.
        FlowQueue& queue = writers_queue_[priority];
        (void)queue;
    }

    void unregister_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        auto it = priorities_.find(writer);
        assert(it != priorities_.end());
        priorities_.erase(it);
    }

    void work_done() const
    {
        // Do nothing
    }

    void add_new_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        find_queue(writer).add_new_sample(change);
    }

    void add_old_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        find_queue(writer).add_old_sample(change);
    }

    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        fastrtps::rtps::CacheChange_t* ret_change = nullptr;

        if (0 < writers_queue_.size())
        {
            for (auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
            {
                ret_change = it->second.get_next_change();
            }
        }

        return ret_change;
    }

    void add_interested_changes_to_queue_nts()
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        for (auto& queue : writers_queue_)
        {
            queue.second.add_interested_changes_to_queue();
        }
    }

    void set_bandwith_limitation(
            uint32_t) const
    {
    }

    void trigger_bandwidth_limit_reset() const
    {
    }

private:

    FlowQueue& find_queue(
            fastrtps::rtps::RTPSWriter* writer)
    {
        // Find priority.
        auto priority_it = priorities_.find(writer);
        assert(priority_it != priorities_.end());
        auto queue_it = writers_queue_.find(priority_it->second);
        assert(queue_it != writers_queue_.end());
        return queue_it->second;
    }

    std::map<int32_t, FlowQueue> writers_queue_;

    std::unordered_map<fastrtps::rtps::RTPSWriter*, int32_t> priorities_;
};

//! Priority with reservation scheduling
struct FlowControllerPriorityWithReservationSchedule
{
    void register_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        assert(nullptr != writer);
        int32_t priority = 10;
        auto property = fastrtps::rtps::PropertyPolicyHelper::find_property(
            writer->getAttributes().properties, "fastdds.sfc.priority");

        if (nullptr != property)
        {
            char* ptr = nullptr;
            priority = strtol(property->c_str(), &ptr, 10);

            if (property->c_str() != ptr)     // A valid integer was read.
            {
                if (-10 > priority || 10 < priority)
                {
                    priority = 10;
                    logError(RTPS_WRITER,
                            "Wrong value for fastdds.sfc.priority property. Range is [-10, 10]. Priority set to lowest (10)");
                }
            }
            else
            {
                priority = 10;
                logError(RTPS_WRITER,
                        "Not numerical value for fastdds.sfc.priority property. Priority set to lowest (10)");
            }
        }

        uint32_t reservation = 0;
        property = fastrtps::rtps::PropertyPolicyHelper::find_property(
            writer->getAttributes().properties, "fastdds.sfc.bandwidth_reservation");

        if (nullptr != property)
        {
            char* ptr = nullptr;
            reservation = strtoul(property->c_str(), &ptr, 10);

            if (property->c_str() != ptr)     // A valid integer was read.
            {
                if (100 < reservation)
                {
                    reservation = 0;
                    logError(RTPS_WRITER,
                            "Wrong value for fastdds.sfc.bandwidth_reservation property. Range is [0, 100]. Reservation set to lowest (0)");
                }
            }
            else
            {
                reservation = 0;
                logError(RTPS_WRITER,
                        "Not numerical value for fastdds.sfc.bandwidth_reservation property. Reservation set to lowest (0)");
            }
        }

        // Calculate reservation in bytes.
        uint32_t reservation_bytes = (0 == bandwidth_limit_? 0 :
                ((bandwidth_limit_ * reservation) / 100));

        auto ret = writers_queue_.emplace(writer, std::make_tuple(FlowQueue(), priority, reservation_bytes, 0u));
        (void)ret;
        assert(ret.second);

        priorities_[priority].push_back(writer);
    }

    void unregister_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        auto it = writers_queue_.find(writer);
        assert(it != writers_queue_.end());
        int32_t priority = std::get<1>(it->second);
        writers_queue_.erase(it);
        auto priority_it = priorities_.find(priority);
        assert(priority_it != priorities_.end());
        auto writer_it = std::find(priority_it->second.begin(), priority_it->second.end(), writer);
        assert(writer_it != priority_it->second.end());
        priority_it->second.erase(writer_it);
    }

    void work_done()
    {
        if (nullptr != writer_being_processed_)
        {
            assert(0 != size_being_processed_);
            auto writer = writers_queue_.find(writer_being_processed_);
            std::get<3>(writer->second) += size_being_processed_;
            writer_being_processed_ = nullptr;
            size_being_processed_ = 0;
        }
    }

    void add_new_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        // Find writer queue..
        auto it = writers_queue_.find(writer);
        assert(it != writers_queue_.end());
        std::get<0>(it->second).add_new_sample(change);
    }

    void add_old_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change)
    {
        // Find writer queue..
        auto it = writers_queue_.find(writer);
        assert(it != writers_queue_.end());
        std::get<0>(it->second).add_old_sample(change);
    }

    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        fastrtps::rtps::CacheChange_t* highest_priority = nullptr;
        fastrtps::rtps::CacheChange_t* ret_change = nullptr;

        if (0 < writers_queue_.size())
        {
            for (auto& priority : priorities_)
            {
                for (auto writer_it : priority.second)
                {
                    auto writer = writers_queue_.find(writer_it);
                    fastrtps::rtps::CacheChange_t* change = std::get<0>(writer->second).get_next_change();

                    if (nullptr == highest_priority)
                    {
                        highest_priority = change;
                    }

                    if (nullptr != change)
                    {
                        // Check if writer's next change can be processed because the writer's bandwidth reservation is
                        // enough.
                        uint32_t size_to_check = change->serializedPayload.length;
                        if (0 != change->getFragmentCount())
                        {
                            size_to_check = change->getFragmentSize();
                        }

                        if (std::get<2>(writer->second) > (std::get<3>(writer->second) + size_to_check))
                        {
                            ret_change = change;
                            writer_being_processed_ = writer_it;
                            size_being_processed_ = size_to_check;
                            break;
                        }
                    }
                }

                if (nullptr != ret_change)
                {
                    break;
                }
            }
        }

        return (nullptr != ret_change ? ret_change : highest_priority);
    }

    void add_interested_changes_to_queue_nts()
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        for (auto& queue : writers_queue_)
        {
            std::get<0>(queue.second).add_interested_changes_to_queue();
        }
    }

    void set_bandwith_limitation(
            uint32_t limit)
    {
        bandwidth_limit_ = limit;
    }

    void trigger_bandwidth_limit_reset()
    {
        for (auto& writer : writers_queue_)
        {
            std::get<3>(writer.second) = 0;
        }
    }

private:

    using map_writers = std::unordered_map<fastrtps::rtps::RTPSWriter*, std::tuple<FlowQueue, int32_t, uint32_t,
                    uint32_t>>;

    using map_priorities = std::map<int32_t, std::vector<fastrtps::rtps::RTPSWriter*>>;

    map_writers writers_queue_;

    map_priorities priorities_;

    uint32_t bandwidth_limit_ = 0;

    fastrtps::rtps::RTPSWriter* writer_being_processed_ = nullptr;

    uint32_t size_being_processed_ = 0;
};

template<typename PublishMode, typename SampleScheduling>
class FlowControllerImpl : public FlowController
{
    using publish_mode = PublishMode;
    using scheduler = SampleScheduling;

public:

    FlowControllerImpl(
            fastrtps::rtps::RTPSParticipantImpl* participant,
            const FlowControllerDescriptor* descriptor
            )
        : participant_(participant)
        , async_mode(participant, descriptor)
    {
        uint32_t limitation = get_max_payload();

        if ((std::numeric_limits<uint32_t>::max)() != limitation)
        {
            sched.set_bandwith_limitation(limitation);
        }
    }

    virtual ~FlowControllerImpl() noexcept
    {
    }

    /*!
     * Initializes the flow controller.
     */
    void init() override
    {
        initialize_async_thread();
    }

    /*!
     * Registers a writer.
     * This object is only be able to manage a CacheChante_t if its writer was registered previously with this function.
     *
     * @param writer Pointer to the writer to be registered. Cannot be nullptr.
     */
    void register_writer(
            fastrtps::rtps::RTPSWriter* writer) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto ret = writers_.insert({ writer->getGuid(), writer});
        (void)ret;
        assert(ret.second);
        register_writer_impl(writer);
    }

    /*!
     * Unregister a writer.
     *
     * @param writer Pointer to the writer to be unregistered. Cannot be nullptr.
     */
    void unregister_writer(
            fastrtps::rtps::RTPSWriter* writer) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        writers_.erase(writer->getGuid());
        unregister_writer_impl(writer);
    }

    /*
     * Adds the CacheChange_t to be managed by this object.
     * The CacheChange_t has to be a new one, that is, it has to be added to the writer's history before this call.
     * This function should be called by RTPSWriter::unsent_change_added_to_history().
     * This function has two specializations depending on template parameter PublishMode.
     *
     * @param Pointer to the writer which the added CacheChante_t is responsable. Cannot be nullptr.
     * @param change Pointer to the new CacheChange_t to be managed by this object. Cannot be nullptr.
     * @return true if sample could be added. false in other case.
     */
    bool add_new_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time) override
    {
        return add_new_sample_impl(writer, change, max_blocking_time);
    }

    /*!
     * Adds the CacheChante_t to be managed by this object.
     * The CacheChange_t has to be an old one, that is, it is already in the writer's history and for some reason has to
     * be sent again.
     *
     * @param Pointer to the writer which the added change is responsable. Cannot be nullptr.
     * @param change Pointer to the old change to be managed by this object. Cannot be nullptr.
     * @return true if sample could be added. false in other case.
     */
    bool add_old_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change) override
    {
        return add_old_sample_impl(writer, change,
                       std::chrono::steady_clock::now() + std::chrono::hours(24));
    }

    /*!
     * If currently the CacheChange_t is managed by this object, remove it.
     * This funcion should be called when a CacheChange_t is removed from the writer's history.
     *
     * @param Pointer to the change which should be removed if it is currently managed by this object.
     */
    void remove_change(
            fastrtps::rtps::CacheChange_t* change) override
    {
        assert(nullptr != change);
        remove_change_impl(change);
    }

    uint32_t get_max_payload() override
    {
        return get_max_payload_impl();
    }

private:

    /*!
     * Initialize asynchronous thread.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    initialize_async_thread()
    {
        bool expected = false;
        if (async_mode.running.compare_exchange_strong(expected, true))
        {
            // Code for initializing the asynchronous thread.
            async_mode.thread = std::thread(&FlowControllerImpl::run, this);
        }
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case the async thread doesn't need to be initialized.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    initialize_async_thread()
    {
        // Do nothing.
    }

    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    register_writer_impl(
            fastrtps::rtps::RTPSWriter* writer)
    {
        std::unique_lock<std::mutex> in_lock(async_mode.changes_interested_mutex);
        sched.register_writer(writer);
    }

    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    register_writer_impl(
            fastrtps::rtps::RTPSWriter*)
    {
        // Do nothing.
    }

    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    unregister_writer_impl(
            fastrtps::rtps::RTPSWriter* writer)
    {
        std::unique_lock<std::mutex> in_lock(async_mode.changes_interested_mutex);
        sched.unregister_writer(writer);
    }

    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    unregister_writer_impl(
            fastrtps::rtps::RTPSWriter*)
    {
        // Do nothing.
    }

    /*!
     * This function store internally the sample and wake up the async thread.
     *
     * @note Before calling this function, the change's writer mutex have to be locked.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    enqueue_new_sample_impl(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& /* TODO max_blocking_time*/)
    {
        assert(nullptr == change->writer_info.previous &&
                nullptr == change->writer_info.next);
        // Sync delivery failed. Try to store for asynchronous delivery.
        std::unique_lock<std::mutex> lock(async_mode.changes_interested_mutex);
        sched.add_new_sample(writer, change);
        // std::cout << "==FlowControllerImpl<...>::enqueue_new_sample_impl(): 向FlowQueue(new_interested_)中添加change==" << std::endl;
        async_mode.cv.notify_one();
        // std::cout << "==FlowControllerImpl<...>::enqueue_new_sample_impl(): 通知异步线程已经向FlowQueue中添加change" << std::endl;
        return true;
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case there is no async mechanism.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    constexpr enqueue_new_sample_impl(
            fastrtps::rtps::RTPSWriter*,
            fastrtps::rtps::CacheChange_t*,
            const std::chrono::time_point<std::chrono::steady_clock>&) const
    {
        // Do nothing. Return false.
        return false;
    }

    /*!
     * This function tries to send the sample synchronously.
     * That is, it uses the user's thread, which is the one calling this function, to send the sample.
     * It calls new function `RTPSWriter::deliver_sample_nts()` for sending the sample.
     * If this function fails (for example because non-blocking socket is full), this function stores internally the sample to
     * try sending it again asynchronously.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_base_of<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_new_sample_impl(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time)
    {
        // This call should be made with writer's mutex locked.
        fastrtps::rtps::LocatorSelectorSender& locator_selector = writer->get_general_locator_selector();
        std::lock_guard<fastrtps::rtps::LocatorSelectorSender> lock(locator_selector);
        fastrtps::rtps::RTPSMessageGroup group(participant_, writer, &locator_selector);
        if (fastrtps::rtps::DeliveryRetCode::DELIVERED !=
                writer->deliver_sample_nts(change, group, locator_selector, max_blocking_time))
        {
            return enqueue_new_sample_impl(writer, change, max_blocking_time);
        }

        return true;
    }

    /*!
     * This function stores internally the sample to send it asynchronously.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_base_of<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_new_sample_impl(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time)
    {
        return enqueue_new_sample_impl(writer, change, max_blocking_time);
    }

    /*!
     * This function store internally the sample and wake up the async thread.
     *
     * @note Before calling this function, the change's writer mutex have to be locked.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_old_sample_impl(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& /* TODO max_blocking_time*/)
    {
        // This comparison is thread-safe, because we ensure the change to a problematic state is always protected for
        // its writer's mutex.
        // Problematic states:
        // - Being added: change both pointers from nullptr to a pointer values.
        // - Being removed: change both pointer from pointer values to nullptr.
        if (nullptr == change->writer_info.previous &&
                nullptr == change->writer_info.next)
        {
            std::unique_lock<std::mutex> lock(async_mode.changes_interested_mutex);
            sched.add_old_sample(writer, change);
            async_mode.cv.notify_one();

            return true;
        }

        return false;
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case there is no async mechanism.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    constexpr add_old_sample_impl(
            fastrtps::rtps::RTPSWriter*,
            fastrtps::rtps::CacheChange_t*,
            const std::chrono::time_point<std::chrono::steady_clock>&) const
    {
        return false;
    }

    /*!
     * This function store internally the sample and wake up the async thread.
     *
     * @note Before calling this function, the change's writer mutex have to be locked.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    remove_change_impl(
            fastrtps::rtps::CacheChange_t* change)
    {
        // This comparison is thread-safe, because we ensure the change to a problematic state is always protected for
        // its writer's mutex.
        // Problematic states:
        // - Being added: change both pointers from nullptr to a pointer values.
        // - Being removed: change both pointer from pointer values to nullptr.
        if (nullptr != change->writer_info.previous ||
                nullptr != change->writer_info.next)
        {
            ++async_mode.writers_interested_in_remove;
            std::unique_lock<std::mutex> lock(mutex_);
            std::unique_lock<std::mutex> interested_lock(async_mode.changes_interested_mutex);

            // When blocked, both pointer are different than nullptr or equal.
            assert((nullptr != change->writer_info.previous &&
                    nullptr != change->writer_info.next) ||
                    (nullptr == change->writer_info.previous &&
                    nullptr == change->writer_info.next));
            if (nullptr != change->writer_info.previous &&
                    nullptr != change->writer_info.next)
            {

                // Try to join previous node and next node.
                change->writer_info.previous->writer_info.next = change->writer_info.next;
                change->writer_info.next->writer_info.previous = change->writer_info.previous;
                change->writer_info.previous = nullptr;
                change->writer_info.next = nullptr;
            }
            --async_mode.writers_interested_in_remove;
        }
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case there is no async mechanism.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    remove_change_impl(
            fastrtps::rtps::CacheChange_t*) const
    {
        // Do nothing.
    }

    /*!
     * Function run by the asynchronous thread.
     */
    void run()
    {
        while (async_mode.running)
        {
            // std::cout << "while (async_mode.running)异步发送线程启动" << std::endl;
            // There are writers interested in removing a sample.
            if (0 != async_mode.writers_interested_in_remove)
            {
                continue;
            }

            std::unique_lock<std::mutex> lock(mutex_);
            fastrtps::rtps::CacheChange_t* change_to_process = nullptr;

            //Check if we have to sleep.
            //代码块：向interest队列中添加数据，分为队列为空和队列不空两种情况
            {
                std::unique_lock<std::mutex> in_lock(async_mode.changes_interested_mutex);
                // Add interested changes into the queue.
                sched.add_interested_changes_to_queue_nts();
                // std::cout << "1111111sched.add_interested_changes_to_queue_nts();" << std::endl;

                while (async_mode.running &&
                        (async_mode.force_wait() || nullptr == (change_to_process = sched.get_next_change_nts())))
                {
                    // std::cout << "强制等待或队列为空的时候执行" << std::endl;
                    // std::cout<<"sched.get_next_change_nts():"<<sched.get_next_change_nts()<<std::endl;
                    // Release main mutex to allow registering/unregistering writers while this thread is waiting.
                    lock.unlock();
                    bool ret = async_mode.wait(in_lock);

                    in_lock.unlock();
                    lock.lock();
                    in_lock.lock();

                    if (ret)
                    {
                        sched.trigger_bandwidth_limit_reset();
                    }
                    sched.add_interested_changes_to_queue_nts();
                    // std::cout << "2222222sched.add_interested_changes_to_queue_nts();" << std::endl;
                }
            }
            fastrtps::rtps::RTPSWriter* current_writer = nullptr;
            //获取数据
            while (nullptr != change_to_process)
            {
                // std::cout << "取出的数据的序列号：" << change_to_process->sequenceNumber << std::endl;
                // Fast check if next change will enter.
                if (!async_mode.fast_check_is_there_slot_for_change(change_to_process))
                {
                    break;
                }

                if (nullptr == current_writer || current_writer->getGuid() != change_to_process->writerGUID)
                {
                    auto writer_it = writers_.find(change_to_process->writerGUID);
                    assert(writers_.end() != writer_it);

                    current_writer = writer_it->second;
                }

                if (!current_writer->getMutex().try_lock())
                {
                    break;
                }

                fastrtps::rtps::LocatorSelectorSender& locator_selector =
                        current_writer->get_async_locator_selector();
                async_mode.group.sender(current_writer, &locator_selector);
                locator_selector.lock();

                // Remove previously from queue, because deliver_sample_nts could call FlowController::remove_sample()
                // provoking a deadlock.
                fastrtps::rtps::CacheChange_t* previous = change_to_process->writer_info.previous;
                fastrtps::rtps::CacheChange_t* next = change_to_process->writer_info.next;
                previous->writer_info.next = next;
                next->writer_info.previous = previous;
                change_to_process->writer_info.previous = nullptr;
                change_to_process->writer_info.next = nullptr;
                //////////////////////////////////////////////////////////////////
                // todo：移除上指针和下指针（相当于在时间优先队列当中移除change_to_process）
                fastrtps::rtps::CacheChange_t* top = nullptr;
                fastrtps::rtps::CacheChange_t* bottom = nullptr;
                if (change_to_process->writer_info.earlier != nullptr && change_to_process->writer_info.later != nullptr)
                {
                    top = change_to_process->writer_info.earlier;
                    bottom = change_to_process->writer_info.later;
                    top->writer_info.later = bottom;
                    bottom->writer_info.earlier = top;
                    change_to_process->writer_info.earlier = nullptr;
                    change_to_process->writer_info.later = nullptr;
                }
                //////////////////////////////////////////////////////////////////
                fastrtps::rtps::DeliveryRetCode ret_delivery = current_writer->deliver_sample_nts(
                    change_to_process, async_mode.group, locator_selector,
                    std::chrono::steady_clock::now() + std::chrono::hours(24));

                if (fastrtps::rtps::DeliveryRetCode::DELIVERED != ret_delivery)
                {
                    // If delivery fails, put the change again in the queue.
                    previous->writer_info.next = change_to_process;
                    next->writer_info.previous = change_to_process;
                    change_to_process->writer_info.previous = previous;
                    change_to_process->writer_info.next = next;

                    //////////////////////////////////////////////////////////////////
                    // todo：恢复上指针和下指针
                    if (top != nullptr && bottom != nullptr)
                    {
                        top->writer_info.later = change_to_process;
                        bottom->writer_info.earlier = change_to_process;
                        change_to_process->writer_info.earlier = top;
                        change_to_process->writer_info.later = bottom;
                    }
                    //////////////////////////////////////////////////////////////////
                    
                    async_mode.process_deliver_retcode(ret_delivery);

                    locator_selector.unlock();
                    current_writer->getMutex().unlock();
                    // Unlock mutex_ and try again.
                    break;
                }

                locator_selector.unlock();
                current_writer->getMutex().unlock();
                sched.work_done();

                if (0 != async_mode.writers_interested_in_remove)
                {
                    // There are writers that want to remove samples.
                    break;
                }

                // Add interested changes into the queue.
                {
                    std::unique_lock<std::mutex> in_lock(async_mode.changes_interested_mutex);
                    sched.add_interested_changes_to_queue_nts();
                }

                change_to_process = sched.get_next_change_nts();
                // std::cout << "获取FlowQueue的下一个数据" << std::endl;
                if (change_to_process == nullptr)
                {
                    // std::cout << "==SubThread-获取失败,当前FlowQueue为空==" << std::endl;
                }
            }

            async_mode.group.sender(nullptr, nullptr);
        }
        // std::cout << "==SubThread-FlowControllerImpl<PublishMode, SampleScheduling>::run(): 异步发送线程结束==" << std::endl;
    }

    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_base_of<FlowControllerLimitedAsyncPublishMode, PubMode>::value, uint32_t>::type
    get_max_payload_impl()
    {
        return static_cast<uint32_t>(async_mode.max_bytes_per_period);
    }

    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_base_of<FlowControllerLimitedAsyncPublishMode, PubMode>::value, uint32_t>::type
    constexpr get_max_payload_impl() const
    {
        return (std::numeric_limits<uint32_t>::max)();
    }

    std::mutex mutex_;

    fastrtps::rtps::RTPSParticipantImpl* participant_ = nullptr;

    std::map<fastrtps::rtps::GUID_t, fastrtps::rtps::RTPSWriter*> writers_;

    scheduler sched;

    // async_mode must be destroyed before sched.
    publish_mode async_mode;

    int call_num_ = 0;
};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // _RTPS_FLOWCONTROL_FLOWCONTROLLERIMPL_HPP_
