#include <iostream>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <queue>
#include <functional>

using read_write_lock = std::shared_mutex;
using read_lock = std::shared_lock<read_write_lock>;
using write_lock = std::unique_lock<read_write_lock>;

template <typename task_type_T> 
struct task_queue {
private:
    using task_queue_implementation = std::queue<task_type_T>;
    mutable read_write_lock m_rw_lock;
    task_queue_implementation task_queue;
public:
    inline task_queue() = default;
    inline ~task_queue() { clear(); }

public:
    inline size_t size();
    inline bool empty();

    template <typename... arguments>
    inline void emplace(arguments&&... parameters);

public:
    inline void clear();
    inline bool pop(task_type_T& task);
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
    inline void add_task(task_t&& task, arguments&&... parameters);

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
    return task_queue.size();
}

template<typename task_type_T>
inline bool task_queue<task_type_T>::empty()
{
    read_lock _(m_rw_lock);
    return task_queue.empty();
}

template<typename task_type_T>
inline void task_queue<task_type_T>::clear()
{
    write_lock _(m_rw_lock);
    while (!task_queue.empty()) {
        task_queue.pop();
    }
}

template <typename task_type_T>
bool task_queue<task_type_T>::pop(task_type_T& task) {
    write_lock _(m_rw_lock);
    if (task_queue.empty()) {
        return false;
    }
    else {
        task = std::move(task_queue.pop());
        return true;
    }
}

template <typename task_type_T>
template <typename... arguments>
void task_queue<task_type_T>::emplace(arguments&&... parameters) {
    write_lock _(m_rw_lock);
    task_queue.emplace(std::forward<arguments>(parameters)...);
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
        std::function<void()> task;
        {
            write_lock _(m_rw_lock);
            auto wait_condition = [this, &task_accquired, &task] {
                task_accquired = m_tasks.pop(task);
                return m_terminated || task_accquired;
                };

            m_task_writer.wait(_, wait_condition);
        }
        if (m_terminated && !task_accquired)
            return;
        task();
    }
}

template<typename task_t, typename ...arguments>
inline void thread_pool::add_task(task_t&& task, arguments && ...parameters)
{
    {
        read_lock _(m_rw_lock);
        if (!working_unsafe()) {
            return;
        }
    }

    auto bind = std::bind(std::forward<task_t>(task), std::forward<arguments>(parameters)...);

    m_tasks.emplace(bind);
    m_task_writer.notify_one();
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

int main()
{
    thread_pool pool;
    //add tasks
    pool.initialize(4);
}

enum Task_Status
{
    NotStarted,
    InProgress,
    Completed
};