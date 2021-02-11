#ifndef TRACKING_IRQ_H
#define TRACKING_IRQ_H

#include "qapi/qmp/qstring.h"
#include <stdbool.h>

typedef struct TrackIrq {
    bool isCalling;
    QString *outbuf;
} TrackIrq;

bool is_irq_tracking_calling(TrackIrq *ti);
void call_irq_tracking(TrackIrq *ti, const char *msg);
void finish_irq_tracking(TrackIrq *ti);
void add_irq_to_tracking(TrackIrq *ti, const char *msg);

#endif