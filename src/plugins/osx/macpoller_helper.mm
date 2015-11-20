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
#include "macpoller.h"
#include <CoreServices/CoreServices.h>

#include <QApplication>

// See http://stackoverflow.com/questions/19229777/how-to-detect-global-mouse-button-events for 
// background relative to using this approach inspired by WidgetBasedPoller.

#import <AppKit/AppKit.h>

class CocoaEventFilter : public QAbstractNativeEventFilter
{
public:
    const static int mask = NSLeftMouseDownMask | NSLeftMouseUpMask | NSRightMouseDownMask
                    | NSRightMouseUpMask | NSOtherMouseDownMask | NSOtherMouseUpMask
                    | NSLeftMouseDraggedMask | NSRightMouseDraggedMask | NSOtherMouseDraggedMask
                    | NSMouseMovedMask | NSScrollWheelMask | NSTabletPointMask | NSKeyDownMask;

    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result)
    {
        Q_UNUSED(eventType)
        Q_UNUSED(result)
        Q_UNUSED(message)
        if (poller->m_catch) {
            // don't call out of this function if unnecessary
            poller->detectedActivity();
        }
        if (poller->m_NTimeouts) {
            poller->poll(true);
        }
        return false;
    };

    OSXIdlePoller *poller;
    id m_monitorId;
};

bool OSXIdlePoller::additionalSetUp()
{
    CocoaEventFilter *nativeGrabber = new CocoaEventFilter;
    bool ret = false;
    if (nativeGrabber) {
        // quick and dirty, no real point in going through a setter
        nativeGrabber->poller = this;
        nativeGrabber->m_monitorId = 0;
        m_nativeGrabber = nativeGrabber;
        QCoreApplication::processEvents();
        @autoreleasepool {
            nativeGrabber->m_monitorId = [NSEvent addGlobalMonitorForEventsMatchingMask:CocoaEventFilter::mask
                handler:^(NSEvent* event) { m_nativeGrabber->nativeEventFilter("NSEventFromGlobalMonitor", event, 0); }];
        }
        if (nativeGrabber->m_monitorId) {
            qApp->installNativeEventFilter(m_nativeGrabber);
            QCoreApplication::processEvents();
            ret = true;
        } else {
            qCWarning(KIDLETIME) << "Failure installing the global native event filter";
            delete nativeGrabber;
            m_nativeGrabber = 0;
        }
    }
    return ret;
}

void OSXIdlePoller::additionalUnload()
{
    if (m_nativeGrabber) {
        CocoaEventFilter *nativeGrabber = static_cast<CocoaEventFilter*>(m_nativeGrabber);
        qApp->removeNativeEventFilter(m_nativeGrabber);
        if (nativeGrabber->m_monitorId) {
            @autoreleasepool {
                 [NSEvent removeMonitor:nativeGrabber->m_monitorId];
            }
        }
        delete nativeGrabber;
        m_nativeGrabber = 0;
    }
}
