#include <iostream>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <queue>
#include <functional>
#include <map>
#include <random>
#include <cmath>
#include <chrono>
#include <atomic>

enum Task_Status
{
    NotFound,
    NotStarted,
    InProgress,
    Completed
};

using read_write_lock = std::shared_mutex;
using read_lock = std::shared_lock<read_write_lock>;
using write_lock = std::unique_lock<read_write_lock>;

template <typename task_type_T> 
struct task_queue {
private:
    using task_queue_implementation = std::queue<std::pair<unsigned int, task_type_T>>;
    mutable read_write_lock m_rw_lock;
    task_queue_implementation m_tasks;
    unsigned int ID_counter = 0;
    unsigned int editing_number = 0;
    unsigned int total_length = 0;

private:
    std::map<unsigned int, Task_Status> task_statuses;
    mutable read_write_lock task_rw_lock;

public:
    inline task_queue() = default;
    inline ~task_queue() { clear(); }

public:
    inline size_t size();
    inline bool empty();
    inline float get_average_length();

public:
    template <typename... arguments>
    inline unsigned int emplace(arguments&&... parameters);

    inline bool pop(unsigned int& ID, task_type_T& task);
    inline void clear();

public:
    inline bool add_status(unsigned int ID);
    inline void set_status(unsigned int ID, Task_Status status);
    inline Task_Status get_status(unsigned int ID);
};

class thread_pool {
public:
    inline thread_pool() = default;
    inline ~thread_pool() { terminate(); }

public:
    void initialize(const size_t worker_count);
    void terminate();
    void routine();

    bool working() const;
    bool working_unsafe() const;
    float get_avrg_len();

    void print_statistics();

public:
    template <typename task_t, typename... arguments>
    inline unsigned int add_task(task_t&& task, arguments&&... parameters);

public:
    Task_Status get_task_status(unsigned int ID);
    int get_task_results(unsigned int ID);

private:
    mutable read_write_lock m_rw_lock;
    mutable std::condition_variable_any m_task_writer;
    std::vector<std::thread> m_workers;

    task_queue<std::function<int()>> m_tasks;
    std::map<unsigned int, int> task_results;
    mutable read_write_lock result_rw_lock;

    bool m_initialized = false;
    bool m_terminated = false;

    std::atomic<int64_t> thread_wait_time{ 0 };
    std::atomic<int64_t> thread_work_time{ 0 };
    std::atomic<int> cycle_numbers{ 0 };
};


template<typename task_type_T>
inline size_t task_queue<task_type_T>::size()
{
    read_lock _(m_rw_lock);
    return m_tasks.size();
}

template<typename task_type_T>
inline bool task_queue<task_type_T>::empty()
{
    read_lock _(m_rw_lock);
    return m_tasks.empty();
}

template<typename task_type_T>
inline float task_queue<task_type_T>::get_average_length()
{
    read_lock _(m_rw_lock);
    return std::round((float)total_length / editing_number * 100) / 100;
}

template<typename task_type_T>
inline void task_queue<task_type_T>::clear()
{
    write_lock _(m_rw_lock);
    while (!m_tasks.empty()) {
        m_tasks.pop();
    }
    task_statuses.clear();
}

template<typename task_type_T>
inline bool task_queue<task_type_T>::add_status(unsigned int ID)
{
    write_lock _(task_rw_lock);
    if (task_statuses.count(ID) != 0)
        return false;
    std::cout << ID << ": " << "\033[31mNotStarted\033[0m" << std::endl;
    task_statuses[ID] = Task_Status::NotStarted;
    return true;
}

template<typename task_type_T>
inline void task_queue<task_type_T>::set_status(unsigned int ID, Task_Status status)
{
    write_lock _(task_rw_lock);
    auto it = task_statuses.find(ID);
    if (it != task_statuses.end()) {
        it->second = status;
        std::cout << ID << ": " << ((status == Task_Status::InProgress) ? "\033[33mInProgress\033[0m" : "\033[32mCompleted\033[0m") << std::endl;
    }
}

template<typename task_type_T>
inline Task_Status task_queue<task_type_T>::get_status(unsigned int ID)
{
    read_lock _(task_rw_lock);
    auto it = task_statuses.find(ID);
    if (it == task_statuses.end()) {
        return Task_Status::NotFound;
    }
    return it->second;
}

template <typename task_type_T>
bool task_queue<task_type_T>::pop(unsigned int& ID, task_type_T& task) {
    write_lock _(m_rw_lock);
    if (m_tasks.empty()) {
        return false;
    }
    editing_number++;
    total_length += m_tasks.size();
    std::tie(ID, task) = std::move(m_tasks.front());
    m_tasks.pop();
    return true;
}

template <typename task_type_T>
template <typename... arguments>
unsigned int task_queue<task_type_T>::emplace(arguments&&... parameters) {
    write_lock _(m_rw_lock);
    ID_counter++;
    editing_number++;
    total_length += m_tasks.size();
    m_tasks.emplace(ID_counter, std::forward<arguments>(parameters)...);
    add_status(ID_counter);
    return ID_counter;
}

void thread_pool::initialize(const size_t worker_count)
{
    write_lock _(m_rw_lock);
    if (m_initialized || m_terminated)
        return;

    m_workers.reserve(worker_count);
    for (size_t id = 0; id < worker_count; id++) {
        m_workers.emplace_back(&thread_pool::routine, this);
    }
    m_initialized = !m_workers.empty();
}

void thread_pool::terminate()
{
    {
        write_lock _(m_rw_lock);

        if (working_unsafe()) {
            m_terminated = true;
        }
        else {
            m_workers.clear();
            m_terminated = false;
            m_initialized = false;
            return;
        }
    }

    m_task_writer.notify_all();

    for (std::thread& worker : m_workers) {
        worker.join();
    }

    m_workers.clear();
    m_terminated = false;
    m_initialized = false;
}

void thread_pool::routine()
{
    while (true) {
        bool task_accquired = false;
        unsigned int task_ID = 0;
        std::function<int()> task;
        {
            write_lock _(m_rw_lock);
            auto wait_condition = [this, &task_accquired, &task, &task_ID] {
                task_accquired = m_tasks.pop(task_ID, task);
                return m_terminated || task_accquired;
                };
            auto wait_beggin = std::chrono::high_resolution_clock::now();
            m_task_writer.wait(_, wait_condition);
            auto wait_end = std::chrono::high_resolution_clock::now();
            thread_wait_time.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(wait_end - wait_beggin).count());
        }
        if (m_terminated && !task_accquired)
            return;
        m_tasks.set_status(task_ID, Task_Status::InProgress);
        auto task_beggin = std::chrono::high_resolution_clock::now();
        int a = task();
        auto task_end = std::chrono::high_resolution_clock::now();
        thread_work_time.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(task_end - task_beggin).count());
        write_lock _(result_rw_lock);
        task_results[task_ID] = a;
        m_tasks.set_status(task_ID, Task_Status::Completed);
        cycle_numbers.fetch_add(1);
    }
}

template<typename task_t, typename ...arguments>
inline unsigned int thread_pool::add_task(task_t&& task, arguments && ...parameters)
{
    {
        read_lock _(m_rw_lock);
        if (!working_unsafe()) {
            return 0;
        }
    }

    auto bind = std::bind(std::forward<task_t>(task), std::forward<arguments>(parameters)...);

    int task_ID = m_tasks.emplace(bind);
    m_task_writer.notify_one();
    return task_ID;
}

bool thread_pool::working() const
{
    read_lock _(m_rw_lock);
    return working_unsafe();
}

bool thread_pool::working_unsafe() const
{
    return !m_terminated && m_initialized;
}

float thread_pool::get_avrg_len()
{
    return m_tasks.get_average_length();
}

void thread_pool::print_statistics()
{
    std::cout << std::endl << std::endl << "Avrg len: " << get_avrg_len() << std::endl;
    if(cycle_numbers != 0)
    {
        double avg_wait_time = (double)thread_wait_time.load() / cycle_numbers / 1e6;
        double avg_work_time = (double)thread_work_time.load() / cycle_numbers / 1e6;

        std::cout << "\nThread wait avg time: " << std::round(avg_wait_time * 100) / 100 << " ms\n";
        std::cout << "Thread work avg time: " << std::round(avg_work_time * 100) / 100 << " ms\n";
    }
    std::cout << std::endl << std::endl;
}

Task_Status thread_pool::get_task_status(unsigned int ID)
{
    return m_tasks.get_status(ID);
}

int thread_pool::get_task_results(unsigned int ID)
{
    read_lock _(result_rw_lock);
    auto it = task_results.find(ID);
    if (it == task_results.end()) {
        throw std::runtime_error("Error: wrong task ID");
    }
    return it->second;
}

std::default_random_engine generator;
int thread_func() {
    std::uniform_int_distribution<int> distribution(5, 10);
    int sleep_time = distribution(generator);
    std::this_thread::sleep_for(std::chrono::seconds(sleep_time));
    return sleep_time;
}

void task_creation(thread_pool* pool, int task_number = INFINITY) {
    for (int i = 0; i < task_number; i++)
    {
        std::uniform_int_distribution<int> distribution(1, 5);
        std::this_thread::sleep_for(std::chrono::seconds(distribution(generator)));
        pool->add_task(thread_func);
    }
}

int main()
{
    thread_pool pool;
    pool.initialize(4);

    std::thread first_thread = std::thread(task_creation, &pool, 10);
    std::thread second_thread = std::thread(task_creation, &pool, 10);
    first_thread.join();
    second_thread.join();
    
    pool.terminate();
    pool.print_statistics();
}
