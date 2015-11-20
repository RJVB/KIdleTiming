/* This file is part of the KDE libraries
   Copyright (C) 2009 Dario Freddi <drf at kde.org>
   Copyright (C) 2003 Tarkvara Design Inc.  (from KVIrc source code)
   Copyright (c) 2008 Roman Jarosz          <kedgedev at centrum.cz>
   Copyright (c) 2008 the Kopete developers <kopete-devel at kde.org>
   Copyright (C) 2015 René J.V. Bertin <rjvbertin at gmail.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "logging.h"
#include "macdispatcher.h"
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

// #include <QDebug>
#include <QTimer>

#include <algorithm>

#include <stdio.h>

typedef OSErr(*UpdateSystemActivityPtr)(UInt8 activity);
static UpdateSystemActivityPtr updateSystemActivity;

class DispatchCallback
{
public:
    static void idleHandler(void *ref);
};

OSXIdleDispatcher::OSXIdleDispatcher(QObject *parent)
    : AbstractSystemPoller(parent)
    , ioPort(0)
    , ioIterator(0)
    , ioObject(0)
    , m_minTimeout(-1)
    , m_maxTimeout(-1)
    , m_lastTimeout(-1)
    , m_nextTimeout(-1)
    , m_NTimeouts(0)
    , m_catch(false)
    , m_idleDispatch(0)
    , m_nativeGrabber(0)
    , m_available(true)
{
}

OSXIdleDispatcher::~OSXIdleDispatcher()
{
    unloadPoller();
}

void OSXIdleDispatcher::unloadPoller()
{
    if (m_idleDispatch) {
        if (dispatch_source_testcancel(m_idleDispatch) || !m_idleDispatchRunning) {
            dispatch_resume(m_idleDispatch);
        }
        dispatch_release(m_idleDispatch);
        m_idleDispatch = 0;
    }
    additionalUnload();
    if (ioObject) {
        IOObjectRelease( ioObject );
        ioObject = 0;
    }
    if (ioIterator) {
        IOObjectRelease( ioIterator );
        ioIterator = 0;
    }
    m_lastTimeout = -1;
    m_nextTimeout = -1;
    m_available = false;
}

bool OSXIdleDispatcher::isAvailable()
{
    return m_available;
}

bool OSXIdleDispatcher::setUpPoller()
{
    // May already be init'ed.
    if (ioObject) {
        return true;
    }

    // The easiest way to simulate user activity is to call UpdateSystemActivity(), but that function has
    // sadly been deprecated. Hence the attempt to load it dynamically from the framework that provides/d it.
    static CFBundleRef csBundle = 0;
    if (!csBundle) {
        csBundle = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.CoreServices"));
    }
    if (csBundle) {
        updateSystemActivity = (UpdateSystemActivityPtr) CFBundleGetFunctionPointerForName(csBundle, CFSTR("UpdateSystemActivity"));
        if (!updateSystemActivity) {
            qCWarning(KIDLETIME) << "failed to load UpdateSystemActivity from CoreServices.framework";
        }
    } else {
        updateSystemActivity = 0;
        qCWarning(KIDLETIME) << "failed to load CoreServices.framework: UpdateSystemActivity not available";
    }

    kern_return_t status;
    // establish the connection with I/O Kit, on the default port (MACH_PORT_NULL).
    status = IOMasterPort( MACH_PORT_NULL, &ioPort );
    if (status != KERN_SUCCESS) {
        qCWarning(KIDLETIME) << "could not establish a connection with I/O Kit on the default port";
        return false;
    }
    // We will use the IOHID service which will allow us to know about user interaction.
    // Get an iterator on the I/O Kit services, so we can access IOHID:
    status = IOServiceGetMatchingServices( ioPort, IOServiceMatching("IOHIDSystem"), &ioIterator );
    if (status != KERN_SUCCESS) {
        ioIterator = 0;
        qCWarning(KIDLETIME) << "could not get an iterator on the I/O Kit services, to access IOHID";
        return false;
    }
    // get the actual IOHID service object:
    ioObject = IOIteratorNext(ioIterator);
    if (!ioObject) {
        qCWarning(KIDLETIME) << "could not get the actual IOHID service object";
        return false;
    }
    IOObjectRetain(ioObject);
    IOObjectRetain(ioIterator);

    m_idleDispatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
        0, DISPATCH_TIMER_STRICT, DISPATCH_TARGET_QUEUE_DEFAULT);
    if (m_idleDispatch) {
        dispatch_source_set_event_handler_f(m_idleDispatch, DispatchCallback::idleHandler);
        dispatch_set_context(m_idleDispatch, this);
        m_idleDispatchRunning = false;
    } else {
        qCWarning(KIDLETIME) << "failure creating a GCD timer source";
        return false;
    }

    if (!additionalSetUp()) {
        qCWarning(KIDLETIME) << "failure installing the native Cocoa filter for detecting end-of-idle events";
        return false;
    }
    m_realIdle = 0;
    m_available = true;

    return true;
}

QList<int> OSXIdleDispatcher::timeouts() const
{
    return m_timeouts;
}

void OSXIdleDispatcher::addTimeout(int nextTimeout)
{
    if (!m_timeouts.contains(nextTimeout)) {
        m_timeouts.append(nextTimeout);
        std::sort(m_timeouts.begin(), m_timeouts.end());
        if (nextTimeout < m_minTimeout || m_minTimeout < 0) {
            m_minTimeout = nextTimeout;
        }
        if (nextTimeout > m_maxTimeout) {
            m_maxTimeout = nextTimeout;
        }
        m_NTimeouts = m_timeouts.count();
        // this is about the only place except for setUpPoller() where
        // we can reset m_realIdle;
        m_realIdle = 0;
        kickTimer(poll(false));
    }
}

void OSXIdleDispatcher::removeTimeout(int timeout)
{
    m_timeouts.removeOne(timeout);
    std::sort(m_timeouts.begin(), m_timeouts.end());
    m_minTimeout = -1, m_maxTimeout = -1;
    Q_FOREACH (int i, m_timeouts) {
        if (i < m_minTimeout || m_minTimeout < 0) {
            m_minTimeout = i;
        }
        if (i > m_maxTimeout) {
            m_maxTimeout = i;
        }
    }
    if (m_lastTimeout == timeout) {
        m_lastTimeout = -1;
        m_nextTimeout = -2;
    }
    m_NTimeouts = m_timeouts.count();
    poll(false);
}

void DispatchCallback::idleHandler(void *ref)
{
    OSXIdleDispatcher *poller = static_cast<OSXIdleDispatcher*>(ref);
    int64_t elapsed = dispatch_time(DISPATCH_TIME_NOW, 0ll) - poller->timerSet;
    fprintf( stderr, "idleHandler called after %gms\n", elapsed / 1000000.0 ); fflush(stderr);
    poller->checkForIdleFunction();
}

void OSXIdleDispatcher::kickTimer(int64_t idle)
{
    if (!m_NTimeouts) {
        qDebug() << "kickTimer called with an empty timeouts list";
    } else {
        if (m_nextTimeout < 0) {
            m_nextTimeout = m_minTimeout;
        }
        int64_t currentMinTimeout = m_nextTimeout;
        // change the poll timer interval if there is reason to change it.
        // NB: to minimise CPU load wake-ups to the utmost extent, we could consider an
        // option to set the interval to "remainingTime - 1ms" as long as that is >= 1ms,
        // but then the question becomes how to continue polling from there.
        if (idle < currentMinTimeout) {
            int interval = currentMinTimeout - idle;
//             fprintf( stderr, "idleTimer interval set to (%lld-%lld)/2=%d, next timeout=%d\n",
//                      currentMinTimeout, idle, interval, m_nextTimeout);
            if (m_idleDispatch) {
                timerSet = dispatch_time(DISPATCH_TIME_NOW, 0ll);
                int64_t delta = interval * 1000000ull;
                // set the source to dispatch first when we want to read out the idle time (= ballistically)
                dispatch_source_set_timer(m_idleDispatch, timerSet + delta, DISPATCH_TIME_FOREVER, interval * 10000ull);
                if (!m_idleDispatchRunning) {
                    dispatch_resume(m_idleDispatch);
                    m_idleDispatchRunning = true;
                }
            }
        }
    }
}

int64_t OSXIdleDispatcher::poll(bool allowEmit, int64_t &idle)
{
    kern_return_t status;
    CFTypeRef cfIdle;
    CFTypeID type;
    uint64_t time = 0;
    CFMutableDictionaryRef properties = 0;
    status = IORegistryEntryCreateCFProperties(ioObject, &properties, kCFAllocatorDefault, 0);
    if (status == KERN_SUCCESS && properties) {
        cfIdle = CFDictionaryGetValue(properties, CFSTR("HIDIdleTime"));
        if (cfIdle) {
            CFRetain(cfIdle);
            // cfIdle can have different types: handle them properly:
            type = CFGetTypeID(cfIdle);
            if (type == CFDataGetTypeID()) {
                CFDataGetBytes((CFDataRef)cfIdle, CFRangeMake(0, sizeof(time) ), (UInt8*)&time);
            } else if (type == CFNumberGetTypeID()) {
                CFNumberGetValue((CFNumberRef)cfIdle, kCFNumberSInt64Type, &time);
            }
            CFRelease(cfIdle);
        }
        CFRelease((CFTypeRef)properties);
        // convert nanoseconds to milliseconds:
        idle = int64_t(time / 1000000);
        if (idle < m_realIdle) {
            fprintf(stderr, "idle==%lld < previous value %lld\n", idle, m_realIdle); fflush(stderr);
            // an input event was missed, possibly because additionalSetUp() failed
            resumedFromIdle();
        } else if (idle < m_idleOffset) {
            // reset the idle offset if the idle time dropped below it
            m_idleOffset = 0;
        }
        m_realIdle = idle;
    }

    int64_t offsetIdle = idle - m_idleOffset;
    if (allowEmit) {
        // we don't use Q_FOREACH because we want an easy access to the timeout value after the current one
        QList<int>::const_iterator iter;
        for (iter = m_timeouts.constBegin(); iter != m_timeouts.constEnd(); ++iter) {
            int i = *iter;
            // m_timeouts is sorted so we can simply check for offsetIdle>=i
            if (i != m_lastTimeout && i > m_lastTimeout && offsetIdle >= i) {
                // Bingo!
                m_lastTimeout = i;
                if (i < m_maxTimeout) {
                    m_nextTimeout = iter[1];
                }
                qDebug() << ">>> timeout" << i << "hit at idle" << offsetIdle << "(offset,real idle=="
                    << m_idleOffset << idle << "); next timeout:" << m_nextTimeout;

                if (m_minTimeout > 0) {
                    kickTimer(offsetIdle);
                } else {
                    // there are no valid timeout periods; stop the timer to avoid
                    // useless overhead
                    if (m_idleDispatchRunning) {
                        dispatch_suspend(m_idleDispatch);
                        m_idleDispatchRunning = false;
                    }
                }

                emit timeoutReached(i);
                return offsetIdle;
            }
        }
    }

    // return the "virtual" idle i.e. the time since the last simulateUserActivity(),
    // not the actual idle time!
    return offsetIdle;
}

int64_t OSXIdleDispatcher::poll(bool allowEmits)
{
    int64_t idle;
    return poll(allowEmits, idle);
}

int OSXIdleDispatcher::forcePollRequest()
{
    return int(poll(false));
}

void OSXIdleDispatcher::detectedActivity()
{
    if (m_catch) {
        emit resumingFromIdle();
    }
    stopCatchingIdleEvents();
}

void OSXIdleDispatcher::catchIdleEvent()
{
    m_catch = true;
}

void OSXIdleDispatcher::stopCatchingIdleEvents()
{
    // this slot is called after resumingFromIdle, and should not stop the poll timer
    // because that also drives the timeout detection. The m_catch state variable
    // indicates whether resuming-from-idle events should be caught or not.
    m_catch = false;
}

void OSXIdleDispatcher::resumedFromIdle()
{
    m_idleOffset = 0;
    m_lastTimeout = -1;
    m_nextTimeout = m_minTimeout;
}

void OSXIdleDispatcher::checkForIdleFunction()
{
    if (m_NTimeouts || m_catch) {
        int64_t idle = poll(true);
//         fprintf(stderr, "checkForIdleFunction: idle=%lld, nextTimeout=%d\n", idle, m_nextTimeout); fflush(stderr);
        if (m_NTimeouts && idle < m_nextTimeout) {
            kickTimer(idle);
        }
        if (idle == 0 && m_catch) {
            resumedFromIdle();
            emit resumingFromIdle();
        }
    }
}

void OSXIdleDispatcher::checkForIdle()
{
    checkForIdleFunction();
}

void OSXIdleDispatcher::simulateUserActivity()
{
    // The alternative is to disable sleep using
    //     IOReturn success = IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep,
    //                                                    kIOPMAssertionLevelOn, CFSTR("simulated user activity"), &assertionID);
    // coupled with a timer to re-allow sleep, but there are reports that isn't very reliable.
    if (updateSystemActivity) {
        (*updateSystemActivity)(UsrActivity);
    }
//  this doesn't reset the HIDIdleTime property (and requires ApplicationServices)
//     CGEventRef event = CGEventCreate(nil);
//     CGPoint loc = CGEventGetLocation(event);
//     CGEventRef move1 = CGEventCreateMouseEvent(0, kCGEventMouseMoved,
//         CGPointMake(loc.x+1, loc.y+1), kCGMouseButtonLeft /*ignored*/ );
//     CGEventRef move2 = CGEventCreateMouseEvent(0, kCGEventMouseMoved,
//         loc, kCGMouseButtonLeft /*ignored*/ );
//     CGEventPost(kCGHIDEventTap, move1);
//     CGEventPost(kCGHIDEventTap, move2);
//     CFRelease(move2);
//     CFRelease(move1);
//     CFRelease(event);
    // store an idle offset in order to simulate a (software) reset
    resumedFromIdle();
    kickTimer(poll(false, m_idleOffset));
//     m_nextTimeout = -1;
}

