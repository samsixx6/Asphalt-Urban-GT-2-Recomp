#include "audio_mixer.h"
static int16_t clip(int value) {
    return (int16_t)(value>32767 ? 32767 : value<-32768 ? -32768 : value);
}
void asphalt_audio_push(asphalt_audio_mixer *m,int ch,const int16_t *p,unsigned n,int stereo,int l,int r) {
    if(ch<0 || ch>=ASPHALT_AUDIO_CHANNELS || !p || !n) return;
    asphalt_audio_channel *c=&m->channels[ch];
    m->input_frames+=n;m->channel_mask|=1u<<ch;
    /* Bound latency even when the game gets ahead or the device stalls. */
    if(n>ASPHALT_AUDIO_AHEAD) {
        unsigned skip=n-ASPHALT_AUDIO_AHEAD;
        p+=skip*(stereo?2:1);m->dropped_frames+=skip;n=ASPHALT_AUDIO_AHEAD;
    }
    if(c->count+n>ASPHALT_AUDIO_AHEAD) {
        unsigned skip=c->count+n-ASPHALT_AUDIO_AHEAD;
        c->read=(c->read+skip)%ASPHALT_AUDIO_AHEAD;c->count-=skip;m->dropped_frames+=skip;
    }
    l=l<0?0:l>0x8000?0x8000:l;r=r<0?0:r>0x8000?0x8000:r;
    for(unsigned i=0;i<n;i++) {
        unsigned at=(c->read+c->count+i)%ASPHALT_AUDIO_AHEAD;
        c->pcm[at*2]=clip((int)p[i*(stereo?2:1)]*l/0x8000);
        c->pcm[at*2+1]=clip((int)p[i*(stereo?2:1)+(stereo?1:0)]*r/0x8000);
    }
    c->count+=n;
}
void asphalt_audio_mix(asphalt_audio_mixer *m,int16_t *out,unsigned n) {
    for(unsigned i=0;i<n;i++) {
        int l=0,r=0;
        for(int ch=0;ch<ASPHALT_AUDIO_CHANNELS;ch++) {
            asphalt_audio_channel *c=&m->channels[ch];
            if(!c->count) continue;
            l+=c->pcm[c->read*2];r+=c->pcm[c->read*2+1];
            c->read=(c->read+1)%ASPHALT_AUDIO_AHEAD;c->count--;
        }
        out[i*2]=clip(l);out[i*2+1]=clip(r);
        if(out[i*2] || out[i*2+1]) m->nonzero_frames++;
        unsigned al=out[i*2]<0?-(int)out[i*2]:out[i*2];
        unsigned ar=out[i*2+1]<0?-(int)out[i*2+1]:out[i*2+1];
        if(al>m->peak)m->peak=al;if(ar>m->peak)m->peak=ar;
    }
    m->mixed_frames+=n;
}
