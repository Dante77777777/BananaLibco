#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// apply XX to all functions
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace banana
{
static thread_local bool t_hook_enable = false;
bool is_hook_enable()
{
    return t_hook_enable;
}

void set_hook_enable(bool flag)
{
    t_hook_enable = flag;
}

void hook_init()
{
	static bool is_inited = false;
	if(is_inited)
	{
		return;
	}

	// test
	is_inited = true;


// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
	HOOK_FUN(XX)
#undef XX
}


struct HookIniter{
    HookIniter(){
        hook_init();
    }
};


static HookIniter s_hook_initer;

} // namespace banana


struct timer_info{
    int cancelled = 0;
};

template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args) 
{
    if(!banana::is_hook_enable){
        return fun(fd, std::forward<Args>(args)...);
    }

    std::shared_ptr<banana::FdCtx> ctx = banana::FdMgr::GetInstance()->get(fd);
    if(!ctx){
        return fun(fd,std::forward<Args>(args)...);
    }

    if(ctx->isClosed()){
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket() || ctx->m_userNonblock()){
        return fun(fd,std::forward<Args>(args)...);
    }

    uint64_t timeout = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    ssize_t n = fun(fd,std::forward<Args>(args)...);
    while(n == 1 && errno == EINTR){
        n = fun(fd,std::forward<Args>(args)...);
    }

    if(n == -1 && errno == EAGAIN)
    {
        banana::IOManager* iom = banana::IOManager::GetThis();

        std::shared_ptr<banana::Timer> timer;
        std::weak_ptr<banna::timer_info> winfo(tinfo);

        if(timeout != (uint64_t)-1)
        {
            timer = iom->addConditionTimer(timeout, [winfo,fd,iom,event] ()
            {
                auto t = winfo.lock();
                if(!t || t->cancelled)
                {
                    return ;
                }
                t->cancelled = ETIMEOUT;
                iom->cancelEvent(fd,(banna::IOManager::event)event);
            }, winfo);
        }

        int rt = iom->addEvent(fd,(banna::IOManager::event)event);
        if(rt)
        {
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";
            if(timer)
            {
                timer->cancel();
            }
            return -1;
        }else{
            banana::Fiber::GetThis()->yield();//协程在这yield之后，当io资源到位会继续向下指向
            if(timer)//所以要取消掉超时定时器。
            {
                timer->cancel();
            }
            if(tinfo->cancelled == ETIMEDOUT)
            {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    return n;
}


extern "C"
{
#define XX(name) name ## _fun name ## _f = nullptr;
	HOOK_FUN(XX)

#undef XX


unsigned int sleep(unsigned int seconds)
{
    if(!banana::t_hook_enable){
        return sleep_f(seconds);
    }
    std::shared_ptr<banana::Fiber> fiber = banana::Fiber::GetThis();
    banana::IOManager* iom = banana::IOManager::GetThis();
    iom->addTimer(seconds*1000,[fiber,iom](){iom->scheduleLock(fiber,-1);});
    fiber->resume();
    return 0;
}


int usleep(useconds_t usec)
{
    if(!banana::t_hook_enable)
    {
        return usleep_f(usec);
    }
    std::shared_ptr<banana::Fiber> fiber = banana::Fiber::GetThis();
    banana::IOManager* iom = banana::IOManager::GetThis();
    iom->addTimer(usec/1000,[fiber,iom](){iom->scheduleLock(fiber,-1);});
    fiber->yield();
    return 0;
}


int nanosleep(const struct timespec* req, struct timespec* rem)
{
    if(!banana::t_hook_enable)
    {
        return nanosleep_f(req,rem);
    }
    int sleepSeconds = req->tv_sec*1000 + rem->tv_nsec/1000/1000;
    std::shared_ptr<banana::Fiber> fiber = banana::Fiber::GetThis();
    banana::IOManager* iom = banana::IOManager::GetThis();
    iom->addTimer(sleepSeconds,[fiber,iom](){iom->scheduleLock(fiber,-1);});
    fiber->yield();
    return 0;
}


int socket(int domain, int type, int protocol)
{
    if(!banana::t_hook_enable)
    {
        return socket_f(domain, type, protocol);
    }
    int fd = socket_f(domain, type, protocol);
    if(fd == -1)
    {
        std::cerr << "socket() failed:" << strerror(errno) << std::endl;
        return fd;
    }
    banana::FdMgr::GetInstance()->get(fd,true);
    return fd;
}


int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) 
{
    if(!banana::t_hook_enable)
    {
        return connect_f(fd,addr,addrlen);
    }
    std::shared_ptr<banana::FdCtx> ctx = banana::FdMgr::GetInstance()->get(fd);
    if(!ctx || ctx->isClosed()){
        errno = EBADF;
        return -1;
    }
    if(!ctx->isSocket())
    {
        return connect_f(fd,addr,addrlen);
    }
    if(ctx->getUserNonblock())
    {
        return connect_f(fd,addr,addrlen);
    }
    int n = connect_f(fd,addr,addrlen);
    if(n == 0)
    {
        return 0;
    }
    if(n != -1 || errno != EINPROGRESS)
    {
        return n;
    }


    banana::IOManager* iom = banana::IOManager::GetThis();
    std::shared_ptr<banana::Timer> timer;
    std::shared_ptr<timer_info> t_info;
    std::weak_ptr<timer_info> winfo(t_info);

    if(timeout_ms != (uint64_t)-1)
    {
        timer = iom->addConditionTimer(timeout_ms,[winfo,fd,iom]{
            auto t = winfo.lock();
            if(!t || t->cancelled)
            {
                return ;
            }
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd,banana::IOManager::WRITE);
        },winfo);
    }
    int rt = iom->addEvent(fd,banana::IOManager::WRITE);
    if(rt == 0)
    {
        banana::Fiber::GetThis()->yield();
        if(timer)
        {
            timer->cancel();
        }
        if(t_info->cancelled)
        {
            errno = t_info->cancelled;
            return -1;
        }
    }
    else{
        if(timer)
        {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }
    // check out if the connection socket established 
    int error = 0;
    socklen_t len = sizeof(int);
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) 
    {
        return -1;
    }
    if(!error) 
    {
        return 0;
    } 
    else 
    {
        errno = error;
        return -1;
    }
}


static uint64_t s_connect_timeout = -1;
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    return connect_with_timeout(sockfd,addr,addrlen,s_connect_timeout);
}


int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    int fd = do_io(sockfd,accept_f,"accept",banana::IOManager::READ,SO_RCVTIMEO,addr,addrlen);
    if(fd >= 0)
    {
        banana::FdMgr::GetInstance()->get(fd,true);
    }   
    return fd;
}


ssize_t read(int fd, void *buf, size_t count)
{
    return do_io(fd,read_f,"read",banana::IOManager::READ,SO_RCVTIMEO,buf,count);
}


ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    return do_io(fd,readv_f,"readv",banana::IOManager::READ,SO_RCVTIMEO,iov,iovcnt);
}


ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    return do_io(sockfd,recv_f,"recv",banana::IOManager::READ,SO_RCVTIMEO,buf,len,flags);
}


ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    return do_io(sockfd,recvfrom_f,"recvfrom",banana::IOManager::READ,SO_RCVTIMEO,buf,len,flags,src_addr,addrlen);
}


ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    return do_io(sockfd,recvmsg_f,"recvmsg",banana::IOManager::READ,SO_RCVTIMEO,msg,flags);
}
}