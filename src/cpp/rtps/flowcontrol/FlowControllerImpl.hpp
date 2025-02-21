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

    bool is_time_queue_empty() const noexcept
    {
        return new_ones_.is_time_empty() && old_ones_.is_time_empty();
    }

    void add_into_time_queue(FlowQueue& time_queue)
    {
        new_ones_.add_into_time_list(time_queue.new_ones_);
    }

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
        if (!is_empty())
        {
            return !new_ones_.is_empty() ?
                   new_ones_.head.writer_info.next : old_ones_.head.writer_info.next;
        }

        return nullptr;
    }

    fastrtps::rtps::CacheChange_t* get_down_change() noexcept
    {
        if (!is_time_queue_empty())
        {
            return !new_ones_.is_time_empty() ?
                new_ones_.earlier.writer_info.later : old_ones_.earlier.writer_info.later;
        }

        return nullptr;
    }


    void add_interested_changes_to_queue() noexcept
    {
        new_ones_.add_list(new_interested_);
        old_ones_.add_list(old_interested_);
    }


    int get_time_size()
    {
        return new_ones_.list_time_size();
    }


    int get_size()
    {
        return new_ones_.list_size();
    }
    int get_old_size()
    {
        return old_ones_.list_size();
    }

    void print_time_size()
    {
        new_ones_.print_time_list();
    }

    long double calc_ave_trans_time()
    {
        return new_ones_.calc_list_ave_trans_time();
    }

    long double calc_time_sensitive_ave_trans_time()
    {
        return new_ones_.calc_time_sensitive_ave_trans_time();
    }

    void clear_time()
    {
        new_ones_.clear();
    }


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

            earlier.writer_info.later = &later;
            later.writer_info.earlier = &earlier;

        }

        bool is_empty() const noexcept
        {
            assert((&tail == head.writer_info.next && &head == tail.writer_info.previous) ||
                    (&tail != head.writer_info.next && &head != tail.writer_info.previous));
            return &tail == head.writer_info.next;
        }


        bool is_time_empty() const noexcept
        {
            assert((&later == earlier.writer_info.later && &earlier == later.writer_info.earlier) ||
                (&later != earlier.writer_info.later && &earlier != later.writer_info.earlier));
            return &later == earlier.writer_info.later;
        }

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
            fastrtps::rtps::CacheChange_t* temp = head.writer_info.next;
            while (temp != &tail)
            {
                eprosima::fastrtps::rtps::Time_t now; 
                eprosima::fastrtps::rtps::Time_t::now(now);
                if(temp->priority == 1)
                {
                    temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns();
                    long double fracpart,intpart;
                    fracpart = std::modf(temp->remain_time / 10000000000, &intpart);
                    temp->remain_time = fracpart * 0.6;
                }
                else if(temp->priority == 0)
                {
                    temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns();
                    long double fracpart,intpart;
                    fracpart = std::modf(temp->remain_time / 10000000000, &intpart);
                    temp->remain_time = fracpart * 0.6429;
                }
                else if(temp->priority == 6)
                {
                    temp->remain_time = (temp->maxTransportTime + temp->sourceTimestamp).to_ns();
                    long double fracpart,intpart;
                    fracpart = std::modf(temp->remain_time / 10000000000, &intpart);
                    temp->remain_time = fracpart * 0.8571;
                }

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

                    while((temp->remain_time) >=
                            (time_temp->remain_time) && time_temp->writer_info.later != &list.later)
                    {
                        time_temp = time_temp->writer_info.later;
                    }

                    if ((temp->remain_time) < 
                            (time_temp->remain_time))
                    {
                        temp->writer_info.earlier = time_temp->writer_info.earlier;
                        temp->writer_info.earlier->writer_info.later = temp;
                        time_temp->writer_info.earlier = temp;
                        temp->writer_info.later = time_temp;
                    }

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

                eprosima::fastrtps::rtps::Time_t now; 

                eprosima::fastrtps::rtps::Time_t::now(now);
                while (temp->writer_info.later != &later)
                {
                    temp = temp->writer_info.later;
                    trans_time_sum += ((8 - temp->priority) / 8.0 + 1) * (now - temp->sourceTimestamp).to_ns(); 
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
                eprosima::fastrtps::rtps::Time_t now;
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
        assert(nullptr != descriptor);
        assert(0 < descriptor->max_bytes_per_period);
        max_bytes_per_period = 8*1024*1024;
        period_ms = std::chrono::milliseconds(1000);
        group.set_sent_bytes_limitation(static_cast<uint32_t>(max_bytes_per_period));
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


int getPingTime(const std::string& ip)
{
    std::ostringstream commandStream;
    commandStream << "ping -c 1 " << ip << " > ping_tmp.txt";
    int systemResult = std::system(commandStream.str().c_str());


    if (systemResult != 0) {
        std::cerr << "Ping command failed with return code: " << systemResult << std::endl;
        return -1;
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
    std::array<char, 128> buffer;
    std::string result;
    std::string command = "ping -c 1 " + ip + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");

    if (!pipe) {
        return 0.0f;
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int returnCode = pclose(pipe);
    if (returnCode != 0) {
        return 0.0f;
    }
    std::regex regex("time=(\\d+\\.\\d+)\\s*ms");
    std::smatch match;
    if (std::regex_search(result, match, regex)) {
        return std::stof(match[1]);
    } else {
        return 0.0f;
    }
}

/** Classes used to specify FlowController's sample scheduling **/

// ! DRECHS scheduling
struct FlowControllerDRECHSSchedule
{

    void register_writer(
            fastrtps::rtps::RTPSWriter* writer)
    {
        assert(nullptr != writer);
        int priority = 10;
        if(writer->getGuid().entityId.to_string()=="00.00.12.03")
        // if(writer->getGuid().entityId.to_string()=="00.00.11.03")
        {
            priority = 0;
        }
        else if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        // else if(writer->getGuid().entityId.to_string()=="00.00.14.03")
        {
            priority = 1;
        }
        else if(writer->getGuid().entityId.to_string()=="00.00.11.03")
        // else if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        {
            priority = 6;
        }
        auto ret = priorities_.insert({writer, priority});
        (void)ret;
        assert(ret.second);
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
        assert(nullptr != writer);
        int priority = 10;
        if(writer->getGuid().entityId.to_string()=="00.00.12.03")
        // if(writer->getGuid().entityId.to_string()=="00.00.11.03")
        {
            priority = 0;
        }
        else if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        // else if(writer->getGuid().entityId.to_string()=="00.00.14.03")
        {
            priority = 1;
        }
        else if(writer->getGuid().entityId.to_string()=="00.00.11.03")
        // else if(writer->getGuid().entityId.to_string()=="00.00.13.03")
        {
            priority = 6;
        }
        const char* freq_key_map = std::getenv("PUBLISH_key_map");
        const char* freq_time_sensitive = std::getenv("PUBLISH_time_sensitive");
        const char* freq_edge_output = std::getenv("PUBLISH_edge_output");
        const char* freq_edge_time = std::getenv("PUBLISH_edge_time");
        long double key_map_pubtime = std::stod(freq_key_map);
        long double time_sensitive_pubime = std::stod(freq_time_sensitive);
        long double edge_output_pubtime = std::stod(freq_edge_output);
        long double edge_time= std::stod(freq_edge_time);
        long double edge_output_maxTransportTime = edge_output_pubtime - edge_time;
        long double key_map_maxTransportTime = key_map_pubtime;
        long double time_sensitive_maxTransportTime = std::min(time_sensitive_pubime, std::min(edge_output_maxTransportTime, key_map_maxTransportTime));
        switch (priority)
        {
        case 6:
            change->maxTransportTime = eprosima::fastrtps::rtps::Time_t(key_map_maxTransportTime);
            change->priority = 6;
            break;
        case 1:
            change->maxTransportTime = eprosima::fastrtps::rtps::Time_t(edge_output_maxTransportTime);
            change->priority = 1;
            break;
        case 0:
            change->maxTransportTime = eprosima::fastrtps::rtps::Time_t(time_sensitive_maxTransportTime);
            change->priority = 0;
            break;
        default:
            break;
        }
        find_queue(writer).add_new_sample(change);
    }

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
    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        queue_.clear_time();
        if (0 < writers_queue_.size())
        {
            for (auto it = writers_queue_.begin(); it != writers_queue_.end(); ++it)
            {
                it->second.add_into_time_queue(queue_);
            }
        }
        int time_queue_len = queue_.get_time_size();
        fastrtps::rtps::CacheChange_t* temp = queue_.get_down_change();
        long double pingTime = 0.045;
        pingTime = pingTime * 1000000;
        judge_switch(pingTime);
        fastrtps::rtps::CacheChange_t* ret_change = nullptr;
        if (is_switch)
        {
            ret_change = queue_.get_down_change();
            is_time_max = false;

        }
        else
        {

            if (0 < writers_queue_.size() && is_keytask == true)
            {
                for (auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
                {
                    ret_change = it->second.get_next_change();
                }
                is_keytask = true;
            }
            else if(0 < writers_queue_.size() && is_keytask == false)
            {
                for(auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
                {
                    if(it->first == 1)
                    {
                        ret_change = it->second.get_next_change();
                        is_keytask = true;
                    }
                }
                if(ret_change == nullptr)
                {
                    for(auto it = writers_queue_.begin(); nullptr == ret_change && it != writers_queue_.end(); ++it)
                    {
                        ret_change = it->second.get_next_change();
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
        for (auto& queue : writers_queue_)
        {
            queue.second.add_interested_changes_to_queue();
        }
    }

    void switchQueue()
    {
        is_switch = !is_switch;
    }
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
    void judge_switch(long double rtt)
    {
        int time_queue_len = queue_.get_time_size();
        if(time_queue_len == 0)
        {
            is_switch = false;
            return;
        }
        ave_trans_time = queue_.calc_ave_trans_time();
        calc_t_max_min(rtt);
        if (is_switch)
        {
            if (ave_trans_time > t_min && ave_trans_time < t_max)
            {
                if(queue_.get_time_size() != 0)
                {
                    fastrtps::rtps::CacheChange_t* temp = queue_.get_down_change();
                    int time_len = queue_.get_time_size();
                    eprosima::fastrtps::rtps::Time_t now; 
                    eprosima::fastrtps::rtps::Time_t::now(now);
                    while(time_len--)
                    {
                        if(temp->priority == 0)
                        {
                            if((now - temp->sourceTimestamp).to_ns() > r_0_0 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                            {
                                return;
                            }
                        }
                        else if(temp->priority == 1)
                        {
                            if((now - temp->sourceTimestamp).to_ns() > r_0_2 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                            {
                                return;
                            }
                        }
                        else if(temp->priority == 6)
                        {
                            if((now - temp->sourceTimestamp).to_ns() > r_0_4 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                            {
                                return;
                            }
                        }
                        temp = temp->writer_info.later;
                    }
                }
                is_switch = !is_switch;
                is_keytask = true;
                return;
            }
            else if (ave_trans_time < t_min)
            {
                is_switch = !is_switch;
                eprosima::fastrtps::rtps::Time_t now; 
                eprosima::fastrtps::rtps::Time_t::now(now);
                int temp_priority = 0;
                auto queue_0 = writers_queue_.find(temp_priority);
                int len = queue_0->second.get_size();
                fastrtps::rtps::CacheChange_t* queue_0_change = queue_0->second.get_next_change();
                bool temp = true;
                while(len--)
                {

                    if((now - queue_0_change->sourceTimestamp).to_ns() > r_1 * (1.0 * queue_0_change->maxTransportTime.to_ns() - rtt/2))
                    {
                        temp = false;
                        break;
                    }
                    queue_0_change = queue_0_change->writer_info.next;
                }
                if(temp == true)
                {
                    is_keytask = false;
                }
		        return;
            }
        }
        else
        {
	        if (ave_trans_time > t_max)
            {
                
                is_switch = !is_switch;
                is_time_max = true;
		        return;
            }
            else if(ave_trans_time < t_min)
            {
                eprosima::fastrtps::rtps::Time_t now; 
                eprosima::fastrtps::rtps::Time_t::now(now);
                int temp_priority = 0;
                auto queue_0 = writers_queue_.find(temp_priority);
                int len = queue_0->second.get_size();
                fastrtps::rtps::CacheChange_t* queue_0_change = queue_0->second.get_next_change();
                bool temp = true;
                while(len--)
                {
       
                    if((now - queue_0_change->sourceTimestamp).to_ns() > r_1 * (1.0 * queue_0_change->maxTransportTime.to_ns() - rtt/2))
                    {
                        break;
                    }
                    queue_0_change = queue_0_change->writer_info.next;
                }
                if(temp == true)
                {
                    is_keytask = false;
                }
		        return;
            }
            else if(queue_.get_time_size() != 0)
            {
                fastrtps::rtps::CacheChange_t* temp = queue_.get_down_change();
                int time_len = queue_.get_time_size();
                eprosima::fastrtps::rtps::Time_t now; 
                eprosima::fastrtps::rtps::Time_t::now(now);
                while(time_len--)
                {
                    if(temp->priority == 0)
                    {
                        if((now - temp->sourceTimestamp).to_ns() > r_0_0 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                        {
                            is_switch = !is_switch;
                            return;
                        }
                    }
                    else if(temp->priority == 1)
                    {
                        if((now - temp->sourceTimestamp).to_ns() > r_0_2 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                        {
                            is_switch = !is_switch;
                            return;
                        }
                    }
                    else if(temp->priority == 6)
                    {
                        if((now - temp->sourceTimestamp).to_ns() > r_0_4 * (1.0 * temp->maxTransportTime.to_ns() - rtt/2))
                        {
                            is_switch = !is_switch;
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
    const double r_min_0 = 0.2286;
    const double r_max_2 = 0.6;
    const double r_min_2 = 0.2;
    const double r_max_4 = 0.8571;
    const double r_min_4 = 0.3714;
    const double r_0_0 = 0.6429;
    const double r_0_2 = 0.6;
    const double r_0_4 = 0.8571;
    const double r_1 = 0.2286;
};

// ! Fifo scheduling
struct FlowControllerFifoSchedule
{
    void register_writer(
            fastrtps::rtps::RTPSWriter*) const
    {
    }

    void unregister_writer(
            fastrtps::rtps::RTPSWriter*) const
    {
    }

    void work_done() const
    {
        // Do nothing
    }

    void add_new_sample(
            fastrtps::rtps::RTPSWriter*,
            fastrtps::rtps::CacheChange_t* change)
    {
        // std::cout << "==FlowControllerFifoSchedule::add_new_sample(): 向FlowQueue中添加change==" << std::endl;
        queue_.add_new_sample(change);
        int len = queue_.get_size();

    }

    void add_old_sample(
            fastrtps::rtps::RTPSWriter*,
            fastrtps::rtps::CacheChange_t* change)
    {
        queue_.add_old_sample(change);
    }

    /*!
     * Returns the first sample in the queue.
     * Default behaviour.
     * Expects the queue is ordered.
     *
     * @return Pointer to next change to be sent. nullptr implies there is no sample to be sent or is forbidden due to
     * bandwidth exceeded.
     */
    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        int len = queue_.get_size();
        return queue_.get_next_change();
    }

    /*!
     * Store the sample at the end of the list.
     *
     * @return true if there is added changes.
     */
    void add_interested_changes_to_queue_nts()
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        queue_.add_interested_changes_to_queue();
    }

    void set_bandwith_limitation(
            uint32_t) const
    {
    }

    void trigger_bandwidth_limit_reset() const
    {
    }

private:

    //! Scheduler queue. FIFO scheduler only has one queue.
    FlowQueue queue_;
};
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
            // There are writers interested in removing a sample.
            if (0 != async_mode.writers_interested_in_remove)
            {
                continue;
            }

            std::unique_lock<std::mutex> lock(mutex_);
            fastrtps::rtps::CacheChange_t* change_to_process = nullptr;

            //Check if we have to sleep.
            {
                std::unique_lock<std::mutex> in_lock(async_mode.changes_interested_mutex);
                // Add interested changes into the queue.
                sched.add_interested_changes_to_queue_nts();

                while (async_mode.running &&
                        (async_mode.force_wait() || nullptr == (change_to_process = sched.get_next_change_nts())))
                {
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
                }
            }
            fastrtps::rtps::RTPSWriter* current_writer = nullptr;
            while (nullptr != change_to_process)
            {
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
                    if (top != nullptr && bottom != nullptr)
                    {
                        top->writer_info.later = change_to_process;
                        bottom->writer_info.earlier = change_to_process;
                        change_to_process->writer_info.earlier = top;
                        change_to_process->writer_info.later = bottom;
                    }
                    
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
                if (change_to_process == nullptr)
                {
                }
            }

            async_mode.group.sender(nullptr, nullptr);
        }
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
