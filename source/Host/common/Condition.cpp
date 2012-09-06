//===-- Condition.cpp -------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <errno.h>

#include "lldb/Host/Condition.h"
#include "lldb/Host/TimeValue.h"


using namespace lldb_private;

//----------------------------------------------------------------------
// Default constructor
//
// The default constructor will initialize a new pthread condition
// and maintain the condition in the object state.
//----------------------------------------------------------------------
Condition::Condition () :
    m_condition()
{
#ifdef _WIN32
    InitializeConditionVariable(&m_condition);
#else
    ::pthread_cond_init (&m_condition, NULL);
#endif
}

//----------------------------------------------------------------------
// Destructor
//
// Destroys the pthread condition that the object owns.
//----------------------------------------------------------------------
Condition::~Condition ()
{
#ifndef _WIN32
    ::pthread_cond_destroy (&m_condition);
#endif
}

//----------------------------------------------------------------------
// Unblock all threads waiting for a condition variable
//----------------------------------------------------------------------
int
Condition::Broadcast ()
{
#ifdef _WIN32
    WakeAllConditionVariable(&m_condition);
    return 0;
#else
    return ::pthread_cond_broadcast (&m_condition);
#endif
}
#ifndef _WIN32
//----------------------------------------------------------------------
// Get accessor to the pthread condition object
//----------------------------------------------------------------------
pthread_cond_t *
Condition::GetCondition ()
{
    return &m_condition;
}
#endif
//----------------------------------------------------------------------
// Unblocks one thread waiting for the condition variable
//----------------------------------------------------------------------
int
Condition::Signal ()
{
#ifdef _WIN32
    WakeConditionVariable(&m_condition);
    return 0;
#else
    return ::pthread_cond_signal (&m_condition);
#endif
}

//----------------------------------------------------------------------
// The Wait() function atomically blocks the current thread
// waiting on the owned condition variable, and unblocks the mutex
// specified by "mutex".  The waiting thread unblocks only after
// another thread calls Signal(), or Broadcast() with the same
// condition variable, or if "abstime" is valid (non-NULL) this
// function will return when the system time reaches the time
// specified in "abstime". If "abstime" is NULL this function will
// wait for an infinite amount of time for the condition variable
// to be signaled or broadcasted.
//
// The current thread re-acquires the lock on "mutex".
//----------------------------------------------------------------------

/* convert struct timeval to ms(milliseconds) */
static unsigned long int tv2ms(struct timeval a) {
    return ((a.tv_sec * 1000) + (a.tv_usec / 1000));
}

int
Condition::Wait (Mutex &mutex, const TimeValue *abstime, bool *timed_out)
{
#ifdef _WIN32
    DWORD wait = INFINITE;
    if (abstime != NULL)
        wait = tv2ms(abstime->GetAsTimeVal());

    int err = SleepConditionVariableCS(&m_condition, (PCRITICAL_SECTION)&mutex,
        wait);

    if (timed_out != NULL)
    {
        if ((err == 0) && GetLastError() == ERROR_TIMEOUT)
            *timed_out = true;
        else
            *timed_out = false;
    }

    return err != 0;
#else
    int err = 0;
    do
    {
        if (abstime && abstime->IsValid())
        {
            struct timespec abstime_ts = abstime->GetAsTimeSpec();
            err = ::pthread_cond_timedwait (&m_condition, mutex.GetMutex(), &abstime_ts);
        }
        else
            err = ::pthread_cond_wait (&m_condition, mutex.GetMutex());
    } while (err == EINTR);

    if (timed_out != NULL)
    {
        if (err == ETIMEDOUT)
            *timed_out = true;
        else
            *timed_out = false;
    }

    return err;
#endif
}

