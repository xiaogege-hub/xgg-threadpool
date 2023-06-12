#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<iostream>
#include<queue>
#include<memory>
#include<atomic>
#include<vector>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unordered_map>
#include<thread>
#include<future>

const int TASK_MAX_THRESHHOLD = 2;//1024;
const int THREAD_MAX_THRESHHOLD = 10;   //线程数量最好是和核数是一样的
const int THREAD_MAX_IDLE_TIME = 60;    //单位：秒

//线程池支持的模式
enum class PoolMode{
    MODE_FIXED,     //固定数量的线程
    MODE_CACHED     //线程数量可动态增长的
};

//---------------------------线程类型---------------------------
class Thread{
public:
    using ThreadFunc = std::function<void(int)>;
    //构造函数
    Thread(ThreadFunc func);
    //析构函数
    ~Thread() = default;
    //启动线程
    void start();
    //获取线程id
    int getId()const;
private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_;  //保存线程id
};

int Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func): func_(func), threadId_(generateId_) {
    generateId_++;
}

//启动线程
void Thread::start() {
    //创建一个线程来执行线程函数
    std::thread t(func_, threadId_);
    t.detach();
} 

int Thread::getId()const {
    return threadId_;
}

//---------------------------线程池类型---------------------------
class ThreadPool{
public:
    //线程池构造
    ThreadPool();

    //线程池析构
    ~ThreadPool();

    //设置线程池的工作模式
    void setMode(PoolMode mode);

    //设置task任务队列上限阈值
    void setTaskQueMaxThreshold(int threshold);

    //设置线程池cached模式下线程数上限阈值
    void setThreadSizeThreshold(int threshold);

    //给线程池提交任务
    //使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
    //pool.submitTask(sum1, 10, 20);  右值引用 + 引用折叠的原理
    //返回值future<>
    //Result submitTask(std::shared_ptr<Task> sp);
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&...args) -> std::future<decltype(func(args...))>;

    //开启线程池 默认值是当前系统CPU的核心数量
    void start(int initThreadSize = std::thread::hardware_concurrency());

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator= (const ThreadPool&) = delete;

private:
    //线程函数
    void threadFunc(int threadId);

    //检查pool的运行状态
    bool checkRunningState() const;

private:
    //std::vector<std::unique_ptr<Thread>> threads_;  //线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;
    int initThreadSize_;                            //初始的线程数量 
    std::atomic_int curThreadSize_;                 //当前线程池线程的总数 cached
    std::atomic_int idleThreadSize_;                //记录空闲线程的数量
    int threadSizeThreshold_;                       //线程数量上限阈值

    //Task任务 =》 函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQue_;  //任务队列
    std::atomic_int curTaskSize_;                //当前任务的数量
    int taskQueMaxThreshold_;                    //任务队列数量上限阈值

    std::mutex taskQueMtx_;             //保证任务队列的线程安全
    std::condition_variable notFull_;   //表示任务队列不满
    std::condition_variable notEmpty_;  //表示任务队列不空
    std::condition_variable exitCond_;  //等待线程资源全部回收

    PoolMode poolMode_;                 //当前线程池的工作模式
    std::atomic_bool isPoolRunning_;    //表示当前线程池的启动状态
};

ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , curThreadSize_(0)
    , curTaskSize_(0)
    , idleThreadSize_(0)
    , taskQueMaxThreshold_(TASK_MAX_THRESHHOLD)
    , threadSizeThreshold_(THREAD_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
{}

ThreadPool::~ThreadPool() {
    isPoolRunning_ = false;

    //等待线程池里面所有的线程返回 有两种状态：阻塞或正在执行任务
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]() { return threads_.size() == 0; });
}

//设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode) {
    if (checkRunningState()) return;//避免在启动线程池后，再对线程池进行设置
    poolMode_ = mode;
}

//设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshold(int threshold) {
    if (checkRunningState()) return;
    taskQueMaxThreshold_ = threshold;
}

//设置线程池cached模式下线程数上限阈值
void ThreadPool::setThreadSizeThreshold(int threshold) {
    if (checkRunningState()) return;
    if (poolMode_ == PoolMode::MODE_CACHED) {
        threadSizeThreshold_ = threshold;
    }
}

//给线程池提交任务  ********用户调用该接口，传入任务对象，生产任务*********
template<typename Func, typename... Args>
auto ThreadPool::submitTask(Func&& func, Args&&...args) -> std::future<decltype(func(args...))> {
    //打包任务，放入任务队列里面
    using RType = decltype(func(args...));
    auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
    std::future<RType> result = task->get_future();

    //获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    //等待任务队列有空余notFull  wait、wait_for、wait_until wait_for有返回值
    //用户提交任务，最长不能阻塞超过 1s，否则判断提交任务失败，返回
    //因为我们不能一直把用户阻塞住
    bool ret = notFull_.wait_for(lock, std::chrono::seconds(1), [&](){ return taskQue_.size() < (size_t)taskQueMaxThreshold_; }); 
    if (!ret) {
        //表示notFull_等待 1s,条件依然没有满足
        std::cerr << "task queue is full, submit task fail." << std::endl;
        auto task = std::make_shared<std::packaged_task<RType()>>(
            []() ->RType { return RType(); });
        (*task)();  //这里必须要执行一下，不然future那边会一直再get处阻塞
        return task->get_future();
    }
    //如果有空余，把任务放入任务队列
    //这层封装很有意思了，因为我们的任务队列中任务是std::function<void()>的
    //所以我们可以再封装一次 
    taskQue_.emplace([task]() { (*task)(); });
    curTaskSize_++;

    //因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知
    notEmpty_.notify_all();

    //cached模式 需要根据当前任务数量和空闲的线程的数量，判断是否需要创建新的线程出来
    if (poolMode_ == PoolMode::MODE_CACHED && curTaskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshold_) {
        std::cout << "create new thread...." << std::endl;
        //创建新线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        //启动新线程
        threads_[threadId]->start();
        //修改线程个数相关的变量
        curThreadSize_++;
        idleThreadSize_++;
    }

    //返回任务的Result对象
    return result;
}

//开启线程池
void ThreadPool::start(int initThreadSize) {
    //设置线程池的运行状态
    isPoolRunning_ = true;

    //记录初始线程个数
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    //创建线程对象
    for (int i = 0; i < initThreadSize_; i++) {
        //创建Thread对象的时候，把线程函数给到Thread对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
    }

    //启动所有线程
    for (int i = 0; i < initThreadSize_; i++) {
        threads_[i]->start();   //需要去执行一个线程函数
        idleThreadSize_++;      //记录初始空闲线程的数量
    }
}

//线程函数  *******线程池的所有线程从任务队列里面消费任务********
void ThreadPool::threadFunc(int threadId) {
    auto lastTime = std::chrono::high_resolution_clock().now();
    while (isPoolRunning_) {
        Task task;
        {
            //先获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

            //cached模式下，有可能已经创建了很多线程，但是空闲时间超过60s，应该把多余的线程结束回收掉（超过initThreadSize数量的线程要进行回收）
            //当前时间 - 上一次线程池执行的时间 > 60s
            //锁 + 双重判断
            while (isPoolRunning_ && taskQue_.size() == 0) {
                //每秒返回一次  怎么区分：超时返回？还是有任务待执行返回
                if (poolMode_ == PoolMode::MODE_CACHED) {
                    //条件变量，超时返回
                    if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
                        auto curTime = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(curTime - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_) {//不能一直回收
                            //开始回收当前线程
                            //把线程对象从线程列表容器中删除
                            //threadid => thread对象 => 删除
                            threads_.erase(threadId);

                            //记录线程数量的相关变量的值修改
                            curThreadSize_--;
                            idleThreadSize_--;

                            std::cout << "threadid: " << std::this_thread::get_id() << " exit" << std::endl;
                            return;
                        }
                    }
                }
                else {//fixed模式
                    //等待notEmpty条件
                    notEmpty_.wait(lock);
                }

                //线程池要结束，回收线程资源，回收阻塞状态的线程
                if (!isPoolRunning_) {
                    threads_.erase(threadId);
                    std::cout << "threadid: " << std::this_thread::get_id() << " exit" << std::endl;
                    exitCond_.notify_all();
                    return;
                }
            }
            if (!isPoolRunning_) {
                break;
            }
            
            idleThreadSize_--;

            std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功..." << std::endl;

            //如果任务队列不为空，从任务队列取出一个任务
            task = taskQue_.front();
            taskQue_.pop();
            curTaskSize_--;

            //如果依然有剩余任务，继续通知其他线程执行任务
            if (taskQue_.size() > 0) {
                notEmpty_.notify_all();
            }

            //因为取出了任务，任务队列肯定不满了，在notFull_上进行通知
            notFull_.notify_all();
        }
        //当前线程负责执行这个任务
        if (task != nullptr) {
            task(); //执行function<void()>
        }
        idleThreadSize_++;//任务处理完了，再++一下
        lastTime = std::chrono::high_resolution_clock().now();//更新时间
    }
    //因为有些线程是正常工作完跳出while循环的，所以这里要回收工作状态的线程
    //std::unique_lock<std::mutex> lock(taskQueMtx_);
    threads_.erase(threadId);
    std::cout << "threadid: " << std::this_thread::get_id() << " exit" << std::endl;
    exitCond_.notify_all();
}

bool ThreadPool::checkRunningState() const{
    return isPoolRunning_;
}

#endif
