#ifndef ASPHALT_NATIVE_PACKETS_H
#define ASPHALT_NATIVE_PACKETS_H
#include <stdint.h>
enum { ASPHALT_PACKET_WAIT,ASPHALT_PACKET_START,ASPHALT_PACKET_FINISH };
enum { ASPHALT_PACKET_PRE,ASPHALT_PACKET_STEP,ASPHALT_PACKET_POST };
typedef struct {
 float raw[3],sim[3],step[3];
 unsigned pending,total,kind,active,executed;
 uint64_t stages,packets,steps;
} asphalt_native_packets;
typedef int (*asphalt_native_packet_emit)(void *user,int event,const float delta[3]);
void asphalt_native_packets_reset(asphalt_native_packets *p);
/* delta is a COMPLETE original-clock packet, never an individual native-frame
 * delta. NULL drains a pending tail without introducing time. A new packet
 * cannot overwrite an unfinished old one: finish it before polling new input. */
int asphalt_native_packets_begin(asphalt_native_packets *p,const float delta[3]);
/* effective is the original group's transformed/clamped vector. START uses
 * floor(x*rate)+1 steps, retaining the original full y/z values. split exposes
 * two native slots; false consumes all steps when a late frame needs catch-up.
 * Repeated run is idempotent. An emitter's nonzero return cancels the context. */
int asphalt_native_packets_run(asphalt_native_packets *p,const float effective[3],float rate,int split,asphalt_native_packet_emit emit,void *user);
#endif
