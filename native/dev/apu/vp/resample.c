#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "resample.h"

/* hakuX MCPX 8-tap, 16.16 cursor resampler. Preserve the callback's 32-frame
 * refill (including CBO/SSL/notifications and silent-voice decode skip), and
 * the existing per-voice SRC_STATE allocation/reset API. The kernel weights
 * are quantised to 1/65536; measured by raddad772 against retail Xbox APU.
 * Sub-1/256 fractional interpolation is an approximation also used in the
 * reference xemu PR #3105, commit 38e5733.
 */
#define MCPX_REFILL 32
static float lagrange[257][8];
static pthread_once_t lagrange_once=PTHREAD_ONCE_INIT;
static void init_lagrange(void) {
    for(int r=0;r<=256;r++) {
        for(int k=-3;k<=4;k++) {
            double w=1.0;
            for(int j=-3;j<=4;j++) if(j!=k)
                w*=((r/256.0)-j)/(k-j);
            lagrange[r][k+3]=(float)(round(w*65536.0)/65536.0);
        }
    }
}
struct SRC_STATE {
    src_callback_t cb;
    void *cb_data;
    int channels;
    float input[MCPX_REFILL*2];
    int input_len, input_idx;
    float taps[8][2];
    uint32_t pos;
};
SRC_STATE *src_callback_new(src_callback_t cb,int type,int channels,
                            int *error,void *userdata) {
    (void)type;
    if(error) *error=0;
    if(!cb || channels<1 || channels>2) {
        if(error) *error=-1;
        return NULL;
    }
    SRC_STATE *s=calloc(1,sizeof(*s));
    if(!s) { if(error) *error=-1; return NULL; }
    s->cb=cb; s->cb_data=userdata; s->channels=channels;
    s->pos=5u<<16; /* first output aligns at input sample 0 */
    pthread_once(&lagrange_once,init_lagrange);
    return s;
}
int src_reset(SRC_STATE *s) {
    if(!s) return 0;
    s->input_len=s->input_idx=0;
    memset(s->taps,0,sizeof(s->taps));
    s->pos=5u<<16;
    return 0;
}
const char *src_strerror(int error) {
    (void)error;
    return "hakuX MCPX Lagrange resampler";
}
static void read_tap(SRC_STATE *s) {
    if(s->input_idx>=s->input_len) {
        float *data=NULL;
        long n=s->cb(s->cb_data,&data);
        if(n>0 && data) {
            if(n>MCPX_REFILL) n=MCPX_REFILL;
            memcpy(s->input,data,(size_t)n*(size_t)s->channels*sizeof(float));
            s->input_len=(int)n;
        } else {
            s->input_len=0;
        }
        s->input_idx=0;
    }
    memmove(s->taps[0],s->taps[1],7*sizeof(s->taps[0]));
    for(int ch=0;ch<s->channels;ch++)
        s->taps[7][ch]=(s->input_idx<s->input_len) ?
              s->input[s->input_idx*s->channels+ch] : 0.0f;
    if(s->channels==1) s->taps[7][1]=s->taps[7][0];
    if(s->input_idx<s->input_len) s->input_idx++;
}
long src_callback_read_steps(SRC_STATE *s,const uint32_t *steps,
                             long frames,float *out) {
    if(!s || !steps || !out || frames<=0) return 0;
    for(long n=0;n<frames;n++) {
        while(s->pos>=65536u) {
            read_tap(s);
            s->pos-=65536u;
        }
        unsigned idx=s->pos>>8;
        float a=(s->pos&255u)*(1.0f/256.0f);
        float w[8];
        for(int k=0;k<8;k++)
            w[k]=lagrange[idx][k]+(lagrange[idx+1][k]-lagrange[idx][k])*a;
#if defined(__aarch64__)
        if(s->channels==2) {
            float32x2_t acc=vdup_n_f32(0.0f);
            if(s->pos==0) {
                acc=vld1_f32(s->taps[3]);
            } else {
                for(int k=0;k<8;k++)
                    acc=vfma_n_f32(acc,vld1_f32(s->taps[k]),w[k]);
            }
            vst1_f32(out+2*n,acc);
        } else
#endif
        {
            for(int ch=0;ch<s->channels;ch++) {
                float acc=(s->pos==0) ? s->taps[3][ch] : 0.0f;
                if(s->pos) for(int k=0;k<8;k++) acc+=w[k]*s->taps[k][ch];
                out[n*s->channels+ch]=acc;
            }
        }
        /* Avoid arithmetic overflow in the 16.16 accumulator at extreme
         * guest pitch settings; a 64-bit intermediate preserves the full step. */
        uint64_t next=(uint64_t)s->pos+steps[n];
        if(next>UINT32_MAX) {
            /* Extremely large pitch: keep the fractional portion, letting
             * the following samples consume as many input frames as needed. */
            uint64_t whole=next>>16;
            while(whole--) read_tap(s);
            s->pos=(uint32_t)(next&0xFFFFu);
        } else s->pos=(uint32_t)next;
    }
    return frames;
}
long src_callback_read(SRC_STATE *s,double ratio,long frames,float *data) {
    if(!s || !data || frames<=0) return 0;
    if(!(ratio>0.0)) ratio=1.0;
    double x=65536.0/ratio;
    uint32_t step=(uint32_t)fmin(4294967295.0,fmax(1.0,x));
    uint32_t local[32];
    for(int i=0;i<32;i++) local[i]=step;
    long done=0;
    while(done<frames) {
        int n=(int)((frames-done)<32 ? frames-done : 32);
        src_callback_read_steps(s,local,n,data+done*s->channels);
        done+=n;
    }
    return done;
}

void src_float_to_short_array(const float *in, short *out, int len)
{
    if (!in || !out || len <= 0) {
        return;
    }

#if defined(__aarch64__)
    float32x4_t scale = vdupq_n_f32(32767.0f);
    float32x4_t hi = vdupq_n_f32(1.0f);
    float32x4_t lo = vdupq_n_f32(-1.0f);
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        float32x4_t v = vld1q_f32(&in[i]);
        v = vminq_f32(vmaxq_f32(v, lo), hi);
        v = vmulq_f32(v, scale);
        int32x4_t iv = vcvtq_s32_f32(v);
        int16x4_t sv = vqmovn_s32(iv);
        vst1_s16(&out[i], sv);
    }
    for (; i < len; i++) {
        float v = in[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        out[i] = (short)(v * 32767.0f);
    }
#else
    for (int i = 0; i < len; ++i) {
        float v = in[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = (short)(v * 32767.0f);
    }
#endif
}
