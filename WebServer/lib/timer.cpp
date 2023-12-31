#include "timer.h"
#include "epoll.h"
#include <string>
#include <sys/time.h>

// 为每一个连接添加一个过期时间
TimerNode::TimerNode(SP_ReqData _request_data, int timeout) : deleted(false),
                                                              request_data(_request_data)
{
    // cout << "TimerNode()" << endl;
    struct timeval now;
    gettimeofday(&now, NULL);
    // 以毫秒计
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

TimerNode::~TimerNode()
{
    // cout << "~TimerNode()" << endl;
    if (request_data != NULL)
    {
        Epoll::epoll_del(request_data->getFd());
    }
}

void TimerNode::update(int timeout)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

// 是否有效(没有超时)
bool TimerNode::isvalid()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    size_t temp = ((now.tv_sec * 1000) + (now.tv_usec / 1000));
    if (temp < expired_time)
    {
        return true;
    }
    else
    {
        this->setDeleted();
        return false;
    }
}

void TimerNode::clearReq()
{
    // 给智能指针制空值
    request_data.reset();
    this->setDeleted();
}

void TimerNode::setDeleted()
{
    deleted = true;
}

// 是否设置为删除
bool TimerNode::isDeleted() const
{
    return deleted;
}

size_t TimerNode::getExpTime() const
{
    return expired_time;
}

TimerManager::TimerManager()
{
}

TimerManager::~TimerManager()
{
}

void TimerManager::addTimer(SP_ReqData request_data, int timeout)
{
    SP_TimerNode new_node(new TimerNode(request_data, timeout));
    {
        MutexLockGuard locker(lock);
        TimerNodeQueue.push(new_node);
    }
    request_data->linkTimer(new_node);
}

void TimerManager::addTimer(SP_TimerNode timer_node){}

/* 处理逻辑是这样的~
因为(1) 优先队列不支持随机访问
(2) 即使支持，随机删除某节点后破坏了堆的结构，需要重新更新堆结构。
所以对于被置为deleted的时间节点，会延迟到它(1) 超时 或(2) 它前面的节点都被删除时，它才会被删除。
一个点被置为deleted,它最迟会在TIMER_TIME_OUT时间后被删除。
这样做有两个好处：
(1) 第一个好处是不需要遍历优先队列，省时。
(2) 第二个好处是给超时时间一个容忍的时间，就是设定的超时时间是删除的下限(并不是一到超时时间就立即删除)，如果监听的请求在超时后的下一次请求中又一次出现了，
就不用再重新申请requestData节点了，这样可以继续重复利用前面的requestData，减少了一次delete和一次new的时间。
*/

/*
此函数用来计时，当加入epoll红黑树上边的结点超时时候会进行剔除，每次有事件发生时就要进行分离，
处理完事件后重新进行计时；
所以计时器计时的不是事件处理 超时，而是加到epoll里长时间滞留，没有事件发生
*/
void TimerManager::handle_expired_event()
{
    MutexLockGuard locker(lock);
    while (!TimerNodeQueue.empty())
    {
        SP_TimerNode ptimer_now = TimerNodeQueue.top();
        if (ptimer_now->isDeleted())
        {
            TimerNodeQueue.pop();
            // delete ptimer_now;
        }
        else if (ptimer_now->isvalid() == false)
        {
            TimerNodeQueue.pop();
            // delete ptimer_now;
        }
        else
        {
            break;
        }
    }
}