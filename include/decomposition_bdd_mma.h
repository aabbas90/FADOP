#pragma once

#include <memory>
#include "bdd_mma.h"

namespace LPMP {

    struct decomposition_mma_options {
        size_t nr_threads; 
        bool force_thread_nr = false; // otherwise a smaller number of threads might be used
        constexpr static size_t min_nr_bdd_nodes = 10000; // TODO: measure good value
        double parallel_message_passing_weight = 0.5;
    };

    class decomposition_bdd_mma {
        public:

            decomposition_bdd_mma(bdd_storage& bdd_storage_, decomposition_mma_options opt);
            template<typename ITERATOR>
                decomposition_bdd_mma(bdd_storage& stor, ITERATOR cost_begin, ITERATOR cost_end, decomposition_mma_options opt);
            decomposition_bdd_mma(decomposition_bdd_mma&&);

            decomposition_bdd_mma& operator=(decomposition_bdd_mma&&);

            ~decomposition_bdd_mma();
            void set_cost(const double c, const size_t var);
            void backward_run();
            void iteration();
            double lower_bound();
            two_dim_variable_array<std::array<double,2>> min_marginals();

        private:
            struct impl;
            std::unique_ptr<impl> pimpl;

    };

    template<typename ITERATOR>
        decomposition_bdd_mma::decomposition_bdd_mma(bdd_storage& stor, ITERATOR cost_begin, ITERATOR cost_end, decomposition_mma_options opt)
        : decomposition_bdd_mma(stor, opt)
        {
            assert(std::distance(cost_begin, cost_end) <= stor.nr_variables());
            size_t var = 0;
            for(auto cost_it=cost_begin; cost_it!=cost_end; ++cost_it)
                set_cost(*cost_it, var++);
            backward_run();
        }

}
