#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<queue>
#include<memory>
#include<atomic>
#include<vector>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unordered_map>
#include<thread>

//---------------------------Any类型---------------------------
//可以接收任意数据的类型
class Any{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any&) = delete;
    Any& operator= (const Any&) = delete;
    Any(Any&&) = default;
	Any& operator=(Any&&) = default;

    //这个构造函数可以让Any接收任意其他的数据
    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data)) {}

    //这个方法能够把Any对象里面存储的data数据提取出来
    template<typename T>
    T cast() {
        //我们怎么从base_找到它所指向的Derive对象，从它里面取出data成员变量
        //基类指针转成派生类指针 RTTI
        Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
        if(pd == nullptr) {
            throw "type is unmatch!";
        }
        return pd->data_;
    }
private:
    //基类类型
    class Base{
    public:
        virtual ~Base() = default;
    };
    //派生类类型
    template<typename T>
    class Derive : public Base {
    public:
        Derive(T data) : data_(data) {}
        T data_;
    };
private:
    //定义一个基类的指针
    std::unique_ptr<Base> base_;
};

//---------------------------实现一个信号量类---------------------------
//mutex+condition_variable
class Semaphore {
public:
    Semaphore(int limit = 0) : soureLimit_(limit) {}
    ~Semaphore() = default;

    //获取一个信号量资源
    void wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        //等待信号量有资源，没有资源的话，会阻塞当前线程
        cond_.wait(lock, [&]() { return soureLimit_ > 0; });
        soureLimit_--;
    }

    //增加一个信号量资源
    void post() {
        std::unique_lock<std::mutex> lock(mtx_);
        soureLimit_++;
        cond_.notify_all();
    }

private:
    int soureLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

class Task;

//-----------------------实现submitTask的返回值类型Result----------------------
class Result{ 
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    //问题一：setVal方法，获取任务执行完的返回值（线程函数中调用）
    void setVal(Any any);

    //问题二：get方法，用户调用这个方法获取task的返回值
    //如果任务还没有执行完，阻塞在这里
    Any get();
private:
    Any any_;                   //存储任务的返回值
    Semaphore sem_;
    std::shared_ptr<Task> task_;//指向对应获取返回值的任务对象
    std::atomic_bool isValid_;  //返回值是否有效
};

//---------------------------任务抽象基类---------------------------
class Task{
public:
    Task();
    ~Task() = default;
    //线程函数中调用
    void exec();
    //Result的构造函数中调用
    void setResult(Result* res);
    //用户自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;

private:
    Result* result_;//使用裸指针是为了避免智能指针的循环引用
};

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
    ~Thread();
    //启动线程
    void start();
    //获取线程id
    int getId()const;
private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_;  //保存线程id
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
	public:
		Any run() { // 线程代码... }
};
Result res = pool.submitTask(std::make_shared<MyTask>());
int sum = res.get().cast<int>();
*/

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
    Result submitTask(std::shared_ptr<Task> sp);

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

    std::queue<std::shared_ptr<Task>> taskQue_;  //任务队列
    std::atomic_int curTaskSize_;                //当前任务的数量
    int taskQueMaxThreshold_;                    //任务队列数量上限阈值

    std::mutex taskQueMtx_;             //保证任务队列的线程安全
    std::condition_variable notFull_;   //表示任务队列不满
    std::condition_variable notEmpty_;  //表示任务队列不空
    std::condition_variable exitCond_;  //等待线程资源全部回收

    PoolMode poolMode_;                 //当前线程池的工作模式
    std::atomic_bool isPoolRunning_;    //表示当前线程池的启动状态
};

#endif
