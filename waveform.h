#ifndef __WAVEFORM_H__
#define __WAVEFORM_H__

#define SCOPE_NCH 4
#define SCOPE_MEM_LENGTH_MAX 12500000 /* DPO5054 default, 12.5M points maximum */

struct waveform_attribute 
{
    unsigned int chMask;
    uint64_t nPt; /* number of points in each event */
    uint64_t nFrames; /* number of Fast Frames in each event, 0 means off */
    double dt;
    double t0;
    double ymult[SCOPE_NCH];
    double yoff[SCOPE_NCH];
    double yzero[SCOPE_NCH];
};

#endif /* __WAVEFORM_H__ */
