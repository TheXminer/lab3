#include <iostream>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <queue>
#include <functional>
#include <map>

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

private:
    std::map<unsigned int, Task_Status> task_statuses;
    mutable read_write_lock task_rw_lock;

public:
    inline task_queue() = default;
    inline ~task_queue() { clear(); }

public:
    inline size_t size();
    inline bool empty();

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

public:
    template <typename task_t, typename... arguments>
    inline unsigned int add_task(task_t&& task, arguments&&... parameters);

public:
    Task_Status get_task_status(unsigned int ID);

private:
    mutable read_write_lock m_rw_lock;
     mutable std::condition_variable_any m_task_writer;
    std::vector<std::thread> m_workers;

    task_queue<std::function<void()>> m_tasks;

    bool m_initialized = false;
    bool m_terminated = false;
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
    std::tie(ID, task) = std::move(m_tasks.front());
    m_tasks.pop();
    return true;
}

template <typename task_type_T>
template <typename... arguments>
unsigned int task_queue<task_type_T>::emplace(arguments&&... parameters) {
    write_lock _(m_rw_lock);
    ID_counter++;
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
        std::function<void()> task;
        {
            write_lock _(m_rw_lock);
            auto wait_condition = [this, &task_accquired, &task, &task_ID] {
                task_accquired = m_tasks.pop(task_ID, task);
                return m_terminated || task_accquired;
                };

            m_task_writer.wait(_, wait_condition);
        }
        if (m_terminated && !task_accquired)
            return;
        m_tasks.set_status(task_ID, Task_Status::InProgress);
        task();
        m_tasks.set_status(task_ID, Task_Status::Completed);
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

void thread_func() {
    int sleep_time = rand() % 6 + 5;
    std::this_thread::sleep_for(std::chrono::seconds(sleep_time));
}

int main()
{
    thread_pool pool;
    pool.initialize(4);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
    pool.add_task(thread_func);
}
