#ifndef SERVICE_TIME_H
#define SERVICE_TIME_H

#include "server.h"

/* Record the event-loop wake time as the start of service time for this client. */
void serviceTime_startTimer(client *c);

/* After a command is fully processed, record it for service time attribution. */
void serviceTime_trackCmd(client *c);

/* Flush service time sample into the per-command histogram (called after write to client). */
void serviceTime_recordLatencies(client *c);

/* Reset service time tracking state on the client (on error or skip). */
void serviceTime_reset(client *c);

#endif
