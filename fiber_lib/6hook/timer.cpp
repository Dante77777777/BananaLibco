#include "timer.h"

namespace banana
{
bool Timer::cancel()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);
    if(m_cb == nullptr)
    {
        return false;
    }else
    {
        m_cb = nullptr;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it != m_manager->m_timers.end())
    {
        m_manager->m_timers.erase(it);
    }
    return true;
}


bool Timer::refresh()
{
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);
    if(m_cb == nullptr)
    {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end())
    {
        return false;
    }
    m_manager->m_timers.erase(it);
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
    m_manager->m_timers.insert(shared_from_this());
}

//重新设置定时器的超时时间间隔，并可以选择是从现在开始重新计时还是从原来的起始点开始计时。
bool Timer::reset(uint64_t ms, bool from_now)
{
    if(ms != m_ms && !from_now){
        return true;
    }

    {
        if(!m_cb)
        {
            return false;
        }

        auto it = m_manager->m_timers.find(shared_from_this());
        if(it == m_manager->m_timers.end())
        {
            return false;
        }
        m_manager->m_timers.erase(it);
    }
    auto start = from_now ? std::chrono::system_clock::now() : m_next - std::chrono::milliseconds(m_ms); 
    m_ms = ms;
    m_next = std::chrono::milliseconds(m_ms) + start;
    m_manager->addTimer(shared_from_this());
    return true;
}


Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager):
m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager)
{
    auto now = std::chrono::system_clock::now();
    m_next = now + std::chrono::milliseconds(m_ms);
}


bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const
{
    assert(lhs != nullptr && rhs != nullptr);
    return lhs->m_next < rhs->m_next;
}


TimerManager::TimerManager() 
{
    m_previousTime = std::chrono::system_clock::now();
}


TimerManager::~TimerManager() 
{
}


std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) 
{
    std::shared_ptr<Timer> timer(new Timer(ms,cb,recurring,this));
    addTimer(timer);
    return timer;
}


static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
{
    std::shared_ptr<void> tmp = weak_cond.lock();
    if(tmp){
        cb();
    }
}


std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring) 
{
    return addTimer(ms, std::bind(&OnTimer,weak_cond,cb),recurring);
}


uint64_t TimerManager::getNextTimer()
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    m_tickled = false;

    if(m_timers.empty())
    {
        return ~0ull;
    }

    auto now = std::chrono::system_clock::now();
    auto next = (*m_timers.begin())->m_next;
    if(now >= next){
        return 0;
    }
    else
    {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
        return static_cast<uint64_t>(duration.count());
    }
}


void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs)
{
    auto now = std::chrono::system_clock::now();
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    bool roolover = detectClockRollover();

    while((!m_timers.empty() && roolover) || (!m_timers.empty() && now >= (*m_timers.begin())->m_next ))
    {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());
        cbs.push_back(temp->m_cb);
        if(temp->m_recurring)
        {
            temp->m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        }
        else{
            temp->m_cb = nullptr;
        }
    }
}


bool TimerManager::hasTimer() 
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return !m_timers.empty();
}


void TimerManager::addTimer(std::shared_ptr<Timer> timer)
{
    bool at_front = false;
    {
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        auto it = m_timers.insert(timer).first;
        at_front = (it == m_timers.begin()) && !m_tickled;
        if(at_front){
            m_tickled = true;
        }
    }
    if(at_front)
    {
        // wake up 
        onTimerInsertedAtFront();
    }
}


bool TimerManager::detectClockRollover()
{
    bool rollover = false;
    auto now = std::chrono::system_clock::now();
    if(now < (m_previousTime - std::chrono::milliseconds(60 * 60 * 1000))) 
    {
        rollover = true;
    }
    m_previousTime = now;
    return rollover;
}
}