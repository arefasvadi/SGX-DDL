/*******************************************************************************
* Copyright 2018-2020 Intel Corporation
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

#ifndef RNN_HPP
#define RNN_HPP

#include <assert.h>
#include <limits.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "common.hpp"
#include "dnn_types.hpp"
#include "dnnl_common.hpp"
#include "dnnl_debug.hpp"
#include "dnnl_memory.hpp"
#include "perf_report.hpp"

#define AOC array_offset_calculator

namespace rnn {

enum alg_t { VANILLA_RNN, VANILLA_LSTM, VANILLA_GRU, LBR_GRU };
alg_t str2alg(const char *str);
const char *alg2str(alg_t alg);
dnnl_alg_kind_t alg2kind(alg_t alg);

enum activation_t { UNDEF, RELU, LOGISTIC, TANH };
activation_t str2activation(const char *str);
const char *activation2str(activation_t alg);
dnnl_alg_kind_t activation2kind(activation_t alg);

dnnl_rnn_direction_t str2direction(const char *str);
const char *direction2str(dnnl_rnn_direction_t direction);

enum data_kind_t {
    SRC_LAYER,
    SRC_ITER,
    SRC_ITER_C,
    WEIGHTS_LAYER,
    WEIGHTS_ITER,
    BIAS,
    DST_ITER,
    DST_ITER_C,
    DST_LAYER,

    DIFF_SRC_LAYER,
    DIFF_SRC_ITER,
    DIFF_SRC_ITER_C,
    DIFF_WEIGHTS_LAYER,
    DIFF_WEIGHTS_ITER,
    DIFF_BIAS,
    DIFF_DST_ITER,
    DIFF_DST_ITER_C,
    DIFF_DST_LAYER,

    // FIXME: adding peephole related weights to the appropriate places will
    // cause false-positive accuracy check failures in unrelated test cases
    // (e.g.  backward vanilla RNN for bf16) due to the data fill seed being
    // dependent on the position of the tensor kind in the enum: adding
    // `WEIGHTS_PEEPHOLE` before `dst_*` and `*diff_*` results in initializing
    // the corresponding tensors differently.
    // We need a more robust way of testing RNN.
    WEIGHTS_PEEPHOLE,
    DIFF_WEIGHTS_PEEPHOLE,
    WEIGHTS_PROJECTION,
    DIFF_WEIGHTS_PROJECTION,
};
const char *data_kind2str(data_kind_t kind);

// Gates indices
enum {
    LSTM_I = 0,
    LSTM_F = 1,
    LSTM_C = 2,
    LSTM_O = 3,
    GRU_U = 0,
    GRU_R = 1,
    GRU_O = 2,
    LBR_GRU_U_PRIME = 3,
};

// dlc is different at the cell level and the primitive level
// This enum enable to explicitely query the intended one
enum dlc_type_t { CELL, PRIMITIVE };

template <typename Telem>
struct array_offset_calculator {
    array_offset_calculator() = default;

    template <typename... Targs>
    array_offset_calculator(Telem *base_ptr, Targs... dims)
        : base_ptr_(base_ptr), dims_({dims...}) {}

    // ctor for AOC<const T> based on const AOC<T> &
    template <typename Uelem>
    array_offset_calculator(const array_offset_calculator<Uelem> &rhs)
        : base_ptr_(rhs.base_ptr_), dims_(rhs.dims_) {}

    // to make the above ctor work AOC<const T> should be able to access
    // private fields of AOC<T>, hence let's friend them
    friend struct array_offset_calculator<const Telem>;

    template <typename... Targs>
    Telem &operator()(Targs... Fargs) const {
        return *(base_ptr_ + offset(1, Fargs...));
    }

    int64_t nelems() const {
        int64_t res = 1;
        for (auto dim : dims_)
            res *= dim;
        return res;
    }

    void set_base_ptr(Telem *base_ptr) { base_ptr_ = base_ptr; }

private:
    template <typename... Targs>
    int64_t offset(int64_t d, int64_t pos) const {
        return pos;
    }

    template <typename... Targs>
    int64_t offset(int64_t d, int64_t off, int64_t pos) const {
        return off * dims_[d] + pos;
    }

    template <typename... Targs>
    int64_t offset(int64_t d, int64_t off, int64_t pos, Targs... rem) const {
        return offset(d + 1, off * dims_[d] + pos, rem...);
    }

    Telem *base_ptr_;
    std::vector<int64_t> dims_;
};

struct desc_t {
    int64_t sic;
    int64_t slc;
    int64_t dhc;
    int64_t dic;
    int64_t wc;
    int64_t mb;
    int64_t n_layer;
    int64_t n_iter;
    const char *name;
};
int str2desc(desc_t *desc, const char *str);
std::ostream &operator<<(std::ostream &s, const desc_t &d);

/** configuration structure, that controls initial data filling + error check
*
* dt defines precision
*
* for each lst data kind the values are filled as follows:
* if (rand() > f_sparsity) then:
*     v <-- f_base
* else:
*     v <-- f_min + rand() * f_step % (f_max - f_min)
*
* on final check the resulting values should be in [min .. max] range, the
* relative difference should not exceed eps
*/
struct dt_conf_t {
    struct entry_t {
        dnnl_data_type_t dt;
        int min, max; // representative
        float f_min, f_max; // fill range
        float f_mean, f_stddev; // parameters of normal distribution
        double eps; // acceptable error
    };

    dt_conf_t(const std::string &str) : str_(str) {}

    virtual const entry_t &operator[](data_kind_t kind) const = 0;

    const std::string &str() const { return str_; }
    bool is_int8() const { return operator[](SRC_LAYER).dt == dnnl_u8; }

    static const dt_conf_t &create(const std::string &str);

    std::string str_;
};

struct settings_t {
    settings_t() = default;

    // ctor to save certain fields from resetting
    settings_t(const char *perf_template) : settings_t() {
        this->perf_template = perf_template;
    }

    desc_t desc;

    std::vector<dir_t> prop {FWD_D};
    std::vector<std::string> cfg {"f32"};
    std::vector<alg_t> alg {VANILLA_RNN};
    std::vector<dnnl_rnn_direction_t> direction {
            dnnl_unidirectional_left2right};
    std::vector<activation_t> activation {RELU};
    std::vector<bool> skip_nonlinear {false};
    std::vector<bool> trivial_strides {false};
    std::vector<bool> with_peephole {false};
    std::vector<bool> with_projection {false};
    std::vector<int64_t> mb {0};
    std::vector<policy_t> scale_policy {policy_t::NONE};
    attr_t attr = {};
    bool allow_unimpl = false;
    unsigned int flags = 0x0;
    float alpha = 0.9f, beta = 0.0f;

    const char *perf_template_csv
            = "perf,%engine%,%name%,%prop%,%cfg%,%alg%,%activation%,%direction%"
              ","
              "%DESC%,%Gops%,%Gfreq%,%-time%,%-Gflops%,%0time%,%0Gflops%";
    const char *perf_template_def
            = "perf,%engine%,%name%,%prb%,%Gops%,%Gfreq%,%-time%,%-Gflops%,"
              "%0time%,%0Gflops%";
    const char *perf_template = perf_template_def;

    void reset() { *this = settings_t(perf_template); }
};

struct prb_t : public desc_t {
    prb_t(const desc_t &desc, const dt_conf_t &cfg, dir_t prop, alg_t alg,
            bool with_peephole, bool with_projection,
            dnnl_rnn_direction_t direction, const attr_t &attr,
            policy_t scale_policy, unsigned int flags, activation_t activation,
            float alpha, float beta, bool skip_nonlinear, bool trivial_strides,
            int mb = 0)
        : desc_t(desc)
        , cfg(cfg)
        , prop(prop2prop_kind(prop))
        , alg(alg)
        , with_peephole(with_peephole)
        , with_projection(with_projection)
        , direction(direction)
        , flags(flags)
        , activation(activation)
        , alpha(alpha)
        , beta(beta)
        , attr(attr)
        , scale_policy(scale_policy)
        , ops(0.0)
        , skip_nonlinear(skip_nonlinear)
        , trivial_strides(trivial_strides)
        , linear_cscale(0.0f) {

        if (mb) this->mb = mb;
        count_ops();
        wc = MAX2(MAX2(sic, slc), MAX2(dic, dhc));

        wei_oc_scales = nullptr;
        linear_scales = nullptr;

        // We always allocate linear scales. Even if they are not
        // used, they get dereferenced when built in debug mode.
        linear_scales = (float *)zmalloc(sizeof(float) * n_gates(), 64);
        // Here we use the range of SRC_LAYER to set the scales
        set_tparams(cfg[SRC_LAYER].f_min, cfg[SRC_LAYER].f_max);

        if (scale_policy == policy_t::PER_OC)
            wei_oc_scales
                    = (float *)zmalloc(sizeof(float) * dhc * n_gates(), 64);
        set_qparams(-1., 1.);
    }
    ~prb_t() {
        if (wei_oc_scales) zfree(wei_oc_scales);
        if (linear_scales) zfree(linear_scales);
    }

    bool maybe_skip() const {
        bool skip = false;
        // TODO: remove early exit when int8 weights reorder supports non
        // trivial strides
        skip = skip || (is_int8() && !trivial_strides);

        // TODO: remove early exit when other cells will support int8
        skip = skip || (is_int8() && alg != VANILLA_LSTM);

        return skip;
    }

    void count_ops() {
        // Here, we count only the ops in GEMM portion as there is no
        // theoretical number of ops for the post-gemm operations
        int64_t num_cells = (int64_t)n_dir() * n_layer * n_iter;
        int64_t cell_ops = (int64_t)2 * (n_gates() * dhc) * mb * (sic + slc);
        if (with_projection) cell_ops += (int64_t)2 * dhc * mb * dic;
        int64_t prop_multiplier = prop == dnnl_backward ? 2 : 1;
        ops = prop_multiplier * num_cells * cell_ops;
    }

    int64_t n_dir() const {
        return (direction == dnnl_bidirectional_concat
                       || direction == dnnl_bidirectional_sum)
                ? 2
                : 1;
    }
    int64_t n_states() const { return alg == VANILLA_LSTM ? 2 : 1; }
    int64_t n_gates() const {
        return alg == VANILLA_LSTM
                ? 4
                : (alg == VANILLA_GRU || alg == LBR_GRU ? 3 : 1);
    }
    int64_t n_bias() const {
        return alg == LBR_GRU ? n_gates() + 1 : n_gates();
    }

    int64_t dlc(dlc_type_t type) const {
        if (type == PRIMITIVE)
            return (direction == dnnl_bidirectional_concat ? 2 : 1) * dic;
        if (type == CELL) return dic;
        assert(!"unsupported dlc type");
        return 0;
    }

    bool is_int8() const { return cfg[SRC_LAYER].dt == dnnl_u8; }
    bool is_lstm_peephole() const { return with_peephole; }
    bool is_lstm_projection() const { return with_projection; }

    const dt_conf_t &cfg;
    dnnl_prop_kind_t prop;
    alg_t alg;
    bool with_peephole, with_projection;
    dnnl_rnn_direction_t direction;
    unsigned int flags;
    activation_t activation;
    float alpha;
    float beta;
    attr_t attr;
    policy_t scale_policy;

    double ops;

    float data_scale, data_shift;
    float wei_scale;
    float *wei_oc_scales;

    bool skip_nonlinear;
    bool trivial_strides;
    float *linear_scales;
    float linear_cscale;

private:
    /* Todo: fused the two functions in set_shifts_scales */
    void set_qparams(float fp_min, float fp_max);
    void set_tparams(float fp_min, float fp_max);
    prb_t(const prb_t &) = delete;
    prb_t &operator=(const prb_t &) = delete;
};
std::ostream &operator<<(std::ostream &s, const prb_t &p);

struct perf_report_t : public base_perf_report_t {
    using base_perf_report_t::base_perf_report_t;

    void report(const prb_t *p, const res_t *r, const char *prb_str) {
        p_ = p;
        base_report(r, prb_str);
    }

    virtual void dump_alg(std::ostream &s) const override {
        s << alg2str(p_->alg);
    }

    virtual void dump_cfg(std::ostream &s) const override {
        s << p_->cfg.str();
    }

    virtual void dump_desc(std::ostream &s) const override {
        s << static_cast<const desc_t &>(*p_);
    }

    virtual void dump_desc_csv(std::ostream &s) const override {
        s << p_->n_layer << "," << p_->n_iter << "," << p_->mb << "," << p_->sic
          << "," << p_->slc << "," << p_->dhc << "," << p_->dic;
    }

    virtual void dump_rnn_activation(std::ostream &s) const override {
        s << activation2str(p_->activation);
    }

    virtual void dump_rnn_direction(std::ostream &s) const override {
        s << direction2str(p_->direction);
    }

    virtual double ops() const override { return p_->ops; }
    virtual const char *name() const override { return p_->name; }
    virtual const dnnl_prop_kind_t *prop() const override { return &p_->prop; }

private:
    const prb_t *p_ = nullptr;
};

void prepare_ws_fwd(const prb_t &p, std::vector<float> &ws_fwd_buffer,
        AOC<float> &ws_src_layer, AOC<float> &ws_src_iter,
        AOC<float> &ws_src_iter_c, AOC<float> &ws_gates, AOC<float> &ws_ht);

void rnn_linear_fwd(const prb_t &p, const float *src_layer_,
        const float *src_iter_, const float *src_iter_c_,
        const float *weights_layer_, const float *weights_iter_,
        const float *weights_peephole_, const float *weights_projection_,
        const float *bias_, float *dst_layer_, float *dst_iter_,
        float *dst_iter_c_, const AOC<float> &ws_src_layer,
        const AOC<float> &ws_src_iter, const AOC<float> &ws_src_iter_c,
        const AOC<float> &ws_gates, const AOC<float> &ws_ht);

void compute_ref_fwd(const prb_t &p, dnn_mem_t &src_layer_m,
        dnn_mem_t &src_iter_m, dnn_mem_t &src_iter_c_m,
        dnn_mem_t &weights_layer_m, dnn_mem_t &weights_iter_m,
        dnn_mem_t &weights_peephole_m, dnn_mem_t &weights_projection_m,
        dnn_mem_t &bias_m, dnn_mem_t &dst_layer_m, dnn_mem_t &dst_iter_m,
        dnn_mem_t &dst_iter_c_m);

void compute_ref_bwd(const prb_t &p, dnn_mem_t &src_layer_m,
        dnn_mem_t &src_iter_m, dnn_mem_t &src_iter_c_m,
        dnn_mem_t &diff_dst_layer_m, dnn_mem_t &diff_dst_iter_m,
        dnn_mem_t &diff_dst_iter_c_m, dnn_mem_t &weights_layer_m,
        dnn_mem_t &weights_iter_m, dnn_mem_t &weights_peephole_m,
        dnn_mem_t &weights_projection_m, dnn_mem_t &bias_m,
        dnn_mem_t &dst_layer_m, dnn_mem_t &dst_iter_m, dnn_mem_t &dst_iter_c_m,
        dnn_mem_t &diff_src_layer_m, dnn_mem_t &diff_src_iter_m,
        dnn_mem_t &diff_src_iter_c_m, dnn_mem_t &diff_weights_layer_m,
        dnn_mem_t &diff_weights_iter_m, dnn_mem_t &diff_weights_peephole_m,
        dnn_mem_t &diff_weights_projection_m, dnn_mem_t &diff_bias_m);

void check_case_validity(const dt_conf_t &cfg, policy_t policy);

int doit(const prb_t &p, res_t *res);
int bench(int argc, char **argv);

} // namespace rnn

#endif
