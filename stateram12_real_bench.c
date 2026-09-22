
#define _GNU_SOURCE
#include "stateram9.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAX_OBJECTS 64

typedef struct {
    uint64_t seed;
    int id;
} recipe_ctx;

typedef struct {
    stateram9_region *sr;
    unsigned char *raw;
    recipe_ctx ctx;
    size_t bytes;
    uint64_t segments;
    unsigned char *resident;
    uint64_t *last_used;
} object_t;

typedef struct {
    const char *mode;
    const char *profile;
    double logical_gb;
    int objects;
    size_t segment_mb;
    size_t raw_target_mb;
    int shadow_depth;
    int steps;
    unsigned seed;
    const char *csv_path;
    double base_gap_ms;
} config_t;

static object_t objs[MAX_OBJECTS];
static config_t cfg = {
    .mode="stateram",
    .profile="normal",
    .logical_gb=12.0,
    .objects=24,
    .segment_mb=8,
    .raw_target_mb=3000,
    .shadow_depth=2,
    .steps=2000,
    .seed=12012,
    .csv_path="interactions.csv",
    .base_gap_ms=350.0,
};

static size_t page_size;
static uint64_t tick_id=1;
static size_t tracked_raw_bytes=0;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000.0 + ts.tv_nsec/1e6;
}

static uint64_t rng_state=1;
static uint64_t xorshift64(void) {
    uint64_t x=rng_state;
    x ^= x<<13; x ^= x>>7; x ^= x<<17;
    rng_state=x;
    return x;
}
static double frand01(void) {
    return (xorshift64()>>11) * (1.0/9007199254740992.0);
}

static unsigned char recipe_byte(recipe_ctx *c,uint64_t page,size_t off) {
    uint64_t x=c->seed ^ (page*0x9e3779b97f4a7c15ULL) ^
               (uint64_t)off ^ ((uint64_t)c->id<<32);
    x ^= x>>12; x ^= x<<25; x ^= x>>27;
    return (unsigned char)(x*2685821657736338717ULL);
}

static int recipe(uint64_t page,void *out,size_t n,void *u) {
    recipe_ctx *ctx=(recipe_ctx *)u;
    unsigned char *p=(unsigned char *)out;
    for(size_t i=0;i<n;i++)
        p[i]=recipe_byte(ctx,page,i);
    return 0;
}

static size_t rss_bytes(void) {
    FILE *f=fopen("/proc/self/statm","r");
    if(!f) return 0;
    unsigned long size=0,res=0;
    if(fscanf(f,"%lu %lu",&size,&res)!=2) res=0;
    fclose(f);
    return (size_t)res*page_size;
}

static uint64_t total_missing_faults(void) {
    if(strcmp(cfg.mode,"stateram")!=0) return 0;
    uint64_t x=0;
    for(int i=0;i<cfg.objects;i++)
        x += stateram9_missing_faults(objs[i].sr);
    return x;
}

static uint64_t total_write_faults(void) {
    if(strcmp(cfg.mode,"stateram")!=0) return 0;
    uint64_t x=0;
    for(int i=0;i<cfg.objects;i++)
        x += stateram9_write_faults(objs[i].sr);
    return x;
}

static size_t seg_offset(int oi,uint64_t seg) {
    (void)oi;
    return (size_t)seg*cfg.segment_mb*1024UL*1024UL;
}

static size_t seg_len(int oi,uint64_t seg) {
    size_t off=seg_offset(oi,seg);
    size_t segb=cfg.segment_mb*1024UL*1024UL;
    if(off>=objs[oi].bytes) return 0;
    if(segb>objs[oi].bytes-off) return objs[oi].bytes-off;
    return segb;
}

static void *obj_data(int oi) {
    return strcmp(cfg.mode,"stateram")==0 ?
        stateram9_data(objs[oi].sr) : objs[oi].raw;
}

static int evict_segment(int oi,uint64_t seg,double *evict_ms) {
    if(!objs[oi].resident[seg]) return 0;
    if(strcmp(cfg.mode,"raw")==0) return 0;

    size_t off=seg_offset(oi,seg);
    size_t len=seg_len(oi,seg);
    double t0=now_ms();

    if(stateram9_checkpoint_range(objs[oi].sr,off,len)!=0)
        return -1;
    if(stateram9_dematerialize_safe_range(objs[oi].sr,off,len)!=0)
        return -1;

    double t1=now_ms();
    objs[oi].resident[seg]=0;
    if(tracked_raw_bytes>=len) tracked_raw_bytes-=len;
    if(evict_ms) *evict_ms += (t1-t0);
    return 0;
}

static int make_room(size_t need,int protect_o,uint64_t protect_seg,double *evict_ms) {
    if(strcmp(cfg.mode,"raw")==0) return 0;
    size_t target=cfg.raw_target_mb*1024UL*1024UL;

    while(tracked_raw_bytes+need>target) {
        int best_o=-1;
        uint64_t best_s=0;
        uint64_t best_age=UINT64_MAX;

        for(int oi=0;oi<cfg.objects;oi++) {
            for(uint64_t s=0;s<objs[oi].segments;s++) {
                if(!objs[oi].resident[s]) continue;
                if(oi==protect_o && (s==protect_seg ||
                    s+1==protect_seg || (protect_seg+1==s))) continue;
                if(objs[oi].last_used[s]<best_age) {
                    best_age=objs[oi].last_used[s];
                    best_o=oi; best_s=s;
                }
            }
        }

        if(best_o<0) {
            errno=ENOMEM;
            return -1;
        }

        if(evict_segment(best_o,best_s,evict_ms)!=0)
            return -1;
    }
    return 0;
}

static int materialize_segment(int oi,uint64_t seg,double *evict_ms) {
    if(seg>=objs[oi].segments) return 0;
    if(objs[oi].resident[seg]) {
        objs[oi].last_used[seg]=tick_id++;
        return 0;
    }

    size_t len=seg_len(oi,seg);
    if(make_room(len,oi,seg,evict_ms)!=0)
        return -1;

    if(strcmp(cfg.mode,"stateram")==0) {
        size_t off=seg_offset(oi,seg);
        if(stateram9_materialize_range(objs[oi].sr,off,len)!=0)
            return -1;
    }

    objs[oi].resident[seg]=1;
    objs[oi].last_used[seg]=tick_id++;
    tracked_raw_bytes+=len;
    return 0;
}

static void touch_segment(int oi,uint64_t seg,int do_write) {
    unsigned char *p=(unsigned char *)obj_data(oi);
    size_t off=seg_offset(oi,seg);
    size_t len=seg_len(oi,seg);
    volatile uint64_t sink=0;

    for(size_t x=0;x<len;x+=page_size) {
        size_t pos=off+x;
        sink += p[pos];
        if(do_write && ((x/page_size)%256)==0 && x+32<len)
            p[pos+13] ^= (unsigned char)(0x31 + oi);
    }
    (void)sink;
    objs[oi].last_used[seg]=tick_id++;
}

static int init_objects(void) {
    page_size=(size_t)sysconf(_SC_PAGESIZE);
    if(page_size==0) return -1;

    double total_bytes_d=cfg.logical_gb*1024.0*1024.0*1024.0;
    size_t total_bytes=(size_t)total_bytes_d;
    size_t each=(total_bytes+(size_t)cfg.objects-1)/(size_t)cfg.objects;
    size_t segb=cfg.segment_mb*1024UL*1024UL;
    each=((each+segb-1)/segb)*segb;

    for(int i=0;i<cfg.objects;i++) {
        objs[i].bytes=each;
        objs[i].segments=(each+segb-1)/segb;
        objs[i].resident=calloc(objs[i].segments,1);
        objs[i].last_used=calloc(objs[i].segments,sizeof(uint64_t));
        objs[i].ctx.seed=0x91e10da5c79e7b1dULL + (uint64_t)i*0x10001ULL;
        objs[i].ctx.id=i;
        if(!objs[i].resident || !objs[i].last_used) return -1;

        if(strcmp(cfg.mode,"stateram")==0) {
            objs[i].sr=stateram9_create(each,recipe,&objs[i].ctx);
            if(!objs[i].sr) {
                perror("stateram9_create");
                return -1;
            }
        } else {
            objs[i].raw=mmap(NULL,each,PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(objs[i].raw==MAP_FAILED) {
                objs[i].raw=NULL;
                perror("mmap raw");
                return -1;
            }

            /*
             * Real large-RAM baseline: pre-touch every page so the logical
             * state is actually resident before measured interactions.
             */
            fprintf(stderr,"Pretouch raw object %d/%d (%.1f MiB)\n",
                    i+1,cfg.objects,each/(1024.0*1024.0));
            for(size_t off=0;off<each;off+=page_size) {
                uint64_t pg=off/page_size;
                objs[i].raw[off]=recipe_byte(&objs[i].ctx,pg,0);
            }
            for(uint64_t s=0;s<objs[i].segments;s++) {
                objs[i].resident[s]=1;
                objs[i].last_used[s]=tick_id++;
            }
            tracked_raw_bytes+=each;
        }
    }
    return 0;
}

static void destroy_objects(void) {
    for(int i=0;i<cfg.objects;i++) {
        if(objs[i].sr) stateram9_destroy(objs[i].sr);
        if(objs[i].raw) munmap(objs[i].raw,objs[i].bytes);
        free(objs[i].resident);
        free(objs[i].last_used);
    }
}

static void profile_params(double *switch_p,double *seq_p,double *near_p) {
    if(strcmp(cfg.profile,"bursty")==0) {
        *switch_p=.28; *seq_p=.86; *near_p=.10;
    } else if(strcmp(cfg.profile,"low-locality")==0) {
        *switch_p=.35; *seq_p=.48; *near_p=.30;
    } else if(strcmp(cfg.profile,"high-switch")==0) {
        *switch_p=.68; *seq_p=.78; *near_p=.15;
    } else {
        *switch_p=.32; *seq_p=.82; *near_p=.13;
    }
}

static double profile_gap(void) {
    double u=frand01();
    if(strcmp(cfg.profile,"bursty")==0) {
        if(u<.65) return 20+frand01()*60;
        if(u<.95) return 120+frand01()*430;
        return 550+frand01()*550;
    }
    if(u<.12) return 20+frand01()*60;
    if(u<.82) return 120+frand01()*430;
    return 550+frand01()*550;
}

static int next_object(int current,int step,int switched) {
    static const int route[]={
        0,1,2,3,1,4,2,5,1,3,6,2,7,4,1,8,3,5,9,2,10,1,11,4
    };
    int rn=(int)(sizeof(route)/sizeof(route[0]));
    if(!switched) return current;
    int o=route[step%rn]%cfg.objects;
    if(step && step%17==0)
        o=(o+3)%cfg.objects;
    return o;
}

static int predicted_next_object(int current) {
    /*
     * Small fixed predictor for the first real candidate. It intentionally
     * does not know the future exactly; it learns only a simple route bias:
     * keep the next numbered neighbor's entry warm.
     *
     * Later real traces can replace this with the S7.1 transition learner.
     */
    return (current+1)%cfg.objects;
}

static void usage(const char *p) {
    fprintf(stderr,
      "Usage: %s [options]\n"
      "  --mode stateram|raw\n"
      "  --logical-gb N\n"
      "  --objects N\n"
      "  --segment-mb N\n"
      "  --raw-target-mb N\n"
      "  --shadow-depth N\n"
      "  --steps N\n"
      "  --profile normal|bursty|low-locality|high-switch\n"
      "  --seed N\n"
      "  --csv path\n",p);
}

static int parse_args(int argc,char **argv) {
    for(int i=1;i<argc;i++) {
        if(strcmp(argv[i],"--mode")==0 && i+1<argc) cfg.mode=argv[++i];
        else if(strcmp(argv[i],"--profile")==0 && i+1<argc) cfg.profile=argv[++i];
        else if(strcmp(argv[i],"--logical-gb")==0 && i+1<argc) cfg.logical_gb=strtod(argv[++i],NULL);
        else if(strcmp(argv[i],"--objects")==0 && i+1<argc) cfg.objects=atoi(argv[++i]);
        else if(strcmp(argv[i],"--segment-mb")==0 && i+1<argc) cfg.segment_mb=strtoull(argv[++i],NULL,10);
        else if(strcmp(argv[i],"--raw-target-mb")==0 && i+1<argc) cfg.raw_target_mb=strtoull(argv[++i],NULL,10);
        else if(strcmp(argv[i],"--shadow-depth")==0 && i+1<argc) cfg.shadow_depth=atoi(argv[++i]);
        else if(strcmp(argv[i],"--steps")==0 && i+1<argc) cfg.steps=atoi(argv[++i]);
        else if(strcmp(argv[i],"--seed")==0 && i+1<argc) cfg.seed=(unsigned)strtoul(argv[++i],NULL,10);
        else if(strcmp(argv[i],"--csv")==0 && i+1<argc) cfg.csv_path=argv[++i];
        else if(strcmp(argv[i],"--help")==0) { usage(argv[0]); exit(0); }
        else { fprintf(stderr,"Unknown option: %s\n",argv[i]); return -1; }
    }

    if(cfg.objects<2 || cfg.objects>MAX_OBJECTS ||
       cfg.logical_gb<=0 || cfg.segment_mb<1 ||
       cfg.shadow_depth<0 || cfg.steps<1)
        return -1;

    if(strcmp(cfg.mode,"stateram")!=0 && strcmp(cfg.mode,"raw")!=0)
        return -1;
    return 0;
}

int main(int argc,char **argv) {
    if(parse_args(argc,argv)!=0) {
        usage(argv[0]);
        return 2;
    }
    rng_state=((uint64_t)cfg.seed<<1)|1;

    if(init_objects()!=0) {
        fprintf(stderr,
          "Initialization failed. StateRAM mode requires userfaultfd "
          "MISSING + WP support.\n");
        destroy_objects();
        return 1;
    }

    FILE *csv=fopen(cfg.csv_path,"w");
    if(!csv) { perror("fopen csv"); destroy_objects(); return 1; }
    fprintf(csv,
      "step,profile,mode,object,segment,switched,gap_ms,lateness_ms,"
      "foreground_ms,experienced_ms,background_ms,eviction_ms,"
      "rss_mb,tracked_raw_mb,missing_faults,write_faults\n");

    double switch_p,seq_p,near_p;
    profile_params(&switch_p,&seq_p,&near_p);

    int current=0;
    uint64_t cur_seg=0;
    double next_deadline=now_ms();
    double total_bg=0,total_evict=0;

    for(int step=0;step<cfg.steps;step++) {
        double start=now_ms();
        double lateness=start>next_deadline ? start-next_deadline : 0.0;

        int switched=(step==0 || frand01()<switch_p);
        current=next_object(current,step,switched);

        if(switched) {
            cur_seg=0;
        } else {
            double r=frand01();
            if(r<seq_p) {
                if(cur_seg+1<objs[current].segments) cur_seg++;
            } else if(r<seq_p+near_p) {
                uint64_t jump=2+(xorshift64()%4);
                cur_seg=(cur_seg+jump<objs[current].segments) ?
                    cur_seg+jump : objs[current].segments-1;
            } else {
                cur_seg=xorshift64()%objs[current].segments;
            }
        }

        uint64_t mf0=total_missing_faults();
        uint64_t wf0=total_write_faults();
        double evict_ms=0;

        double fg0=now_ms();
        if(materialize_segment(current,cur_seg,&evict_ms)!=0) {
            perror("materialize foreground");
            break;
        }
        touch_segment(current,cur_seg,(step%13)==0);
        double fg1=now_ms();
        double foreground=fg1-fg0;
        double experienced=lateness+foreground;

        double gap=profile_gap();
        next_deadline += gap;
        if(next_deadline<fg1) {
            /* Deadline remains in the past; next iteration will record lateness. */
        }

        double bg0=now_ms();

        if(strcmp(cfg.mode,"stateram")==0) {
            /*
             * Current-object execution shadowing.
             */
            for(int d=1;d<=cfg.shadow_depth;d++) {
                uint64_t ns=cur_seg+(uint64_t)d;
                if(ns>=objs[current].segments) break;

                double e=0;
                if(materialize_segment(current,ns,&e)!=0)
                    break;
                total_evict+=e;
            }

            /*
             * Small entry capsule for one plausible next object.
             */
            int po=predicted_next_object(current);
            double e=0;
            if(materialize_segment(po,0,&e)==0)
                total_evict+=e;
        }

        double bg1=now_ms();
        double bg=bg1-bg0;
        total_bg+=bg;
        total_evict+=evict_ms;

        size_t rss=rss_bytes();

        fprintf(csv,
          "%d,%s,%s,%d,%" PRIu64 ",%d,%.3f,%.3f,%.3f,%.3f,"
          "%.3f,%.3f,%.3f,%.3f,%" PRIu64 ",%" PRIu64 "\n",
          step,cfg.profile,cfg.mode,current,cur_seg,switched,gap,lateness,
          foreground,experienced,bg,evict_ms,
          rss/(1024.0*1024.0),
          tracked_raw_bytes/(1024.0*1024.0),
          total_missing_faults()-mf0,
          total_write_faults()-wf0);

        fflush(csv);

        double n=now_ms();
        if(n<next_deadline) {
            double sleep_ms=next_deadline-n;
            struct timespec ts;
            ts.tv_sec=(time_t)(sleep_ms/1000.0);
            ts.tv_nsec=(long)((sleep_ms-ts.tv_sec*1000.0)*1e6);
            nanosleep(&ts,NULL);
        }
    }

    fclose(csv);

    fprintf(stderr,
      "Done.\n"
      " mode=%s logical=%.2f GiB objects=%d segment=%zu MiB\n"
      " tracked raw target=%zu MiB current tracked=%.1f MiB RSS=%.1f MiB\n"
      " background work=%.1f ms eviction work=%.1f ms\n"
      " CSV=%s\n",
      cfg.mode,cfg.logical_gb,cfg.objects,cfg.segment_mb,
      cfg.raw_target_mb,tracked_raw_bytes/(1024.0*1024.0),
      rss_bytes()/(1024.0*1024.0),
      total_bg,total_evict,cfg.csv_path);

    destroy_objects();
    return 0;
}
