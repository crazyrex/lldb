//===-- POSIXStopInfo.h -----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_POSIXStopInfo_H_
#define liblldb_POSIXStopInfo_H_

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Target/StopInfo.h"

#include "POSIXThread.h"
#include "ProcessMessage.h"

//===----------------------------------------------------------------------===//
/// @class POSIXStopInfo
/// @brief Simple base class for all POSIX-specific StopInfo objects.
///
class POSIXStopInfo
    : public lldb_private::StopInfo
{
public:
    POSIXStopInfo(lldb_private::Thread &thread, uint32_t status)
        : StopInfo(thread, status)
        { }
};

//===----------------------------------------------------------------------===//
/// @class POSIXLimboStopInfo
/// @brief Represents the stop state of a process ready to exit.
///
class POSIXLimboStopInfo
    : public POSIXStopInfo
{
public:
    POSIXLimboStopInfo(POSIXThread &thread)
        : POSIXStopInfo(thread, 0)
        { }

    ~POSIXLimboStopInfo();

    lldb::StopReason
    GetStopReason() const;

    const char *
    GetDescription();

    bool
    ShouldStop(lldb_private::Event *event_ptr);

    bool
    ShouldNotify(lldb_private::Event *event_ptr);
};


//===----------------------------------------------------------------------===//
/// @class POSIXCrashStopInfo
/// @brief Represents the stop state of process that is ready to crash.
///
class POSIXCrashStopInfo
    : public POSIXStopInfo
{
public:
    POSIXCrashStopInfo(POSIXThread &thread, uint32_t status, 
                  ProcessMessage::CrashReason reason)
        : POSIXStopInfo(thread, status),
          m_crash_reason(reason)
        { }

    ~POSIXCrashStopInfo();

    lldb::StopReason
    GetStopReason() const;

    const char *
    GetDescription();

    ProcessMessage::CrashReason
    GetCrashReason() const;

private:
    ProcessMessage::CrashReason m_crash_reason;
};    

#endif
