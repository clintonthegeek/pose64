/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Qt-based replacement for omni_thread (CORBA threading library).
	Provides API-compatible wrappers around QMutex, QWaitCondition,
	and QThread so the ~18 POSE source files that use omni_thread
	primitives continue to work unchanged.
\* ===================================================================== */

#ifndef __omnithread_h_
#define __omnithread_h_

#include <QMutex>
#include <QWaitCondition>
#include <QThread>
#include <time.h>    // clock_gettime for absolute<->relative time conversion

// Forward declarations
class omni_condition;

// ---- Mutex ----
class omni_mutex {
public:
    omni_mutex() {}
    ~omni_mutex() {}

    void lock()    { m.lock(); }
    void unlock()  { m.unlock(); }
    void acquire() { lock(); }
    void release() { unlock(); }

private:
    QMutex m;
    friend class omni_condition;

    // prevent copying
    omni_mutex(const omni_mutex&);
    omni_mutex& operator=(const omni_mutex&);
};

// RAII lock
class omni_mutex_lock {
public:
    omni_mutex_lock(omni_mutex& m) : mutex(m) { mutex.lock(); }
    ~omni_mutex_lock() { mutex.unlock(); }
private:
    omni_mutex& mutex;
    omni_mutex_lock(const omni_mutex_lock&);
    omni_mutex_lock& operator=(const omni_mutex_lock&);
};

// RAII unlock (unlocks in ctor, relocks in dtor)
class omni_mutex_unlock {
public:
    omni_mutex_unlock(omni_mutex& m) : mutex(m) { mutex.unlock(); }
    ~omni_mutex_unlock() { mutex.lock(); }
private:
    omni_mutex& mutex;
    omni_mutex_unlock(const omni_mutex_unlock&);
    omni_mutex_unlock& operator=(const omni_mutex_unlock&);
};

// ---- Condition Variable ----
// NOTE: omni_condition::timedwait takes ABSOLUTE time (secs since epoch).
// QWaitCondition::wait takes RELATIVE time (ms from now). We convert.
class omni_condition {
public:
    omni_condition(omni_mutex* m) : mutex(m) {}
    ~omni_condition() {}

    void wait() { cond.wait(&mutex->m); }

    int timedwait(unsigned long abs_sec, unsigned long abs_nsec = 0) {
        // Convert absolute time to relative milliseconds
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        long long ms = (long long)(abs_sec - now.tv_sec) * 1000
                     + (long long)(abs_nsec - now.tv_nsec) / 1000000;
        if (ms < 0) ms = 0;
        return cond.wait(&mutex->m, (unsigned long)ms) ? 1 : 0;
    }

    void signal()    { cond.wakeOne(); }
    void broadcast() { cond.wakeAll(); }

private:
    omni_mutex* mutex;
    QWaitCondition cond;

    omni_condition(const omni_condition&);
    omni_condition& operator=(const omni_condition&);
};

// ---- Thread ----
// omni_thread used a static function + void* arg pattern for detached threads.
// We wrap it with a QThread that calls the static function.
class omni_thread {
public:
    enum priority_t {
        PRIORITY_LOW,
        PRIORITY_NORMAL,
        PRIORITY_HIGH
    };

    enum state_t {
        STATE_NEW,
        STATE_RUNNING,
        STATE_TERMINATED
    };

    // Detached thread constructor: void (*fn)(void*)
    omni_thread(void (*fn)(void*), void* arg = 0,
                priority_t pri = PRIORITY_NORMAL)
        : m_fn_void(fn), m_fn_ret(0), m_arg(arg), m_thread(0) {}

    // Undetached thread constructor: void* (*fn)(void*)
    omni_thread(void* (*fn)(void*), void* arg = 0,
                priority_t pri = PRIORITY_NORMAL)
        : m_fn_void(0), m_fn_ret(fn), m_arg(arg), m_thread(0) {}

    ~omni_thread() { delete m_thread; }

    void start() {
        class WorkerThread : public QThread {
        public:
            void (*fn_void)(void*);
            void* (*fn_ret)(void*);
            void* arg;
            WorkerThread(void (*fv)(void*), void* (*fr)(void*), void* a)
                : fn_void(fv), fn_ret(fr), arg(a) {}
            void run() override {
                if (fn_void)
                    fn_void(arg);
                else if (fn_ret)
                    fn_ret(arg);
            }
        };
        m_thread = new WorkerThread(m_fn_void, m_fn_ret, m_arg);
        m_thread->start();
    }

    void join(void** = 0) {
        if (m_thread) m_thread->wait();
    }

    static omni_thread* create(void (*fn)(void*), void* arg = 0,
                               priority_t pri = PRIORITY_NORMAL) {
        omni_thread* t = new omni_thread(fn, arg, pri);
        t->start();
        return t;
    }

    static void yield() { QThread::yieldCurrentThread(); }

    static void sleep(unsigned long secs, unsigned long nsecs = 0) {
        QThread::msleep(secs * 1000 + nsecs / 1000000);
    }

    // get_time: compute absolute time = now + relative offset
    // Used by EmSession::Sleep() before calling timedwait()
    static void get_time(unsigned long* abs_sec, unsigned long* abs_nsec,
                         unsigned long rel_sec = 0, unsigned long rel_nsec = 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        *abs_sec  = ts.tv_sec  + rel_sec;
        *abs_nsec = ts.tv_nsec + rel_nsec;
        if (*abs_nsec >= 1000000000UL) {
            *abs_nsec -= 1000000000UL;
            (*abs_sec)++;
        }
    }

    // NOTE: self() cannot map cleanly to Qt — original omnithread stored
    // a thread-local pointer to the omni_thread object.  Returning nullptr
    // means InCPUThread() always returns false, but the session code handles
    // that path safely (it just takes the non-CPU-thread assertion branch).
    static omni_thread* self() { return 0; }
    static void exit(void* = 0) { QThread::currentThread()->quit(); }

    priority_t priority() { return PRIORITY_NORMAL; }
    state_t state()       { return m_thread ? STATE_RUNNING : STATE_NEW; }
    int id()              { return 0; }

    void set_priority(priority_t) {}

private:
    void (*m_fn_void)(void*);
    void* (*m_fn_ret)(void*);
    void* m_arg;
    QThread* m_thread;
};

// ---- Semaphore ----
class omni_semaphore {
public:
    omni_semaphore(unsigned int initial = 1) : m(), c(&m), value(initial) {}
    ~omni_semaphore() {}

    void wait() {
        m.lock();
        while (value == 0) c.wait();
        value--;
        m.unlock();
    }

    int trywait() {
        m.lock();
        if (value > 0) { value--; m.unlock(); return 1; }
        m.unlock();
        return 0;
    }

    void post() {
        m.lock();
        value++;
        c.signal();
        m.unlock();
    }

private:
    omni_mutex m;
    omni_condition c;
    unsigned int value;

    omni_semaphore(const omni_semaphore&);
    omni_semaphore& operator=(const omni_semaphore&);
};

// Semaphore RAII lock
class omni_semaphore_lock {
public:
    omni_semaphore_lock(omni_semaphore& s) : sem(s) { sem.wait(); }
    ~omni_semaphore_lock() { sem.post(); }
private:
    omni_semaphore& sem;
    omni_semaphore_lock(const omni_semaphore_lock&);
    omni_semaphore_lock& operator=(const omni_semaphore_lock&);
};

// Exception classes
class omni_thread_fatal {
public:
    int error;
    omni_thread_fatal(int e = 0) : error(e) {}
};

class omni_thread_invalid {};

// Macros expected by old header guards
#define OMNI_MUTEX_IMPLEMENTATION
#define OMNI_CONDITION_IMPLEMENTATION
#define OMNI_SEMAPHORE_IMPLEMENTATION
#define OMNI_THREAD_IMPLEMENTATION

// Init class stub (original triggered static initialization)
// No-op in Qt wrapper.
class omni_thread_init_t {
public:
    omni_thread_init_t() {}
    ~omni_thread_init_t() {}
};
static omni_thread_init_t omni_thread_init;

#endif // __omnithread_h_
