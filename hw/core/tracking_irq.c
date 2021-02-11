/*
 *  System (CPU) Bus device support code
 *
 *  Copyright (c) 2009 CodeSourcery
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "hw/core/tracking_irq.h"

bool is_irq_tracking_calling(TrackIrq *ti)
{
    return ti->isCalling;
}

void call_irq_tracking(TrackIrq *ti, const char *msg)
{
    ti->outbuf = qstring_new();
    qstring_append(ti->outbuf, msg);
    ti->isCalling = true;
}

void finish_irq_tracking(TrackIrq *ti)
{
    qobject_unref(ti->outbuf);
    ti->isCalling = false;
}

/*ToDo может по месту вызывать qstring_append*/
void add_irq_to_tracking(TrackIrq *ti, const char *msg)
{
    qstring_append(ti->outbuf, msg);
}