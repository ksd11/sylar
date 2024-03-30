#include "scheduler.h"
#include "log.h"
#include "macro.h"
#include "hook.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

static thread_local Scheduler* t_scheduler = nullptr; // 当前协程调度器
static thread_local Fiber* t_scheduler_fiber = nullptr; // 每个线程的主协程

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name)
    :m_name(name) {
    SYLAR_ASSERT(threads > 0);
    
    // 是否将调用者线程作为一个调度线程
    if(use_caller) {
        sylar::Fiber::GetThis(); // 获得主协程
        --threads; // 需要的线程减少一个

        SYLAR_ASSERT(GetThis() == nullptr);
        t_scheduler = this;

        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        sylar::Thread::SetName(m_name);

        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread = sylar::GetThreadId();
        m_threadIds.push_back(m_rootThread);
    } else {
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler::~Scheduler() {
    SYLAR_ASSERT(m_stopping);
    if(GetThis() == this) {
        t_scheduler = nullptr;
    }
}

Scheduler* Scheduler::GetThis() {
    return t_scheduler;
}

Fiber* Scheduler::GetMainFiber() {
    return t_scheduler_fiber;
}

// 开启调度器，运行m_threadCount个线程，若有use_caller，主协程也要开启？
void Scheduler::start() {
    MutexType::Lock lock(m_mutex);
    if(!m_stopping) {
        return;
    }
    m_stopping = false;
    SYLAR_ASSERT(m_threads.empty());

    // 创建m_threadCount个线程，并加入线程池
    m_threads.resize(m_threadCount);
    for(size_t i = 0; i < m_threadCount; ++i) {
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this)
                            , m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
    lock.unlock();

    // if(m_rootFiber) {
    //    //m_rootFiber->swapIn();
    //    m_rootFiber->call();
    //    SYLAR_LOG_INFO(g_logger) << "call out " << m_rootFiber->getState();
    // }
}

void Scheduler::stop() {
    m_autoStop = true;
    if(m_rootFiber
            && m_threadCount == 0
            && (m_rootFiber->getState() == Fiber::TERM
                || m_rootFiber->getState() == Fiber::INIT)) {
        SYLAR_LOG_INFO(g_logger) << this << " stopped";
        m_stopping = true;

        if(stopping()) {
            return;
        }
    }

    //bool exit_on_this_fiber = false;
    if(m_rootThread != -1) {
        SYLAR_ASSERT(GetThis() == this);
    } else {
        SYLAR_ASSERT(GetThis() != this);
    }

    m_stopping = true;
    for(size_t i = 0; i < m_threadCount; ++i) {
        tickle();
    }

    if(m_rootFiber) {
        tickle();
    }

    if(m_rootFiber) {
        //while(!stopping()) {
        //    if(m_rootFiber->getState() == Fiber::TERM
        //            || m_rootFiber->getState() == Fiber::EXCEPT) {
        //        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, true));
        //        SYLAR_LOG_INFO(g_logger) << " root fiber is term, reset";
        //        t_fiber = m_rootFiber.get();
        //    }
        //    m_rootFiber->call();
        //}
        if(!stopping()) {
            m_rootFiber->call(); // 执行未完成任务
        }
    }
    SYLAR_LOG_DEBUG(g_logger) << "main return";

    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }

    for(auto& i : thrs) {
        i->join();
    }
    //if(exit_on_this_fiber) {
    //}
}

// 每个线程设置调度器线程
void Scheduler::setThis() {
    t_scheduler = this;
}

// 每个线程执行的函数
void Scheduler::run() {
    SYLAR_LOG_DEBUG(g_logger) << m_name << " run";
    // set_hook_enable(true);
    setThis(); // 设置调度器线程，每个线程的调度器线程都是一样的

    // 非user_caller线程，每个线程设置自己的主协程
    if(sylar::GetThreadId() != m_rootThread) {
        t_scheduler_fiber = Fiber::GetThis().get();
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));// 创建一个idle协程
    Fiber::ptr cb_fiber;

    FiberAndThread ft;
    while(true) {
        ft.reset();
        bool tickle_me = false;
        bool is_active = false;
        {
            MutexType::Lock lock(m_mutex);

            // 遍历协程队列
            auto it = m_fibers.begin();
            while(it != m_fibers.end()) {
                // 该任务指定了调度线程，但不是当前线程
                if(it->thread != -1 && it->thread != sylar::GetThreadId()) {
                    ++it;
                    tickle_me = true;
                    continue;
                }

                SYLAR_ASSERT(it->fiber || it->cb);

                // 如果该fiber正在执行则跳过
                if(it->fiber && it->fiber->getState() == Fiber::EXEC) {
                    ++it;
                    continue;
                }

                ft = *it;
                m_fibers.erase(it++);
                ++m_activeThreadCount;
                is_active = true; // 标记找到
                break;
            }
            tickle_me |= it != m_fibers.end();
        }

        if(tickle_me) {
            tickle();
        }
        // 如果是fiber，并且处于可执行状态
        if(ft.fiber && (ft.fiber->getState() != Fiber::TERM
                        && ft.fiber->getState() != Fiber::EXCEPT)) {
            ft.fiber->swapIn(); // 切换协程
            --m_activeThreadCount; // 执行完成.

            if(ft.fiber->getState() == Fiber::READY || ft.fiber->getState() == Fiber::HOLD) {
                // 将协程重新加入到队列
                schedule(ft.fiber);
            } else if(ft.fiber->getState() != Fiber::TERM
                    && ft.fiber->getState() != Fiber::EXCEPT) {
                ft.fiber->m_state = Fiber::HOLD; // TODO. bug? 设置hold后面又释放了
            }
            ft.reset();
        } else if(ft.cb) { // 如果是函数
            if(cb_fiber) {
                cb_fiber->reset(ft.cb); // 设置cb_fiber
            } else {
                cb_fiber.reset(new Fiber(ft.cb)); // 将callback包装成一个协程
            }
            ft.reset();
            cb_fiber->swapIn(); // 切换协程
            --m_activeThreadCount;
            if(cb_fiber->getState() == Fiber::READY || cb_fiber->getState() == Fiber::HOLD){
                schedule(cb_fiber);
                cb_fiber.reset();
            } else if(cb_fiber->getState() == Fiber::EXCEPT
                    || cb_fiber->getState() == Fiber::TERM) {
                cb_fiber->reset(nullptr);
            } else {//if(cb_fiber->getState() != Fiber::TERM) {
                cb_fiber->m_state = Fiber::HOLD; // TODO. bug? 设置hold后面又释放了
                cb_fiber.reset();
            }
        } else { // 如果没有任务，切换到Idle协程
            if(is_active) {
                --m_activeThreadCount;
                continue;
            }
            if(idle_fiber->getState() == Fiber::TERM) {
                SYLAR_LOG_INFO(g_logger) << "idle fiber term";
                break;
            }

            ++m_idleThreadCount;
            idle_fiber->swapIn(); // 切换协程
            --m_idleThreadCount;
            if(idle_fiber->getState() != Fiber::TERM
                    && idle_fiber->getState() != Fiber::EXCEPT) { // 当stopping()为true时，idle得状态变为term
                idle_fiber->m_state = Fiber::HOLD;
            }
        }
    }
}

void Scheduler::tickle() {
    SYLAR_LOG_INFO(g_logger) << "tickle";
}

// 判断停止条件
bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    return m_autoStop && m_stopping
        && m_fibers.empty() && m_activeThreadCount == 0;
}

// 只要线程没有任务，就执行idle协程
void Scheduler::idle() {
    SYLAR_LOG_INFO(g_logger) << "idle";
    while(!stopping()) {
        sylar::Fiber::YieldToHold();
    }
}

void Scheduler::switchTo(int thread) {
    SYLAR_ASSERT(Scheduler::GetThis() != nullptr);
    if(Scheduler::GetThis() == this) {
        if(thread == -1 || thread == sylar::GetThreadId()) {
            return;
        }
    }
    schedule(Fiber::GetThis(), thread);
    Fiber::YieldToHold();
}

std::ostream& Scheduler::dump(std::ostream& os) {
    os << "[Scheduler name=" << m_name
       << " size=" << m_threadCount
       << " active_count=" << m_activeThreadCount
       << " idle_count=" << m_idleThreadCount
       << " stopping=" << m_stopping
       << " ]" << std::endl << "    ";
    for(size_t i = 0; i < m_threadIds.size(); ++i) {
        if(i) {
            os << ", ";
        }
        os << m_threadIds[i];
    }
    return os;
}

SchedulerSwitcher::SchedulerSwitcher(Scheduler* target) {
    m_caller = Scheduler::GetThis();
    if(target) {
        target->switchTo();
    }
}

SchedulerSwitcher::~SchedulerSwitcher() {
    if(m_caller) {
        m_caller->switchTo();
    }
}

}
