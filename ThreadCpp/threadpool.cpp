#include"threadpool.h"
#include<functional>
#include<thread>
#include<iostream>
const int TASK_MAX_THRESHHOLD=INT32_MAX;
const int THREAD_MAX_THRESHHOLD=10;
const int THREAD_MAX_IDLE_TIME=60;

ThreadPool::ThreadPool()
    :initThreadSize_(4)
    ,taskSize_(0)
    ,idleThreadSize_(0)
    ,curThreadSize_(0)
    ,taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    ,threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    ,poolMode_(PoolMode::MODE_FIXED)
    ,isPoolRunning_(false)
    {}

ThreadPool::~ThreadPool(){
    isPoolRunning_=false;
    
    std::unique_lock<std::mutex>lock(taskQueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock,[&]()->bool{return threads_.size()==0;});
}

void ThreadPool::setMode(PoolMode mode){
    if(checkRunningState())return;
    poolMode_=mode;
}

void ThreadPool::setInitThreadSize(int size){
    initThreadSize_=size;
}

void ThreadPool::setTaskQueMaxThreshHold(int threshHold){
    if(checkRunningState())return;
    taskQueMaxThreshHold_=threshHold;
}
void ThreadPool::setThreadSizeThreshHold(int threshHold){
    if(checkRunningState())return;
    if(poolMode_== PoolMode::MODE_CACHED)threadSizeThreshHold_=threshHold;
}
Result ThreadPool::submitTask(std::shared_ptr<Task>sp){
    //获取锁
    std::unique_lock<std::mutex>lock(taskQueMtx_);
    //线程的通信

    if(!notFull_.wait_for(lock,std::chrono::seconds(1),
        [&]()->bool{
            return taskQue_.size()<(size_t)taskQueMaxThreshHold_;}))
    {
        std::cerr<<"task queue is full,submit task fail."<<std::endl;
        return Result(sp,false);
    }
    //如果有空余，任务放入任务队列中
    taskQue_.emplace(sp);
    taskSize_++;
    
    //放入任务队列后，在notEmpty_上通知
    notEmpty_.notify_all();

    //cached多且小的任务
    //TODO 根据任务和空闲线程的数量，判断是否需要创建新的线程出来?
    if(poolMode_==PoolMode::MODE_CACHED
        &&taskSize_>idleThreadSize_
        &&curThreadSize_<threadSizeThreshHold_)
    {   
        std::cout<<"create new thread"<<std::endl;
            auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
            int threadId=ptr->getId();
            threads_.emplace(threadId,std::move(ptr));
            threads_[threadId]->start();
            curThreadSize_++;
            idleThreadSize_++;
    }
    

    return Result(sp);

}

void ThreadPool::start(int initThreadSize){

    isPoolRunning_=true;

    initThreadSize_=initThreadSize;
    curThreadSize_=initThreadSize;

    for(int i=0;i<initThreadSize_;i++)
    {
        auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
        //threads_.emplace_back(std::move(ptr));
        int threadId=ptr->getId();
        threads_.emplace(threadId,std::move(ptr));
    }
    for(int i=0;i<initThreadSize_;i++)
    {
        threads_[i]->start();
        idleThreadSize_++;
    }
} 

void ThreadPool::threadFunc(int threadid){
    
   /*  std::cout<<"begin threadFunc tid:"
    <<std::this_thread::get_id()<<std::endl;

    std::cout<<"end threadFunc tid:"
    <<std::this_thread::get_id()<<std::endl; */
    auto lastTime=std::chrono::high_resolution_clock().now();

    for(;;){

        std::shared_ptr<Task>task;
        {//获取锁
        
            std::unique_lock<std::mutex>lock(taskQueMtx_);
            std::cout<<"tid:"<<std::this_thread::get_id()<<"尝试获取任务..."<<std::endl;
            
            //cached模式下，有可能已经创建了很多的线程
            //但是空闲时间超过60s,应该把多余的线程结束回收掉
           
            while(taskQue_.size()==0)
            {   
                if(!isPoolRunning_)
                {
                    threads_.erase(threadid);
                    std::cout<<"threadid:"<<std::this_thread::get_id()
                    <<"exit"<<std::endl;
                    exitCond_.notify_all();

                    return;
                }

                if(poolMode_==PoolMode::MODE_CACHED){

                    if(std::cv_status::timeout==
                        notEmpty_.wait_for(lock,std::chrono::seconds(1)))
                    {
                        auto now=std::chrono::high_resolution_clock().now();
                        auto dur=std::chrono::duration_cast<std::chrono::seconds>
                        (now-lastTime);
                        if(dur.count()>=THREAD_MAX_IDLE_TIME&&
                        curThreadSize_>initThreadSize_)
                        {
                            threads_.erase(threadid);
                            curThreadSize_--;
                            idleThreadSize_--;

                            std::cout<<"threadid:"<<std::this_thread::get_id()
                            <<"exit"<<std::endl;

                            return;
                        }
                    }
                }
                else
                {
                    notEmpty_.wait(lock);
                }
               /*  if(!isPoolRunning_){
                    threads_.erase(threadid);
                    curThreadSize_--;
                    idleThreadSize_--;

                    std::cout<<"threadid:"<<std::this_thread::get_id()
                    <<"exit"<<std::endl;
                    exitCond_.notify_all();
                    return;
                } */
            }  

        
            idleThreadSize_--;
            
            std::cout<<"tid:"<<std::this_thread::get_id()<<"获取任务成功"<<std::endl;
            //从任务队列取一个任务出来
            task=taskQue_.front();
            taskQue_.pop();
            taskSize_--;

            //先释放锁
            if(taskQue_.size()>0){
                notEmpty_.notify_all();
            }
            
            notFull_.notify_all(); 
        }


        //当前线程负责执行这个任务
        if(task!=nullptr){
            //task->run();
            task->exec();
        }
       
        idleThreadSize_++; 
        lastTime=std::chrono::high_resolution_clock().now();
        
    }

  

}

bool ThreadPool::checkRunningState()const{
    return isPoolRunning_;
}

int Thread::generateId_=0;

Thread::Thread(ThreadFunc func)
    :func_(func)
    ,threadId_(generateId_++)
{}

Thread::~Thread(){

}

//
void Thread::start(){
    std::thread t(func_,threadId_);
    t.detach();
}

int Thread::getId()const{
    return threadId_;
}


Task::Task()
    :result_(nullptr)
{}


void Task::exec(){
    if(result_!=nullptr)
    result_->setVal(run());
}

void Task::setResult(Result*res){
    result_=res;
}


Result::Result(std::shared_ptr<Task>task,bool isValid)
    :isValid_(isValid)
    ,task_(task)
{
    task_->setResult(this);
}

Any Result::get(){
    if(!isValid_){
        return "";
    }
    sem_.wait();
    return std::move(any_);
}

void Result::setVal(Any any){
    this->any_=std::move(any);
    sem_.post();
}