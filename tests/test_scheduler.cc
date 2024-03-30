#include "sylar/sylar.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();

void test_fiber() {
    static int s_count = 5;
    SYLAR_LOG_INFO(g_logger) << "test in fiber s_count=" << s_count;

    sleep(1);
    if(--s_count >= 0) {
        // 固定只能当前线程去执行该协程任务
        sylar::Scheduler::GetThis()->schedule(&test_fiber, sylar::GetThreadId());
    }
}

int main(int argc, char** argv) {
    SYLAR_LOG_INFO(g_logger) << "main";
    sylar::Scheduler sc(1, false, "worker");
    sc.start();
    sleep(2);
    SYLAR_LOG_INFO(g_logger) << "schedule";
    sc.schedule(&test_fiber); // 将任务加入到任务队列
    sc.stop();
    SYLAR_LOG_INFO(g_logger) << "over";
    return 0;
}
