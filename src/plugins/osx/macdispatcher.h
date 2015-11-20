/* This file is part of the KDE libraries
   Copyright (C) 2009 Dario Freddi <drf at kde.org>
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

#ifndef MACPOLLER_H
#define MACPOLLER_H

#include "abstractsystempoller.h"

#include <QAbstractNativeEventFilter>

#include <QTimer>

// Use IOKIT instead of the deprecated Carbon interface
#include <IOKit/IOKitLib.h>
// Use GCD instead of a QTimer
#include <dispatch/dispatch.h>

class QWidget;
class DispatchCallback;

/**
 * This is a modernised Macintosh backend (plugin) implementation for KIdleTime.
 * It uses a Cocoa event filter to detect global and application user input events
 * (which indicate absence/end of idling) and the HIDIdleTime property from IOKit
 * to obtain the time since the last event.
 * 
 * Custom timeouts ("the system has not had input events for X milliseconds") are
 * detected via a polling algorithm that uses an adaptive polling interval in the
 * default configuration that limits its overhead as much as possible while maintaining
 * good detection accuracy. The class does provide a mechanism to switch it to a
 * fixed-frequency polling strategy of configurable resolution, but that mechanism
 * isn't yet accessible via the KIdleTime class.
 * 
 * @note polling comes at a cost. This cost is minimised with the default, adaptive interval
 * configuration, but applications should not let the KIdleTime instance active when it 
 * is not needed. This OS X backend allows to deactivate KIdleTime by removing all timeouts;
 * @see KIdleTime::removeAllIdleTimeouts.
 */
class OSXIdleDispatcher: public AbstractSystemPoller
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.kidletime.AbstractSystemPoller" FILE "osx.json")
    Q_INTERFACES(AbstractSystemPoller)

public:
    OSXIdleDispatcher(QObject *parent = 0);
    virtual ~OSXIdleDispatcher();

    bool isAvailable();
    bool setUpPoller();
    void unloadPoller();

public Q_SLOTS:
    void addTimeout(int nextTimeout);
    void removeTimeout(int nextTimeout);
    QList<int> timeouts() const;
    int forcePollRequest();
    void catchIdleEvent();
    void stopCatchingIdleEvents();
    void simulateUserActivity();

private Q_SLOTS:
    void checkForIdle();
    void detectedActivity();

private:
    /**
     * Query IOKit for the current HIDIdleTime value, and return it. Also compares the current idle
     * time to the registered list of timeouts, and emits a \c timeoutReached signal when
     * a hit is found.
     * @param allowEmits : should timeoutReached() signals be emitted?
     * @param idle : returns the current true idle time (time without input events)
     * @returns : the simulated idle time (time without input events and since the last
     * call to simulateUserActivity).
     */
    int64_t poll(bool allowEmits, int64_t &idle);
    /**
     * Query IOKit for the current HIDIdleTime value, and return it. Also compares the current idle
     * time to the registered list of timeouts, and emits a \c timeoutReached signal when
     * a hit is found.
     * @param allowEmits : should timeoutReached() signals be emitted?
     * @returns : the simulated idle time (time without input events and since the last
     * call to simulateUserActivity).
     */
    int64_t poll(bool allowEmits);
    void resumedFromIdle();
    /**
     * when the adaptive interval is used, this function reconfigures the idle polling
     * timer as a function of the current idle time. The timer is stopped when no timeouts
     * are to be monitored.
     */
    void kickTimer(int64_t idle);
    /**
     * sets up the Cocoa global events filter.
     * @returns true in case of success
     */
    bool additionalSetUp();
    /**
     * takes down the Cocoa global events filter.
     */
    void additionalUnload();
    void checkForIdleFunction();
    QList<int> m_timeouts;
    mach_port_t ioPort;
    io_iterator_t ioIterator;
    io_object_t ioObject;
    dispatch_source_t m_idleDispatch;
    bool m_idleDispatchRunning;
    dispatch_time_t timerSet;
    int m_minTimeout,
        m_maxTimeout,
        m_NTimeouts;
    int m_lastTimeout,
        m_nextTimeout;
    int64_t m_realIdle,
        m_idleOffset;
    bool m_catch;
    bool m_available;
    /**
     * instance of a class that "contains" the Cocoa global event filter.
     */
    QAbstractNativeEventFilter *m_nativeGrabber;
    friend class CocoaEventFilter;
    friend class DispatchCallback;
};

#endif /* MACPOLLER_H */
