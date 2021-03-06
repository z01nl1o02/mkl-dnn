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

#ifndef GEMM_X8S8S32X_INNER_PRODUCT_HPP
#define GEMM_X8S8S32X_INNER_PRODUCT_HPP

#include <assert.h>

#include "c_types_map.hpp"
#include "memory_tracking.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

#include "gemm/gemm.hpp"
#include "jit_generator.hpp"

#include "cpu_inner_product_pd.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <impl::data_type_t src_type, impl::data_type_t dst_type>
struct gemm_x8s8s32x_inner_product_fwd_t: public cpu_primitive_t {
    struct pd_t: public cpu_inner_product_fwd_pd_t {
        pd_t(engine_t *engine, const inner_product_desc_t *adesc,
                const primitive_attr_t *attr,
                const inner_product_fwd_pd_t *hint_fwd_pd)
            : cpu_inner_product_fwd_pd_t(engine, adesc, attr, hint_fwd_pd) {}

        DECLARE_COMMON_PD_T(src_type == data_type::u8
                ? IGEMM_S8U8S32_IMPL_STR
                : IGEMM_S8S8S32_IMPL_STR,
                gemm_x8s8s32x_inner_product_fwd_t);

        virtual status_t init() override {
            using namespace utils;
            using namespace data_type;

            assert(engine()->kind() == engine_kind::cpu);

            bool ok = true
                && this->set_default_params() == status::success
                && one_of(desc()->prop_kind, prop_kind::forward_training,
                        prop_kind::forward_inference)
                && !has_zero_dim_memory()
                && this->desc()->src_desc.data_type == src_type
                && this->desc()->dst_desc.data_type == dst_type
                && this->desc()->weights_desc.data_type == s8
                && IMPLICATION(this->with_bias(), utils::one_of(
                            this->desc()->bias_desc.data_type, f32, s32, s8,
                            u8))
                && attr()->post_ops_.len_ <= 1
                && IMPLICATION(attr()->post_ops_.len_,
                        attr()->post_ops_.entry_[0].is_relu(true, false))
                && dense_gemm_consitency_check(src_pd(), weights_pd(),
                        dst_pd());
            if (!ok) return status::unimplemented;

            dst_is_acc_ = one_of(dst_type, s32, f32);

            init_scratchpad();

            return status::success;
        }

        bool dst_is_acc_;

    protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;

            if (this->src_pd_.desc()->format == any) {
                switch (ndims()) {
                case 0: // XXX: temp fix
                case 2: CHECK(this->src_pd_.set_format(nc)); break;
                case 3: CHECK(this->src_pd_.set_format(nwc)); break;
                case 4: CHECK(this->src_pd_.set_format(nhwc)); break;
                case 5: CHECK(this->src_pd_.set_format(ndhwc)); break;
                default: assert(!"unsupported ndims format");
                }
            }
            if (this->dst_pd_.desc()->format == any)
                CHECK(this->dst_pd_.set_format(nc));
            if (this->weights_pd_.desc()->format == any) {
                switch (ndims()) {
                case 0: // XXX: temp fix
                case 2: CHECK(this->weights_pd_.set_format(io)); break;
                case 3: CHECK(this->weights_pd_.set_format(wio)); break;
                case 4: CHECK(this->weights_pd_.set_format(hwio)); break;
                case 5: CHECK(this->weights_pd_.set_format(dhwio)); break;
                default: assert(!"unsupported ndims format");
                }
            }
            if (this->bias_pd_.desc()->format == any)
                CHECK(this->bias_pd_.set_format(x));

            return status::success;
        }

    private:
        void init_scratchpad() {
            if (!dst_is_acc_) {
                auto scratchpad = scratchpad_registry().registrar();
                scratchpad.book(
                        memory_tracking::names::key_iprod_int_dat_in_acc_dt,
                        sizeof(acc_data_t) * MB() * OC());
            }
        }
    };

    gemm_x8s8s32x_inner_product_fwd_t(const pd_t *apd, const input_vector &inputs,
            const output_vector &outputs)
        : cpu_primitive_t(apd, inputs, outputs, true)
    { pp_kernel_ = new pp_kernel_t(apd, pd()->dst_is_acc_); }
    ~gemm_x8s8s32x_inner_product_fwd_t() { delete pp_kernel_; }

    typedef typename prec_traits<dst_type>::type data_t;

    typedef typename prec_traits<src_type>::type src_data_t;
    typedef typename prec_traits<data_type::s8>::type wei_data_t;
    typedef typename prec_traits<dst_type>::type dst_data_t;
    typedef typename prec_traits<data_type::s32>::type acc_data_t;

    virtual void execute(event_t *e) const {
        execute_forward();
        e->set_state(event_t::ready);
    }

private:
    // XXX: this is throwaway code that will become unnecessary when we have a
    // sufficiently advanced igemm jit generator that supports quantization,
    // relu, and whatnot
    class pp_kernel_t: jit_generator {
    public:
        DECLARE_CPU_JIT_AUX_FUNCTIONS(
                gemm_x8s8s32x_inner_product_fwd_t::pp_kernel);
        pp_kernel_t(const pd_t *pd, bool dst_is_acc);

        void operator()(dst_data_t *dst, const acc_data_t *acc,
                const char *bias, const float *scales, float nslope,
                size_t start, size_t end);
    private:
        void generate();

        struct ker_args {
            dst_data_t *dst;
            const acc_data_t *acc;
            const char *bias;
            const float *scales;
            float nslope;
            size_t len;
            size_t oc_offset;
        };
        void (*ker_)(const ker_args *args);

        size_t OC_;
        data_type_t bias_data_type_;
        size_t bias_data_type_size_;
        size_t scale_idx_mult_;
        round_mode_t rmode_;
        bool do_bias_;
        bool do_relu_;
    };

    void execute_forward() const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd(); }

    pp_kernel_t *pp_kernel_;
};
}
}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
