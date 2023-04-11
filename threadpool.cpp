#include "threadpool.h"

const int TASK_MAX_THRESHHOLD = INT32_MAX;  // 定义任务队列的大小
const int THREAD_MAX_THRESHHOLD = 1024; // 线程数量上限
const int THREAD_MAX_IDLE_TIME = 60;    // 闲置线程的最大存活时间

// 线程构造
Thread::Thread(ThreadFunc func)
    : func_(func)
    , threadId_(generateId_++)
{}

// 启动线程
void Thread::start()
{
    // 创建线程, 执行线程函数 pthread_create
    // C++11中 线程对象t, 线程函数func_
    std::thread t(func_, threadId_);
    
    // 设置分离线程
    t.detach();
}

// 获取线程id
int Thread::getId()const
{
    return threadId_;
}

// 初始化最小的线程id为0
int Thread::generateId_ = 0;

// 线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(0)    // 初始时线程数量为0
    , taskSize_(0)          // 初始任务数量为0
    , idleThreadSize_(0)    // 初始时先定为0, 具体数量在线程池启动时再进行统计
    , curThreadSize_(0)     // 总线程数量, 线程池还没启动, 初始为0
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)    // 任务队列大小
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)  // 线程数量上限
    , poolMode_(PoolMode::MODE_FIXED)   // 线程池默认Fixed模式
    , isPoolRunning_(false) // 初始时线程池并未启动, 默认为false
{}

// 线程池析构
ThreadPool::~ThreadPool()
{
    // 先改变线程池的运行状态
    isPoolRunning_ = false;

    // 等待线程池里面所有的线程返回, 此处线程有两种状态: 阻塞 / 正在执行任务中
    // 先进行上锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    // 唤醒所有正在等待 notEmpty_ 条件变量的线程
    notEmpty_.notify_all();
    // 等待所有线程结束后, 解锁互斥量并继续执行后面的代码
    exitCond_.wait(lock, [&]()->bool {
        // lambda表达式为true时, 所有线程已经结束, 释放lock资源
        return threads_.size() == 0;
    });
}

// 检查线程池的运行状态
bool ThreadPool::checkPoolRunningState()const
{
    return isPoolRunning_;
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkPoolRunningState()) return;
    poolMode_ = mode;
}

// 设置任务队列的上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
    if (checkPoolRunningState()) return;
    taskQueMaxThreshHold_ = threshhold;
}

// 设置 Cached 模式下线程阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
    if (checkPoolRunningState()) return;
    // 需要在Cached模式下才进行设置
    if (poolMode_ == PoolMode::MODE_CACHED)
    {
        threadSizeThreshHold_ = threshhold;
    }
    else
    {
        std::cerr << "The thread pool mode is not Cached mode." << std::endl;
    }
}

// 线程池启动函数
void ThreadPool::start(int initThreadSize = std::thread::hardware_concurrency())
{
    // 修改线程池运行的状态为true
    isPoolRunning_ = true;

    // 对相关值进行初始化
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;
    idleThreadSize_ = initThreadSize;

    // 根据初始值创建线程对象
    for (int i = 0; i < initThreadSize_; i++)
    {
        /*
        std::make_unique 是一个 C++14 中的函数模板, 用于创建一个指向动态分配对象的 std::unique_ptr
        这里先创建一个指向 Thread 对象的 std::unique_ptr, 并将其初始化
        std::placeholders::_1 是占位符, 在这里表示一个占位符, 因为 ThreadPool::threadFunc 中只有一个参数
        通过 std::bind , 将当前对象传递给 threadFunc 函数
        */
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
    }
    // 启动所有线程
    for (int i = 0; i < initThreadSize_; i++)
    {
        threads_[i]->start();
    }
}

// 定义线程函数
void ThreadPool::threadFunc(int threadid)
{
    // 获取当前时间, 精度为纳秒级别
    auto lastTime = std::chrono::high_resolution_clock().now();

    // 所有任务都执行完成, 线程池才可以回收线程资源
    while (1)
    {
        Task task;
        {
            // 修改临界区先进行锁的获取
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid: " << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

            while (taskQue_.size() == 0)
            {
                // 线程池结束, 开始回收资源
                if (!isPoolRunning_)
                {
                    threads_.erase(threadid);
                    std::cout << "thread id: " << std::this_thread::get_id() << "Exit." << std::endl;
                    exitCond_.notify_all();
                    return; // 线程函数结束, 线程结束
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    // 超时
                    if (std::cv_status::timeout == 
                        notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();  // 当前时间
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);    // 计算时间差
                        // 判断是否需要回收线程
                        if (dur.count() >= THREAD_MAX_IDLE_TIME
                            && curThreadSize_ > initThreadSize_)
                        {
                            // 开始回收线程
                            // 删除思路: threadid -> thread对象 -> 删除 -> 修改相关计数变量
                            threads_.erase(threadid);
                            curThreadSize_--;
                            idleThreadSize_--;

                            std::cout << "thread id: " << std::this_thread::get_id() << "Exit." << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    // 等待notEmpty_条件
                    notEmpty_.wait(lock);
                }
            }

            // 任务队列里有任务
            // 现在分配一个空闲线程给这个任务
            idleThreadSize_--;

            std::cout << "tid: " << std::this_thread::get_id() << "获取任务成功..." << std::endl;

            // 从任务队列取一个任务出来
            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--;

            // 如果此时依然有剩余任务, 通知其他线程执行任务
            if (taskQue_.size() > 0)
            {
                notEmpty_.notify_all();
            }

            // 取出一个任务, 任务队列必然不满, 进行通知, 可以继续提交生产任务
            notFull_.notify_all();
        }   // 离开作用域, 此时锁资源释放
        
        // 当前线程负责执行这个任务
        if (task != nullptr)
        {
            task(); // 执行function<void()>
        }

        // 任务执行完, 恢复空闲线程的计数
        idleThreadSize_++;
        // 更新线程执行完任务的时间
        lastTime = std::chrono::high_resolution_clock().now();
    }
}
