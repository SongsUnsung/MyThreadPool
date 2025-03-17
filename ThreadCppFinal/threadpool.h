#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unordered_map>
#include<thread>
#include<future>
#include<iostream>

// 任务队列最大阈值
const int TASK_MAX_THRESHHOLD = INT32_MAX;
// 线程池最大线程数阈值（仅CACHED模式有效）
const int THREAD_MAX_THRESHHOLD = 10;
// 线程最大空闲时间（秒，CACHED模式回收线程用）
const int THREAD_MAX_IDLE_TIME = 60;

// 线程池工作模式枚举
enum class PoolMode {
    MODE_FIXED,   // 固定数量线程
    MODE_CACHED,  // 动态数量线程
};

// 线程类封装
class Thread {
public:
    // 线程函数对象类型（接受线程ID参数）
    using ThreadFunc = std::function<void(int)>;

    // 构造函数（初始化线程函数和生成唯一线程ID）
    Thread(ThreadFunc func)
        : func_(func)
        , threadId_(generateId_++)  // 使用静态原子变量生成唯一ID
    {}

    ~Thread() = default;

    // 启动线程（立即分离运行）
    void start() {
        std::thread t(func_, threadId_);  // 创建线程并传入线程ID
        t.detach();  // 分离线程使其自主运行
    }

    // 获取线程ID
    int getId() const {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generateId_;  // 静态线程ID生成器
    int threadId_;           // 当前线程ID
};

// 静态成员初始化（线程ID从0开始）
int Thread::generateId_ = 0;

// 线程池核心类
class ThreadPool {
public:
    // 默认构造函数（初始化各种参数）
    ThreadPool()
        : initThreadSize_(4)           // 初始线程数
        , taskSize_(0)                // 当前任务数
        , idleThreadSize_(0)          // 空闲线程数
        , curThreadSize_(0)            // 当前总线程数
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)  // 任务队列最大容量
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)// 最大线程数阈值
        , poolMode_(PoolMode::MODE_FIXED)  // 默认固定模式
        , isPoolRunning_(false)        // 运行状态标记
    {}

    // 析构函数（安全关闭线程池）
    ~ThreadPool() {
        isPoolRunning_ = false;
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();  // 唤醒所有等待线程
        exitCond_.wait(lock, [&]()->bool { 
            return threads_.size() == 0; // 等待所有线程退出
        });
    }

    // 设置线程池模式（运行中不可修改）
    void setMode(PoolMode mode) {
        if (checkRunningState()) return;
        poolMode_ = mode;
    }

    // 设置初始线程数（运行前设置）
    void setInitThreadSize(int size) {
        initThreadSize_ = size;
    }

    // 设置任务队列最大容量（运行中不可修改）
    void setTaskQueMaxThreshHold(int threshHold) {
        if (checkRunningState()) return;
        taskQueMaxThreshHold_ = threshHold;
    }

    // 设置线程数阈值（仅CACHED模式有效）
    void setThreadSizeThreshHold(int threshHold) {
        if (checkRunningState()) return;
        if (poolMode_ == PoolMode::MODE_CACHED)
            threadSizeThreshHold_ = threshHold;
    }

    // 提交任务接口（可变参数模板+完美转发）
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
        // 推导任务返回类型
        using RType = decltype(func(args...));
        
        // 封装任务到packaged_task（支持获取future）
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        
        // 获取future对象用于异步获取结果
        std::future<RType> result = task->get_future();

        // 加锁操作任务队列
        std::unique_lock<std::mutex> lock(taskQueMtx_);

        // 任务队列满时等待1秒（带超时的条件变量等待）
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) 
        {
            // 超时后返回空任务future
            std::cerr << "任务队列已满，提交失败" << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType { return RType(); });
            (*task)();
            return task->get_future();
        }

        // 任务入队（封装为void()类型）
        taskQue_.emplace([task]() {
            (*task)();  // 执行打包的任务
        });
        taskSize_++;  // 原子计数增加

        // 通知消费者有任务可执行
        notEmpty_.notify_all();

        // CACHED模式动态创建线程逻辑：
        if (poolMode_ == PoolMode::MODE_CACHED 
            && taskSize_ > idleThreadSize_  // 任务数超过空闲线程
            && curThreadSize_ < threadSizeThreshHold_) // 未达线程数上限
        {
            std::cout << ">>> 创建新线程..." << std::endl;
            // 创建新线程并启动
            auto ptr = std::make_unique<Thread>(
                std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            threads_[threadId]->start();  // 启动线程
            curThreadSize_++;    // 当前线程数增加
            idleThreadSize_++;  // 空闲数增加（新线程会立即取任务）
        }

        return result;  // 返回future给调用者
    }

    // 启动线程池（可指定初始线程数）
    void start(int initThreadSize = int(std::thread::hardware_concurrency())) {
        isPoolRunning_ = true;  // 设置运行标志
        initThreadSize_ = initThreadSize;  // 可覆盖初始线程数
        curThreadSize_ = initThreadSize;   // 设置当前线程数

        // 创建指定数量的线程对象
        for (int i = 0; i < initThreadSize_; i++) {
            // 绑定线程函数threadFunc，传入线程ID
            auto ptr = std::make_unique<Thread>(
                std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));  // 存入线程表
        }
        
        // 启动所有线程
        for (int i = 0; i < initThreadSize_; i++) {
            threads_[i]->start();  // 启动线程（detach运行）
            idleThreadSize_++;     // 空闲数递增
        }
    }

    // 禁止拷贝构造和赋值
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 线程函数（所有工作线程执行此函数）
    void threadFunc(int threadid) {
        auto lastTime = std::chrono::high_resolution_clock().now(); // 记录最后活跃时间
        
        // 无限循环处理任务
        for (;;) {
            Task task;  // 定义任务对象
            
            {   // 加锁区开始
                std::unique_lock<std::mutex> lock(taskQueMtx_);
                std::cout << "线程ID:" << std::this_thread::get_id() 
                          << " 尝试获取任务..." << std::endl;

                // 等待任务队列非空（带退出机制）
                while (taskQue_.size() == 0) {
                    // 检查线程池是否关闭
                    if (!isPoolRunning_) {
                        threads_.erase(threadid);  // 从线程表移除
                        std::cout << "线程ID:" << std::this_thread::get_id()
                                  << " 退出！" << std::endl;
                        exitCond_.notify_all();  // 通知析构函数
                        return;  // 直接结束线程函数
                    }

                    // CACHED模式处理线程超时回收
                    if (poolMode_ == PoolMode::MODE_CACHED) {
                        // 带超时的等待（1秒）
                        if (std::cv_status::timeout == 
                            notEmpty_.wait_for(lock, std::chrono::seconds(1))) 
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(
                                now - lastTime);
                            // 超时且空闲超时，且线程数大于初始值
                            if (dur.count() >= THREAD_MAX_IDLE_TIME 
                                && curThreadSize_ > initThreadSize_) 
                            {
                                threads_.erase(threadid);  // 移出线程表
                                curThreadSize_--;    // 当前线程数减1
                                idleThreadSize_--;  // 空闲数减1
                                std::cout << "线程ID:" << std::this_thread::get_id()
                                          << " 因空闲超时退出！" << std::endl;
                                return;  // 结束线程函数
                            }
                        }
                    }
                    else {  // FIXED模式无限等待
                        notEmpty_.wait(lock);
                    }
                }  // end while

                // 获取任务前减少空闲计数
                idleThreadSize_--;

                std::cout << "线程ID:" << std::this_thread::get_id() 
                          << " 获取任务成功！" << std::endl;

                // 从队列取出任务
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;  // 原子计数减1

                // 通知生产者可以继续提交任务
                if (taskQue_.size() > 0) {
                    notEmpty_.notify_all();  // 还有其他任务
                }

                notFull_.notify_all();  // 队列有空位
            }  // 自动释放锁

            // 执行任务（在锁外执行）
            if (task != nullptr) {
                task();  // 执行函数对象
            }

            // 执行完成后更新空闲计数和时间戳
            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now();
        }  // end for
    }

    // 检查线程池是否在运行
    bool checkRunningState() const {
        return isPoolRunning_;
    }

private:
    // 线程列表（使用线程ID映射管理）
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;

    // 初始线程数量
    int initThreadSize_;
    // 线程数量上限阈值
    int threadSizeThreshHold_;
    // 当前线程总数（原子操作）
    std::atomic_int curThreadSize_;
    // 空闲线程数量（原子操作）
    std::atomic_int idleThreadSize_;

    // 任务队列相关
    using Task = std::function<void()>;  // 任务类型
    std::queue<Task> taskQue_;          // 任务队列
    std::atomic_int taskSize_;          // 当前任务数
    int taskQueMaxThreshHold_;          // 任务队列最大容量

    // 同步机制
    std::mutex taskQueMtx_;              // 任务队列互斥锁
    std::condition_variable notFull_;    // 队列未满条件变量
    std::condition_variable notEmpty_;   // 队列非空条件变量
    std::condition_variable exitCond_;   // 线程退出通知条件变量

    // 运行模式
    PoolMode poolMode_;
    // 线程池运行状态（原子标记）
    std::atomic_bool isPoolRunning_;
};
#endif