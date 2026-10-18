#ifndef TELEMETRY_H
#define TELEMETRY_H
#include <stddef.h>

#define HTTP_PORT 9090
#define FLIP_PORT 9091   /* UDP: one packet of any content = one frame flip */

void   tm_start(void);                       /* starts sampler + flip listener threads */
void   tm_frame_flip(void);                  /* call once per presented frame */
size_t tm_snapshot_json(char *buf, size_t n);/* returns bytes written */
size_t tm_diag_json(char *buf, size_t n);    /* probes which sysctls this console allows */
#endif
