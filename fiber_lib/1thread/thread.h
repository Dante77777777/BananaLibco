#ifndef _THREAD_H_
#define _THREAD_H_

#include <string>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace banana
{
class Semaphore
{
private:
    std::condition_variable cv;
    std::mutex mutex_;
    int count;
public:
    explicit Semaphore(int count_ = 0) : count(count_){}
    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while(count == 0){
            cv.wait(lock);
        }
        count--;
    }
    void signal(){
        std::unique_lock<std::mutex> lock(mutex_);
        count++;
        cv.notify_one();
    }
};    

class Thread
{
private:
    pid_t m_id = -1;
    pthread_t m_thread = 0;
    std::function<void()> m_cb;
    std::string m_name;
    Semaphore m_semaphore;
public:
    Thread(std::function<void()> cb, const std::string& name);
    ~Thread();
    pid_t getId() const { return m_id; }
    const std::string& getName() const { return m_name; }

    void join();
public:
    // 获取系统分配的线程id
	static pid_t GetThreadId();
    // 获取当前所在线程
    static Thread* GetThis();

    // 获取当前线程的名字
    static const std::string& GetName();
    // 设置当前线程的名字
    static void SetName(const std::string& name);

private:
	// 线程函数
    static void* run(void* arg);
};









} // namespace banana




#endif