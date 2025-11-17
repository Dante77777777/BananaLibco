#ifndef __BANANA_TIMER_H__
#define __BANANA_TIMER_H__

#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <assert.h>
#include <functional>
#include <mutex>

namespace banana
{

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer>
{
    friend class TimerManager;

public:
    bool cancel();
    bool refresh();
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);

private:
    bool m_recurring = false;
    uint64_t m_ms = 0;
    std::chrono::time_point<std::chrono::system_clock> m_next;
    std::function<void()> m_cb;
    TimerManager* m_manager = nullptr;

private:
    struct Comparator
    {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };
};

class TimerManager
{
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,bool recurring = false);
    uint64_t getNextTimer();
    void listExpiredCb(std::vector<std::function<void()>>& cbs);
    bool hasTimer();

protected:
    virtual void onTimerInsertedAtFront() {};
    void addTimer(std::shared_ptr<Timer> timer);

private:
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex;
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;
    bool m_tickled = false;
    std::chrono::time_point<std::chrono::system_clock> m_previousTime;

};












}











#endif