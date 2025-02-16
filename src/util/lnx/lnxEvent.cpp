/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2014-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "palAutoBuffer.h"
#include "palEvent.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/timerfd.h>

namespace Util
{

// Represents an invalid event handle (file descriptor) on Linux platforms.
const Event::EventHandle Event::InvalidEvent = -1;

// =====================================================================================================================
Event::Event()
    :
    m_hEvent(InvalidEvent),
    m_isReference(false)
{
}

// =====================================================================================================================
Event::~Event()
{
    if (m_hEvent != InvalidEvent)
    {
        const int32 result = close(m_hEvent);
        PAL_ALERT(result == -1);

        m_hEvent = InvalidEvent;
    }
}

// =====================================================================================================================
// Used to initialize a static event object, not needed (although not dangerous) for dynamic event objects.
//
// On Linux, we're using "eventfd" objects to represent the manual-reset event used on Windows platforms.  An eventfd is
// a file descriptor which can be used as an wait/notify mechanism by userspace applications and by the kernel to notify
// userspace applications of events.  This mechanism was chosen because it is the most likely candidate for the kernel
// graphics driver to be able to notify the UMD of event ocurrences.
//
// SEE: http://man7.org/linux/man-pages/man2/eventfd.2.html
Result Event::Init(
    const EventCreateFlags& flags)
{
    Result result = Result::Success;
    PAL_ASSERT(flags.manualReset == true);

    const uint32 initialState = flags.initiallySignaled ? 1 : 0;

    int32 eventflags = 0;
    eventflags |= flags.closeOnExecute ? EFD_CLOEXEC   : 0;
    eventflags |= flags.nonBlocking    ? EFD_NONBLOCK  : 0;
    eventflags |= flags.semaphore      ? EFD_SEMAPHORE : 0;

    m_hEvent = eventfd(initialState, eventflags);

    if (m_hEvent == InvalidEvent)
    {
        result = Result::ErrorInitializationFailed;
    }

    return result;
}

// =====================================================================================================================
// Sets the Event object (i.e., puts it into a signaled state).
Result Event::Set() const
{
    Result result = Result::ErrorUnavailable;

    // According to the Linux man pages for eventfd, writing data to a non-blocking, non-semaphore eventfd will add the
    // data contained in the supplied buffer to the eventfd object's current counter.  It is invalid to add a negative
    // number to the eventfd object's counter using this function.  If the write will cause the event counter to
    // overflow, nothing happens and -1 is returned (errno is set to EAGAIN).

    if (m_hEvent != InvalidEvent)
    {
        const uint64 incrementValue = 1;
        if (write(m_hEvent, &incrementValue, sizeof(incrementValue)) < 0)
        {
            // EAGAIN indicates that the event's counter would have overflowed. This should never happen with us adding
            // 1 each time, because we'd need 2^64-1 calls to Set() between calls to Reset().
            PAL_ASSERT(errno == EAGAIN);
        }

        result = Result::Success;
    }

    return result;
}

// =====================================================================================================================
// Resets the Event object (i.e., puts it into a non-signaled state).
Result Event::Reset() const
{
    Result result = Result::ErrorUnavailable;

    // According to the Linux man pages for eventfd, reading data from a non-blocking, non-semaphore eventfd will cause
    // the eventfd object's current counter to be copied to the output buffer and the counter to be reset to zero if the
    // counter has a nonzero value at the time the read is attempted. If the read is attempted when the event is already
    // in the non-signaled state, nothing happens and -1 is returned (errno is set to EAGAIN).

    if (m_hEvent != InvalidEvent)
    {
        uint64 previousValue = 0;
        if (read(m_hEvent, &previousValue, sizeof(previousValue)) < 0)
        {
            // EAGAIN indicates the event was already in the non-signaled state.
            PAL_ASSERT(errno == EAGAIN);
        }

        result = Result::Success;
    }

    return result;
}

// =====================================================================================================================
Result Event::Open(
    EventHandle handle,
    bool        isReference)
{
    return Result::Unsupported;
}

// =====================================================================================================================
// Wait for the Event object to become set.
Result Event::Wait(
    float timeout // Max time to wait, in seconds.
    ) const
{
    Result result = Result::Success;

    if (timeout < 0.0f)
    {
        result = Result::ErrorInvalidValue;
    }
    else
    {
        pollfd socketPollFd;
        socketPollFd.revents = 0;
        socketPollFd.fd = m_hEvent;
        socketPollFd.events = POLLIN;

        int ret = poll(&socketPollFd, 1, timeout * 1000);  // Timeout in mS

        if (ret == -1)
        {
            // An unknown error occurred.
            result = Result::ErrorUnknown;
        }
        else if (ret == 0)
        {
            // Timeout ocurred!
            result = Result::Timeout;
        }
    }

    return result;
}
} // Util
