#include<iostream>
#include<chrono>
#include<thread>
#include"threadpool.h"

using uLong=unsigned long long;

class MyTask:public Task{
    public:
    //MyTask() = default;
    MyTask(int begin,int end)
        :begin_(begin)
        ,end_(end)
    {}
    Any run(){
            std::cout<<"tid:"
        <<std::this_thread::get_id()<<"begin!"<<std::endl;

    // std::this_thread::sleep_for(std::chrono::seconds(2));

    uLong sum=0;
        for(uLong i=begin_;i<=end_;i++)sum+=i;

        std::cout<<"tid:"
        <<std::this_thread::get_id()<<"end!"<<std::endl;

        return sum;
    }
    private:
    int begin_;
    int end_;
};
int main(){
    {
    ThreadPool pool;

    pool.start(4);

    Result res1=pool.submitTask(std::make_shared<MyTask>(1,10000000));

    uLong sum1=res1.get().cast_<uLong>();

    std::cout<<sum1<<std::endl;
    }
    std::cout<<"main over"<<std::endl;

#if 0
    ThreadPool pool;
    //用户自己设置线程池的工作模式

    pool.setMode(PoolMode::MODE_CACHED);

    pool.start(0);
    Result res1 =pool.submitTask(std::make_shared<MyTask>(1,100000000));
    Result res2 =pool.submitTask(std::make_shared<MyTask>(100000001,200000000));
    Result res3 =pool.submitTask(std::make_shared<MyTask>(200000001,300000000));

    uLong sum1=res1.get().cast_<uLong>();

    uLong sum2=res2.get().cast_<uLong>();
    uLong sum3=res3.get().cast_<uLong>();

    /* pool.submitTask(std::make_shared<MyTask>());
    pool.submitTask(std::make_shared<MyTask>()); */

    //std::this_thread::sleep_for(std::chrono::seconds(5));
    std::cout<<sum1+sum2+sum3<<std::endl;

    getchar();
#endif


}