#!/usr/bin/env python3
"""Exercise the actual DBS port decision functions with mocked CPUFreq policies.

Run on a development host with Python 3 and a C compiler. This does not test
kernel concurrency, CPUFreq driver integration, or phone power/performance.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <limits.h>
typedef uint64_t u64;
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define clamp(x,lo,hi) min(max((x),(lo)),(hi))
#define unlikely(x) (x)
#define READ_ONCE(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define CPUFREQ_RELATION_H 0
#define CPUFREQ_RELATION_L 1
struct dbs_data {
 void *tuners;
 unsigned int sampling_rate, sampling_down_factor, up_threshold;
};
struct cpufreq_policy {
 unsigned int cur, min, max;
 struct { unsigned int min_freq, max_freq; } cpuinfo;
 void *governor_data;
};
struct policy_dbs_info { struct dbs_data *dbs_data; };
static unsigned int sample_load, last_target, calls;
static u64 now_us;
static unsigned int dbs_update(struct cpufreq_policy *p) { return sample_load; }
static u64 ktime_get(void) { return now_us; }
static u64 ktime_to_us(u64 t) { return t; }
static u64 div_u64(u64 a, unsigned int b) { assert(b); return a / b; }
static int __cpufreq_driver_target(struct cpufreq_policy *p, unsigned int f, int r)
{
 assert(f >= p->min && f <= p->max);
 last_target = f; calls++; return 0;
}
'''
TEST_COMMON = r'''
static void sweep(void)
{
 struct cs_dbs_tuners tuners = {0};
 struct dbs_data dbs = {.tuners = &tuners, .sampling_rate = 20000,
                       .sampling_down_factor = 4, .up_threshold = 95};
 struct cs_policy_dbs_info info = {.policy_dbs = {.dbs_data = &dbs}};
 struct cpufreq_policy policy = {.governor_data = &info.policy_dbs};
 unsigned int clusters[] = {1785600, 2419200, 2841600};
 unsigned int c, load, pct, capped;
 setup_tuners(&tuners);
 for (c = 0; c < 3; c++) {
  policy.cpuinfo.min_freq = 300000;
  policy.cpuinfo.max_freq = clusters[c];
  for (capped = 0; capped < 2; capped++) {
   policy.min = 400000;
   policy.max = capped ? 1200000 : clusters[c];
   for (pct = 0; pct <= 100; pct++) {
    policy.cur = policy.min + (policy.max - policy.min) * pct / 100;
    info.requested_freq = policy.cur;
    for (load = 0; load <= 100; load++) {
     sample_load = load;
     now_us += 20000;
     assert(cs_dbs_update(&policy) == dbs.sampling_rate);
    }
   }
  }
 }
 /* A policy limited to one frequency must not overflow or divide by zero. */
 policy.cur = policy.min = policy.max = 300000;
 policy.cpuinfo.min_freq = policy.cpuinfo.max_freq = 300000;
 info.requested_freq = 300000;
 for (load = 0; load <= 100; load++) {
  sample_load = load;
  assert(cs_dbs_update(&policy) == dbs.sampling_rate);
 }
}
'''
TEST_DESPAIR = r'''
static void setup_tuners(struct cs_dbs_tuners *t)
{ t->freq_step = 5; t->down_threshold = 35; t->twostep_threshold = 65; }
static void behavior(void)
{
 struct cs_dbs_tuners t; setup_tuners(&t);
 struct dbs_data d = {.tuners=&t, .sampling_rate=20000,
                     .sampling_down_factor=1, .up_threshold=95};
 struct cs_policy_dbs_info a={.policy_dbs={.dbs_data=&d},.requested_freq=600000};
 struct cs_policy_dbs_info b={.policy_dbs={.dbs_data=&d},.requested_freq=600000};
 struct cpufreq_policy p={.min=300000,.max=2000000,.cur=600000,
                         .governor_data=&a.policy_dbs};
 sample_load=70; now_us=20000; cs_dbs_update(&p);
 assert(a.requested_freq==650000 && a.twostep_counter==1);
 assert(b.requested_freq==600000 && b.twostep_counter==0);
 sample_load=100; cs_dbs_update(&p); assert(a.requested_freq==750000);
 sample_load=0; cs_dbs_update(&p); assert(a.requested_freq==650000);
 t.freq_step=0; calls=0; cs_dbs_update(&p); assert(!calls);
 p.max=500000; t.freq_step=5; sample_load=100;
 cs_dbs_update(&p); assert(last_target==500000);
}
'''
TEST_ELEMENTALX = r'''
static void setup_tuners(struct cs_dbs_tuners *t)
{ t->down_differential=20; t->active_floor_freq=960000; t->powersave=0; }
static void behavior(void)
{
 struct cs_dbs_tuners t; setup_tuners(&t);
 struct dbs_data d={.tuners=&t,.sampling_rate=20000,
                   .sampling_down_factor=4,.up_threshold=90};
 struct cs_policy_dbs_info a={.policy_dbs={.dbs_data=&d}};
 struct cpufreq_policy p={.min=300000,.max=2000000,.cur=300000,
   .cpuinfo={.min_freq=300000,.max_freq=2000000},.governor_data=&a.policy_dbs};
 sample_load=100; cs_dbs_update(&p); assert(last_target==p.max);
 p.cur=1000000; sample_load=0; cs_dbs_update(&p);
 assert(last_target==960000);
 p.max=700000; p.cur=700000; cs_dbs_update(&p);
 assert(last_target<=700000);
 t.down_differential=d.up_threshold; calls=0;
 cs_dbs_update(&p); assert(!calls); /* concurrent tuning snapshot */
 t.down_differential=20;
 for (unsigned int save=0; save<2; save++)
  for (unsigned int hz=300000; hz<=2000000; hz+=10000) {
   p.cur=hz; p.max=2000000;
   unsigned int f=ex_ramp(&p,100,100,save);
   assert(f>=hz && f<=p.max);
  }
}
'''

def main():
    for name, test in [('despair', TEST_DESPAIR), ('elementalx', TEST_ELEMENTALX)]:
        source = (ROOT / f'drivers/cpufreq/cpufreq_{name}.c').read_text()
        decision_code = source.split('/************************** sysfs interface')[0]
        decision_code = '\n'.join(l for l in decision_code.splitlines()
                                  if not l.startswith('#include'))
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'test.c'
            binary = Path(tmp) / 'test'
            c.write_text(STUBS + decision_code + test + TEST_COMMON +
                         '\nint main(void) { behavior(); sweep(); return 0; }\n')
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1',
                            '-fsanitize=address,undefined', str(c), '-o', str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)
        print(f'{name}: frequency bounds, cluster independence and decision tests PASS')

if __name__ == '__main__':
    main()
