#include"threadpool.h"
#include<functional>
#include<thread>
#include<iostream>
#include<memory>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;   //线程数量最好是和核数是一样的
const int THREAD_MAX_IDLE_TIME = 60;    //单位：秒

//----------------Result方法实现----------------
Result::Result(std::shared_ptr<Task> task, bool isValid) 
    : task_(task)
    , isValid_(isValid)
{
    task_->setResult(this);
}

void Result::setVal(Any any) {//线程函数中调用
    //存储task的返回值；
    any_ = std::move(any);
    sem_.post();//已经获取任务的返回值，增加信号量资源
}

Any Result::get() {//用户调用的
    if (!isValid_) {
        return "";
    }
    sem_.wait();    //task任务如果没有执行完，这里会阻塞用户的线程
    return std::move(any_);
}

//----------------Task方法实现----------------
Task::Task() : result_(nullptr) {}

void Task::exec() {
    if (result_ != nullptr) {
        result_->setVal(run());//这里发生多态调用
    }
}

void Task::setResult(Result* res) {
    result_ = res;
}

//----------------Thread方法实现----------------
int Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func): func_(func), threadId_(generateId_) {
    generateId_++;
}

Thread::~Thread() {}

//启动线程
void Thread::start() {
    //创建一个线程来执行线程函数
    std::thread t(func_, threadId_);
    t.detach();
} 

int Thread::getId()const {
    return threadId_;
}

//----------------------ThreadPool方法实现----------------------
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
Result ThreadPool::submitTask(std::shared_ptr<Task> task) {
    //获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    //等待任务队列有空余notFull  wait、wait_for、wait_until wait_for有返回值
    //用户提交任务，最长不能阻塞超过 1s，否则判断提交任务失败，返回
    //因为我们不能一直把用户阻塞住
    bool ret = notFull_.wait_for(lock, std::chrono::seconds(1), [&](){ return taskQue_.size() < (size_t)taskQueMaxThreshold_; }); 
    if (!ret) {
        //表示notFull_等待 1s,条件依然没有满足
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return Result(task, false);
    }
    /*while (taskQue_.size() == taskQueMaxThreshold_) {
        notFull_.wait(lock);
    }*/ // 一个意思，不同写法
    
    //如果有空余，把任务放入任务队列
    taskQue_.emplace(task);
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
    return Result(task);
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
        std::shared_ptr<Task> task;
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
            //task->run(); //执行任务；把任务的返回值通过setVal方法给到result
            task->exec();
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