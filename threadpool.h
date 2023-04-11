#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,     // Fixed模式: 固定数量的线程
    MODE_CACHED,    // Cached模式: 线程数量可以动态的增长
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造函数
    Thread(ThreadFunc func);

    //线程析构
    ~Thread() = default;

    // 启动线程
    void start();

    // 获取线程id
    int getId()const;

private:
    ThreadFunc func_;   // 线程函数
    static int generateId_; // 生成线程id
    int threadId_;  // 线程id
};

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool();

    // 线程池析构
    ~ThreadPool();

    // 拷贝构造函数禁用
    ThreadPool(const ThreadPool&) = delete;

    // 拷贝赋值运算符禁用
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 设置任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshhold);

    // 设置线程池cached模式下, 线程的上限阈值
    void setThreadSizeThreshHold(int threshhold);

    // 开启线程池, 初始线程数量为CPU核心数量
    void start(int initThreadSize);

    // 使用可变参模板编程，给线程池提交任务
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        /*
        ThreadPool::submitTask 接受一个函数对象func, 和一系列参数Args, 返回一个std::future对象
        decltype(func(args...)) 用于推断 func(args...) 表达式的返回类型
        该函数将任务封装成一个 std::packaged_task 对象，然后将该对象提交给线程池进行处理
        */

        // 打包任务
        using Rtype = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<Rtype()>>(
            // std::forward 保持实参的值类别（左值或右值）不变, 以避免不必要的拷贝和移动操作
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
        );
        std::future<Rtype> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        // 用户提交任务不能无限阻塞, 此处设定最长阻塞时间为1s, 否则提交任务失败
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&](){
                // 阻塞当前线程, 直到任务队列的大小小于 taskQueMaxThreshHold_ 或者等待超时为止
                return taskQue_.size() < (size_t)taskQueMaxThreshHold_;
            }))
        {
            // notFull_等待了1s, 条件依然没有满足
            std::cerr << "The task queue is full and waiting for more than one second. The task fails to be submitted" << std::endl;
            // 返回一个空对象
            auto task = std::make_shared<std::packaged_task<Rtype()>>(
                []()->Rtype {
                    return Rtype();
                }
            );
            (*task)();
            return task->get_future();
        }

        // 此时不存在任务队列满的问题了, 可以把任务放入任务队列中
        taskQue_.emplace([task]() {(*task)();});
        taskSize_++;

        // 放入了任务, 此时任务队列肯定不空了
        notEmpty_.notify_all();

        // Cached模式下, 有可能需要创建新的线程, 但也不能无限创建, 所以要小于threadSizeThreshHold_
        if (poolMode_ == PoolMode::MODE_CACHED
            && taskSize_ > idleThreadSize_
            && curThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << "Creating a new thread..." << std::endl;

            // 创建新的线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            // 添加到任务队列
            threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            // 修改相关计数的变量
            curThreadSize_++;
            idleThreadSize_++;
        }
        return result;
    }

private:
    // 检查线程池的运行状态
    bool checkPoolRunningState()const;

    // 定义线程函数
    void threadFunc(int threadid);

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;  // 线程列表

    int initThreadSize_;    // 初始的线程数量
    int threadSizeThreshHold_;  // 线程数量的上限阈值
    std::atomic_int curThreadSize_; // 记录当前线程池里面的线程总数量
    std::atomic_int idleThreadSize_;    // 记录空闲线程的数量

    using Task = std::function<void()>;
    std::queue<Task> taskQue_;  // 任务队列
    std::atomic_int taskSize_;  // 任务数量
    int taskQueMaxThreshHold_;  // 任务队列大小

    std::mutex taskQueMtx_; // 互斥变量，保证任务队列的线程安全
    std::condition_variable notFull_;   // 表示任务队列目前还没满
    std::condition_variable notEmpty_;  // 表示任务队列目前不是空的
    std::condition_variable exitCond_;  // 保证线程池析构时资源全部回收的条件变量

    PoolMode poolMode_; // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_;    // 表示当前线程池的启动状态
};

#endif
