#include<iostream>
#include<chrono>
#include<thread>
#include"threadpool.h"
/*
有些场景，是希望能够获取线程执行任务的返回值的
举例：
1+...+30000的和
thread1 1+...+10000
thread2 10001+...+20000
......
main thread: 给每一个线程分配计算的区间，并等待他们算完返回结果，合并最终的结果；

*/
class MyTask : public Task {
public:
    MyTask(int begin, int end) : begin_(begin), end_(end) {}
    Any run() {
        std::cout << "tid:" << std::this_thread::get_id() << "begin!" << std::endl;
        //std::this_thread::sleep_for(std::chrono::seconds(5));
        int sum = 0;
        for (int i = begin_; i <= end_; i++) {
            sum += i;
        }
        std::cout << "tid:" << std::this_thread::get_id() << "end!" << std::endl;
        return sum;
    }
private:
    int begin_;
    int end_;
};

int main() {
    ThreadPool pool;
    pool.start(4);

    Result res1= pool.submitTask(std::make_shared<MyTask>(1, 10000));
    int sum1 = res1.get().cast<int>();
    std::cout << sum1 << std::endl;

    std::cout << "main over" << std::endl;

#if 0
    //问题：ThreadPool对象析构以后，怎样把线程池相关的线程资源全部回收？
    {
        ThreadPool pool;
        //用户自己设置线程池的工作模式
        pool.setMode(PoolMode::MODE_CACHED);
        //启动线程池
        pool.start(4);

        //-std=c++17就不会出现result的这些报错
        Result res1= pool.submitTask(std::make_shared<MyTask>(1, 10000));
        Result res2= pool.submitTask(std::make_shared<MyTask>(10001, 20000));
        Result res3= pool.submitTask(std::make_shared<MyTask>(20001, 30000));

        int sum1 = res1.get().cast<int>();
        int sum2 = res2.get().cast<int>();
        int sum3 = res3.get().cast<int>();

        //Master-Slave线程模型
        //Master线程用来分解任务，然后给各个Slave线程分配任务
        //等待各个Slave线程执行完任务，返回结果
        //Master线程合并各个任务结果，输出
        std::cout << (sum1 + sum2 + sum3) << std::endl;

        /*pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());
        pool.submitTask(std::make_shared<MyTask>());*/
    }
    getchar();
    //std::this_thread::sleep_for(std::chrono::seconds(5));
#endif
    
}

