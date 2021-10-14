#include "bdd_cuda.h"
#ifdef WITH_CUDA
#include "bdd_cuda_parallel_mma.h"
#endif
#include "time_measure_util.h"

namespace LPMP {

    class bdd_cuda::impl {
        public:
            impl(BDD::bdd_collection& bdd_col);

#ifdef WITH_CUDA
            bdd_cuda_parallel_mma pmma;
#endif
    };

    bdd_cuda::impl::impl(BDD::bdd_collection& bdd_col)
#ifdef WITH_CUDA
    : pmma(bdd_col)
#endif
    {
#ifndef WITH_CUDA
        throw std::runtime_error("bdd_solver not compiled with CUDA support");
#endif
    }

    bdd_cuda::bdd_cuda(BDD::bdd_collection& bdd_col)
    {
#ifdef WITH_CUDA
        MEASURE_FUNCTION_EXECUTION_TIME; 
        pimpl = std::make_unique<impl>(bdd_col);
#else
        throw std::runtime_error("bdd_solver not compiled with CUDA support");
#endif
    }

    bdd_cuda::bdd_cuda(bdd_cuda&& o)
        : pimpl(std::move(o.pimpl))
    {}

    bdd_cuda& bdd_cuda::operator=(bdd_cuda&& o)
    { 
        pimpl = std::move(o.pimpl);
        return *this;
    }

    bdd_cuda::~bdd_cuda()
    {}

    //void bdd_cuda::set_cost(const double c, const size_t var)
    //{
#ifdef WITH_CUDA
    //    pimpl->pmma.set_cost(c, var);
#endif
    //}

    template<typename COST_ITERATOR>
    void bdd_cuda::update_costs(COST_ITERATOR costs_lo_begin, COST_ITERATOR costs_lo_end, COST_ITERATOR costs_hi_begin, COST_ITERATOR costs_hi_end)
    {
#ifdef WITH_CUDA
        pimpl->pmma.update_costs(costs_lo_begin, costs_lo_end, costs_hi_begin, costs_hi_end);
#endif
    }

    // Need to have explicit instantiation in the base.
    template void bdd_cuda::update_costs(double*, double*, double*, double*);
    template void bdd_cuda::update_costs(float*, float*, float*, float*);
    template void bdd_cuda::update_costs(std::vector<double>::iterator, std::vector<double>::iterator, std::vector<double>::iterator, std::vector<double>::iterator);
    template void bdd_cuda::update_costs(std::vector<double>::const_iterator, std::vector<double>::const_iterator, std::vector<double>::const_iterator, std::vector<double>::const_iterator);
    template void bdd_cuda::update_costs(std::vector<float>::iterator, std::vector<float>::iterator, std::vector<float>::iterator, std::vector<float>::iterator);
    template void bdd_cuda::update_costs(std::vector<float>::const_iterator, std::vector<float>::const_iterator, std::vector<float>::const_iterator, std::vector<float>::const_iterator);

    void bdd_cuda::backward_run()
    {
#ifdef WITH_CUDA
        pimpl->pmma.backward_run();
#endif
    }

    void bdd_cuda::iteration()
    {
#ifdef WITH_CUDA
        pimpl->pmma.iteration();
#endif
    }

    double bdd_cuda::lower_bound()
    {
#ifdef WITH_CUDA
        return pimpl->pmma.lower_bound();
#endif
        return -std::numeric_limits<double>::infinity();
    } 

    two_dim_variable_array<std::array<double,2>> bdd_cuda::min_marginals()
    {
#ifdef WITH_CUDA
        return pimpl->pmma.min_marginals();
#endif
    }

}
