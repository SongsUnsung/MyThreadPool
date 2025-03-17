#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include <functional>
#include<unordered_map>
#include<thread>

class Any{
    public: 

    Any()=default;
    ~Any()=default;
    Any(const Any&)=delete;
    Any& operator=(const Any&)=delete;
    Any(Any&&)=default;
    Any& operator=(Any&&)=default;

    template<typename T>
    Any(T data):base_(std::make_unique<Derive<T>>(data)){}

    template<typename T>
    T cast_(){
        Derive<T>*pd=dynamic_cast<Derive<T>*>(base_.get());
        if(pd==nullptr){
            throw"type is unmatch!";
        }
        return pd->data_;
    }
    private:
    class Base{
        public:
        virtual ~Base()=default;
    };

    template<typename T>
    class Derive:public Base{
        public:
        Derive(T data):data_(data){}

    

        T data_;

        
    };
    private:
    std::unique_ptr<Base>base_;
};

class Semaphore{
    public:
    Semaphore(int limit=0)
        :resLimit_(limit)
        ,isExit_(false)
    {}
    ~Semaphore(){
        isExit_=true;
    }
    void wait(){
        if(isExit_)return;
        std::unique_lock<std::mutex>lock(mtx_);
        cond_.wait(lock,[&]()->bool{return resLimit_>0;});
        resLimit_--;
    }
    void post(){
        if(isExit_)return;
        std::unique_lock<std::mutex>lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }
    private:
    std::atomic_bool isExit_;
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;

};
class Task;
class Result{
    public:
    Result(std::shared_ptr<Task>task,bool isValid=true);
    ~Result()=default;
    void setVal(Any any);
    Any get();
    private:
    Any any_;
    Semaphore sem_;
    std::shared_ptr<Task>task_;
    std::atomic_bool isValid_;

};

class Task{
    public:
    Task();
    ~Task()=default;
    void exec();
    void setResult(Result*res);
    virtual Any run()=0;

    private:
    Result * result_;
};

enum class PoolMode{
    MODE_FIXED,
    MODE_CACHED,
};

class Thread{
    public:
    using ThreadFunc=std::function<void(int)>;

    Thread(ThreadFunc func);

    ~Thread();

    void start();

    int  getId()const;
    private:
    
    ThreadFunc func_;
    static int generateId_;
    int threadId_;

};

class ThreadPool{
    public:
    ThreadPool();

    ~ThreadPool();

    void setMode(PoolMode mode);

    void setInitThreadSize(int Size);

    void setTaskQueMaxThreshHold(int threshHold);

    void setThreadSizeThreshHold(int threshHold);

    Result submitTask(std::shared_ptr<Task>sp);

    void start(int initThreadSize=int(std::thread::hardware_concurrency()));

    ThreadPool(const ThreadPool&)=delete;
    ThreadPool& operator=(const ThreadPool&)=delete;


    private:
    void threadFunc(int threadid);

    bool checkRunningState()const;

    private:

    //std::vector<std::unique_ptr<Thread>>threads_;
    std::unordered_map<int,std::unique_ptr<Thread>>threads_;

    int initThreadSize_;
    std::atomic_int curThreadSize_;
    int threadSizeThreshHold_;
    std::atomic_int idleThreadSize_;

    std::queue<std::shared_ptr<Task>>taskQue_;
    std::atomic_int taskSize_;
    int taskQueMaxThreshHold_;

    std::mutex taskQueMtx_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::condition_variable exitCond_;

    PoolMode poolMode_;
    std::atomic_bool isPoolRunning_;
    
};
#endif 