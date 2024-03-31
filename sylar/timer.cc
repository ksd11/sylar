#include "timer.h"
#include "util.h"

namespace sylar {

// return lhs < rhs
bool Timer::Comparator::operator()(const Timer::ptr& lhs
                        ,const Timer::ptr& rhs) const {
    if(!lhs && !rhs) {
        return false;
    }
    if(!lhs) {
        return true;
    }
    if(!rhs) {
        return false;
    }
    if(lhs->m_next < rhs->m_next) {
        return true;
    }
    if(rhs->m_next < lhs->m_next) {
        return false;
    }
    return lhs.get() < rhs.get();
}


Timer::Timer(uint64_t ms, std::function<void()> cb,
             bool recurring, TimerManager* manager)
    :m_recurring(recurring)
    ,m_ms(ms)
    ,m_cb(cb)
    ,m_manager(manager) {
    // 执行时间为当前执行时间 + 周期
    m_next = sylar::GetCurrentMS() + m_ms;
}

Timer::Timer(uint64_t next)
    :m_next(next) {
}

// 取消定时器
bool Timer::cancel() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(m_cb) {
        m_cb = nullptr;
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

// 刷新定时器
bool Timer::refresh() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    m_next = sylar::GetCurrentMS() + m_ms;
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

// 重置定时器，from_now:是否从当前时间开始计算
bool Timer::reset(uint64_t ms, bool from_now) {
    // 若周期和当前周期相同，且不是从当前时间开始计算
    if(ms == m_ms && !from_now) {
        return true;
    }
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    uint64_t start = 0;
    if(from_now) {
        start = sylar::GetCurrentMS();
    } else {
        start = m_next - m_ms; // 反推开始时间
    }
    m_ms = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;

}

TimerManager::TimerManager() {
    m_previouseTime = sylar::GetCurrentMS();
}

TimerManager::~TimerManager() {
}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb
                                  ,bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutexType::WriteLock lock(m_mutex);
    addTimer(timer, lock);
    return timer;
}

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    // 使用weak_cond的lock函数获取一个shared_ptr指针tmp
    std::shared_ptr<void> tmp = weak_cond.lock();
    // 若tmp不为空，则调用回调函数cb
    if(tmp) {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb
                                    ,std::weak_ptr<void> weak_cond
                                    ,bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

/*
return
    有过期的，返回0； 
    没有定时器，返回无穷大
    有定时器，返回最近过期需要的时间
*/ 
uint64_t TimerManager::getNextTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    m_tickled = false;
    if(m_timers.empty()) {
        return ~0ull; // 无穷大
    }

    const Timer::ptr& next = *m_timers.begin();
    uint64_t now_ms = sylar::GetCurrentMS();
    if(now_ms >= next->m_next) {
        return 0;
    } else {
        return next->m_next - now_ms;
    }
}

// 获取所有过期的定时器，并将他们的回调函数填入cbs
void TimerManager::listExpiredCb(std::vector<std::function<void()> >& cbs) {
    uint64_t now_ms = sylar::GetCurrentMS();
    std::vector<Timer::ptr> expired;
    {
        RWMutexType::ReadLock lock(m_mutex);
        if(m_timers.empty()) {
            return;
        }
    }
    RWMutexType::WriteLock lock(m_mutex);
    if(m_timers.empty()) {
        return;
    }
    bool rollover = detectClockRollover(now_ms);
    // 没有时间倒流，并且最早的定时器还没有到期
    if(!rollover && ((*m_timers.begin())->m_next > now_ms)) {
        return;
    }

    // 找到所有过期的定时器
    Timer::ptr now_timer(new Timer(now_ms));
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
    while(it != m_timers.end() && (*it)->m_next == now_ms) {
        ++it;
    }
    expired.insert(expired.begin(), m_timers.begin(), it);
    m_timers.erase(m_timers.begin(), it);
    cbs.reserve(expired.size());

    for(auto& timer : expired) {
        cbs.push_back(timer->m_cb);
        if(timer->m_recurring) { // 如果循环，则重新添加
            timer->m_next = now_ms + timer->m_ms;
            m_timers.insert(timer);
        } else {
            timer->m_cb = nullptr;
        }
    }
}

// 添加定时器
void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock& lock) {
    auto it = m_timers.insert(val).first;

    // 是最早截止的定时器，并且没有触发onTimerInsertedAtFront(？)
    bool at_front = (it == m_timers.begin()) && !m_tickled;
    if(at_front) {
        m_tickled = true;
    }
    lock.unlock();

    if(at_front) {
        onTimerInsertedAtFront(); // 当有定时器插入到首部时回调
    }
}

bool TimerManager::detectClockRollover(uint64_t now_ms) {
    bool rollover = false;
    // 如果当前时间比上次执行时间还小 并且 小于一个小时的时间，相当于时间倒流了
    if(now_ms < m_previouseTime &&
            now_ms < (m_previouseTime - 60 * 60 * 1000)) {
        rollover = true;
    }
    m_previouseTime = now_ms; // 重新调整时间
    return rollover;
}

bool TimerManager::hasTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    return !m_timers.empty();
}

}
