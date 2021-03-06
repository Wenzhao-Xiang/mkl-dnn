/*******************************************************************************
* Copyright 2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <float.h>
#include <math.h>
#include <time.h>

#include "mkldnn.h"

#include "mkldnn_common.hpp"
#include "mkldnn_memory.hpp"
#include "norm.hpp"

#include "shuffle/shuffle.hpp"

namespace shuffle {

inline float saturate(float value, float min, float max) {
    return MAX2(min, MIN2(max, value));
}

int fill_memory(const prb_t *p, dnn_mem_t &mem) {
    dt_conf_t c_src;
    switch (p->dt) {
        case mkldnn_u8: c_src = conf_u8; break;
        case mkldnn_s8: c_src = conf_s8; break;
        case mkldnn_s32: c_src = conf_s32; break;
        default: c_src = conf_f32; break;
    }
    const int range = c_src.range;
    const int max = c_src.min + range - 1;

    const size_t nelems = mem.nelems();

    for (size_t idx = 0; idx < nelems; ++idx) {
        float value = saturate((float)(idx % c_src.range), c_src.min, max);
        mem.set_elem(idx, value);
    }

    return OK;
}

static int compare(const prb_t *p, const dnn_mem_t &fp_mem,
        const dnn_mem_t &dt_mem, res_t *r) {
    size_t nelems = fp_mem.nelems();
    assert(nelems == dt_mem.nelems());
    r->errors = 0;

    for (size_t i = 0; i < nelems; ++i) {
        const float fp = fp_mem.get_elem(i);
        const float dt = dt_mem.get_elem(i);
        const float diff = fabsf(fp - dt);
        if (r->errors < 10 && diff != 0.0) {
            printf("idx: %zu fp: %f dt:%f\n", i, fp, dt);
            r->errors++;
        }
    }

    if (r->errors)
        r->state = FAILED;

    if (r->state == UNTESTED)
        r->state = PASSED; /* optimism */

    return r->state == FAILED ? FAIL : OK;
}

static int init_pd(const prb_t *p, mkldnn_shuffle_desc_t &sd,
        mkldnn_primitive_desc_t &spd, res_t *r) {

    mkldnn_memory_desc_t data_d;
    mkldnn_dims_t data_dims;
    const int ndims = (int)p->dims.size();

    for (int i = 0; i < ndims; ++i) data_dims[i] = p->dims[i];
    DNN_SAFE(mkldnn_memory_desc_init(&data_d, ndims, data_dims, p->dt, p->fmt),
           WARN);

    mkldnn_status_t init_status = mkldnn_success;
    mkldnn_primitive_desc_t hint_fwd_pd = NULL;
    if (p->dir == FWD_D) {
        auto prop = mkldnn_forward_training;
        DNN_SAFE(mkldnn_shuffle_forward_desc_init(&sd, prop,
                    &data_d, p->a, p->g), WARN);
    } else if (p->dir == BWD_D) {
        DNN_SAFE(mkldnn_shuffle_backward_desc_init(&sd, &data_d, p->a,
                    p->g), WARN);
        mkldnn_shuffle_desc_t sd_fwd;
        DNN_SAFE(mkldnn_shuffle_forward_desc_init(&sd_fwd,
                    mkldnn_forward_training, &data_d, p->a, p->g), WARN);
        DNN_SAFE(mkldnn_primitive_desc_create(&hint_fwd_pd, &sd_fwd, engine,
                    NULL), WARN);
    }
    init_status = mkldnn_primitive_desc_create(&spd, &sd, engine, hint_fwd_pd);
    mkldnn_primitive_desc_destroy(hint_fwd_pd);

    if (init_status == mkldnn_unimplemented)
        return r->state = UNIMPLEMENTED, OK;
    else
        SAFE(init_status, WARN);

    const char *impl_str = query_impl_info(spd);
    print(5, "mkldnn implementation: %s\n", impl_str);

    return OK;
}

int doit(const prb_t *p, res_t *r) {

    res_t res_zero{};
    *r = res_zero;

    mkldnn_shuffle_desc_t sd;
    mkldnn_primitive_desc_t spd;
    mkldnn_primitive_t s{};

    SAFE(init_pd(p, sd, spd, r), WARN);
    if (r->state == SKIPPED || r->state == UNIMPLEMENTED)
        return OK;

    const auto fp = p->dt;
    auto &src_dt_d = sd.data_desc;

    const int ndims = (int)p->dims.size();
    const auto src_format = (ndims == 1)
           ? mkldnn_x
           : (ndims == 2)
           ? mkldnn_nc
           : get_default_format(ndims, fmt2data_kind(p->fmt));

    dnn_mem_t data_fp(src_dt_d, fp, src_format),
              data_dt(src_dt_d);
    dnn_mem_t d_data_fp(src_dt_d, fp, src_format),
              d_data_dt(src_dt_d);

    SAFE(fill_memory(p, data_fp), WARN);

    if (p->dir & FLAG_FWD) {
        mkldnn_primitive_at_t inputs[1];
        const_mkldnn_primitive_t outputs[1];
        SAFE(data_dt.reorder(data_fp), WARN);
        inputs[0] = {data_dt.p_, 0};
        outputs[0] = d_data_dt.p_;
        DNN_SAFE(mkldnn_primitive_create(&s, spd, inputs, outputs), WARN);
        SAFE(execute(s), WARN);
        if (bench_mode & CORR) {
            compute_shuffle(p, data_fp, d_data_fp);
            dnn_mem_t data(d_data_dt.md_, fp, src_format);
            SAFE(data.reorder(d_data_dt), WARN);
            SAFE(compare(p, d_data_fp, data, r), WARN);
        }
    } else if (p->dir == BWD_D) {
        mkldnn_primitive_at_t inputs[1];
        const_mkldnn_primitive_t outputs[1];
        SAFE(data_dt.reorder(data_fp), WARN);
        inputs[0] = {data_dt.p_, 0};
        outputs[0] = d_data_dt.p_;
        DNN_SAFE(mkldnn_primitive_create(&s, spd, inputs, outputs), WARN);
        DNN_SAFE_V(mkldnn_primitive_desc_destroy(spd));
        SAFE(execute(s), WARN);
        if (bench_mode & CORR) {
            compute_shuffle(p, data_fp, d_data_fp);
            dnn_mem_t data(d_data_dt.md_, fp, src_format);
            SAFE(data.reorder(d_data_dt), WARN);
            SAFE(compare(p, d_data_fp, data, r), WARN);
        }
    }

    if (bench_mode & PERF) {
        auto &t = r->timer;
        t.reset();
        while (true) {
            SAFE(execute(s), WARN);
            t.stamp();
            const bool stop = false
                || (fix_times_per_prb && t.times() >= fix_times_per_prb)
                || (!fix_times_per_prb
                        && t.total_ms() >= max_ms_per_prb
                        && t.times() >= min_times_per_prb);
            if (stop) break;
        }
    }

    DNN_SAFE_V(mkldnn_primitive_destroy(s));
    return OK;
}

}
