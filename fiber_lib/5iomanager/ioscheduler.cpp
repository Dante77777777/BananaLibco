#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = true;

namespace banana
{

IOManager* IOManager::GetThis()
{
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}    


IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) 
{
    assert(event == READ || event == WRITE);
    switch(event)
    {
        case READ : return read;
        case WRITE : return write;
    }
    throw std::invalid_argument("Unsupported event type");
}


void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}


void IOManager::FdContext::triggerEvent(IOManager::Event event)
{
    assert(event & events);
    events = (Event)(events & ~event);
    EventContext& ctx = getEventContext(event);
    if(ctx.cb)
    {
        ctx.scheduler->scheduleLock(&ctx.cb);
    }else if(ctx.fiber)
    {
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }
    resetEventContext(ctx);
    return ;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name): 
Scheduler(threads, use_caller, name), TimerManager()
{
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    int rt = pipe(m_tickleFds);
    assert(!rt);

    epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];

    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    contextResize(32);
    start();
}


IOManager::~IOManager()
{
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); i++)
    {
        if(m_fdContexts[i])
        {
            delete m_fdContexts[i];
        }
    }
    
}


void IOManager::contextResize(size_t size)
{
    m_fdContexts.resize(size);
    for (size_t i = 0; i < m_fdContexts.size(); i++)
    {
        if(m_fdContexts[i] == nullptr)
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i;
        }
    }
    
}


int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    FdContext* fd_ctx = nullptr;
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }else
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    if(fd_ctx->events & event)
    {
        return -1;
    }
    int op = fd_ctx->events  ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | event | fd_ctx->events;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd,op,fd,&epevent);
    if (rt) 
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }
    ++m_pendingEventCount;
    fd_ctx->events = (Event)(fd_ctx->events | event);

    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) 
    {
        event_ctx.cb.swap(cb);
    } 
    else 
    {
        event_ctx.fiber = Fiber::GetThis();
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0;
}


bool IOManager::delEvent(int fd, Event event)
{
    FdContext* fd_ctx = nullptr;
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }else
    {
        read_lock.unlock();
        return false;
    }
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    if(!(fd_ctx->events & event)){
        return false;
    }
    Event newEvents = (Event)(fd_ctx->events & ~event);
    int op  = newEvents ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | newEvents;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    --m_pendingEventCount;
    fd_ctx->events = newEvents;
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}


bool IOManager::cancelEvent(int fd, Event event)
{
    FdContext* fd_ctx = nullptr;
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }else{
        read_lock.unlock();
        return false;
    }
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    if(!(fd_ctx->events & event))
    {
        return false;
    }
    Event newEvents = (Event)(fd_ctx->events & ~event);
    int op = newEvents ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | newEvents;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd,op,fd,&epevent);
    if (rt) 
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }
    --m_pendingEventCount;
    fd_ctx->triggerEvent(event);
    return true;
}


bool IOManager::cancelAll(int fd)
{
    FdContext* fd_ctx = nullptr;
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd)
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }else{
        read_lock.unlock();
        return false;
    }
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    if(!fd_ctx->events)
    {
        return false;
    }

    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd,op,fd,&epevent);
    if (rt) 
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    if(fd_ctx->events & READ)
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if(fd_ctx->events & WRITE)
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }
    assert(fd_ctx->events == 0);
    return true;
}


void IOManager::tickle() 
{
    if(!hasIdleThreads()){
        return ;
    }
    int rt = write(m_tickleFds[1],"T",1);
    assert(rt == 1);
}


bool IOManager::stopping()
{
    uint64_t timeout = getNextTimer();
    return timeout = ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}


void IOManager::idle()
{
    static const uint64_t MAX_EVENTS = 256;
    std::shared_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);
    while (true)
    {
        if(debug) std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl; 

        if(stopping()) 
        {
            if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            break;
        }

        int rt = 0;
        while(true)
        {
            static const uint64_t MAX_TIMEOUT = 5000;
            uint64_t nextTimeout = getNextTimer();
            nextTimeout = std::min(nextTimeout,MAX_TIMEOUT);

            rt = epoll_wait(m_epfd,events.get(),MAX_EVENTS,nextTimeout);
            if(rt < 0 && errno == EINTR) 
            {
                continue;
            } 
            else 
            {
                break;
            }
        }
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if(!cbs.empty())
        {
            for(auto& each : cbs)
            {
                scheduleLock(each);
            }
            cbs.clear();
        }

        for(int i = 0; i < rt; i++)
        {
            epoll_event& event = events[i];
            if(event.data.fd == m_tickleFds[0])
            {
                uint8_t dummy[256];
                while(read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }
            FdContext* fd_ctx = (FdContext*)event.data.ptr;
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            if(event.events & (EPOLLHUP | EPOLLERR))
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            int real_event = NONE;
            if(event.events & EPOLLIN){
                real_event |= READ;
            }

            if(event.events & EPOLLOUT)
            {
                real_event |= WRITE;
            }

            if((fd_ctx->events & real_event) == NONE)
            {
                continue;
            }

            int leftEvent = (fd_ctx->events & ~real_event);
            int op = leftEvent ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | leftEvent;

            int rt2 = epoll_ctl(m_epfd,op,fd_ctx->fd,&event);
            if (rt2) 
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            if(real_event & READ)
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }

            if(real_event & WRITE)
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        }
        Fiber::GetThis()->yield();
    }
    
}


void IOManager::onTimerInsertedAtFront() 
{
    tickle();
}






}