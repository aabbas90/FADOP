import torch
from BDD.bdd_cuda_learned_mma_py import bdd_cuda_learned_mma
from torch.autograd.function import once_differentiable

def valid_input_format(*args):
    for arg in args:
        assert arg.dtype == torch.float32, 'argument not in FP32 format'
        assert arg.is_contiguous(), 'argument not contiguous'

def ComputePerBDDSolutions(solvers, lo_costs_batch, hi_costs_batch):
    valid_input_format(lo_costs_batch, hi_costs_batch)
    per_bdd_solution_hi = torch.zeros_like(lo_costs_batch) # Initialize by 0's to also copy to deferred min-marginals.
    # per_bdd_solution_lo = torch.empty_like(lo_costs_batch)
    layer_start = 0
    for (b, solver) in enumerate(solvers):
        solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), per_bdd_solution_hi[layer_start].data_ptr())
        solver.solution_per_bdd(per_bdd_solution_hi[layer_start].data_ptr())
        # current_sol_lo = 1.0 - per_bdd_solution_hi[layer_start: layer_start + solver.nr_layers()]
        # terminal_indices = torch.empty((2 * solver.nr_bdds()), device = hi_costs_batch.device, dtype = torch.int32)
        # solver.terminal_nodes_indices(terminal_indices.data_ptr())
        # current_sol_lo[terminal_indices] = 0.0 # Terminal nodes.
        # per_bdd_solution_lo[layer_start: layer_start + solver.nr_layers()] = current_sol_lo
        layer_start += solver.nr_layers()
    
    return per_bdd_solution_hi
    #return per_bdd_solution_lo, per_bdd_solution_hi

class DualIterations(torch.autograd.Function):
    @staticmethod
    def forward(ctx, solvers, lo_costs_batch, hi_costs_batch, def_mm_batch, dist_weights_batch, num_iterations, omega, grad_dual_itr_max_itr, improvement_slope):
        valid_input_format(lo_costs_batch, hi_costs_batch, def_mm_batch, dist_weights_batch)
        assert(lo_costs_batch.dim() == 1)
        assert(def_mm_batch.dim() == 1)
        assert(dist_weights_batch.dim() == 1)
        assert(lo_costs_batch.shape == hi_costs_batch.shape)
        assert(lo_costs_batch.shape == dist_weights_batch.shape)
        assert(lo_costs_batch.shape == def_mm_batch.shape)
        assert(torch.numel(omega) == 1 or omega.shape == hi_costs_batch.shape)
        ctx.set_materialize_grads(False)
        ctx.save_for_backward(lo_costs_batch, hi_costs_batch, def_mm_batch, dist_weights_batch, omega)
        ctx.solvers = solvers
        ctx.num_iterations = num_iterations
        ctx.grad_dual_itr_max_itr = grad_dual_itr_max_itr
        lo_costs_out = torch.empty_like(lo_costs_batch)
        hi_costs_out = torch.empty_like(hi_costs_batch)
        def_mm_out = torch.empty_like(def_mm_batch)

        is_omega_scalar = torch.numel(omega) == 1
        layer_start = 0
        actual_num_itr = []
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), def_mm_batch[layer_start].data_ptr())
            if is_omega_scalar:
                num_itr = solver.iterations(dist_weights_batch[layer_start].data_ptr(), num_iterations, omega[0].item(), improvement_slope, 0, False)
            else:
                num_itr = solver.iterations(dist_weights_batch[layer_start].data_ptr(), num_iterations, 1.0, improvement_slope, omega[layer_start].data_ptr(), True)

            actual_num_itr.append(num_itr)
            solver.get_solver_costs(lo_costs_out[layer_start].data_ptr(), hi_costs_out[layer_start].data_ptr(), def_mm_out[layer_start].data_ptr()) 
            layer_start += solver.nr_layers()

        ctx.actual_num_itr = actual_num_itr
        assert(layer_start == lo_costs_batch.shape[0])
        return lo_costs_out, hi_costs_out, def_mm_out

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_lo_costs_out, grad_hi_costs_out, grad_def_mm_out):
        valid_input_format(grad_lo_costs_out, grad_hi_costs_out, grad_def_mm_out)
        assert(grad_lo_costs_out.dim() == 1)
        assert(grad_lo_costs_out.shape == grad_hi_costs_out.shape)
        assert(grad_lo_costs_out.shape == grad_def_mm_out.shape)

        grad_lo_costs_in = grad_lo_costs_out.detach().clone()
        grad_hi_costs_in = grad_hi_costs_out.detach().clone()
        grad_deff_mm_diff_in = grad_def_mm_out.detach().clone()
        solvers = ctx.solvers

        lo_costs_batch, hi_costs_batch, def_mm_batch, dist_weights_batch, omega = ctx.saved_tensors
        grad_dist_weights_batch_in = torch.empty_like(dist_weights_batch)
        grad_omega = torch.empty_like(omega)

        is_omega_scalar = torch.numel(omega) == 1
        layer_start = 0
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), def_mm_batch[layer_start].data_ptr())
            num_iterations_b = ctx.actual_num_itr[b]
            track_grad_for_num_itr = min(num_iterations_b, ctx.grad_dual_itr_max_itr)
            track_grad_after_itr = num_iterations_b - track_grad_for_num_itr
            try:
                if is_omega_scalar:
                    solver.grad_iterations(dist_weights_batch[layer_start].data_ptr(), grad_lo_costs_in[layer_start].data_ptr(), grad_hi_costs_in[layer_start].data_ptr(),
                                            grad_deff_mm_diff_in[layer_start].data_ptr(), grad_dist_weights_batch_in[layer_start].data_ptr(), grad_omega[0].data_ptr(),
                                            omega[0].item(), track_grad_after_itr, track_grad_for_num_itr, 0, False)
                else:
                    solver.grad_iterations(dist_weights_batch[layer_start].data_ptr(), grad_lo_costs_in[layer_start].data_ptr(), grad_hi_costs_in[layer_start].data_ptr(),
                                            grad_deff_mm_diff_in[layer_start].data_ptr(), grad_dist_weights_batch_in[layer_start].data_ptr(), grad_omega[layer_start].data_ptr(),
                                            1.0, track_grad_after_itr, track_grad_for_num_itr, omega[layer_start].data_ptr(), True)

            except:
                print(f'Error in grad_iterations.')
            layer_start += solver.nr_layers()
        return None, grad_lo_costs_in, grad_hi_costs_in, grad_deff_mm_diff_in, grad_dist_weights_batch_in, None, grad_omega, None, None

class DistributeDeferredDelta(torch.autograd.Function):
    @staticmethod
    def forward(ctx, solvers, lo_costs_batch, hi_costs_batch, def_mm_batch):
        valid_input_format(lo_costs_batch, hi_costs_batch, def_mm_batch)
        assert(lo_costs_batch.dim() == 1)
        assert(def_mm_batch.dim() == 1)
        assert(lo_costs_batch.shape == hi_costs_batch.shape)
        assert(lo_costs_batch.shape == def_mm_batch.shape)

        ctx.set_materialize_grads(False)
        ctx.solvers = solvers
        ctx.save_for_backward(lo_costs_batch, hi_costs_batch, def_mm_batch)

        lo_costs_out = torch.empty_like(lo_costs_batch)
        hi_costs_out = torch.empty_like(hi_costs_batch)
        def_mm_batch_out = torch.empty_like(def_mm_batch)

        layer_start = 0
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), def_mm_batch[layer_start].data_ptr())
            solver.distribute_delta() # After this def_mm_batch_out will be zero.
            solver.get_solver_costs(lo_costs_out[layer_start].data_ptr(), hi_costs_out[layer_start].data_ptr(), def_mm_batch_out[layer_start].data_ptr()) 
            layer_start += solver.nr_layers()
        assert(layer_start == lo_costs_batch.shape[0])
        return lo_costs_out, hi_costs_out

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_lo_costs_out, grad_hi_costs_out):
        valid_input_format(grad_lo_costs_out, grad_hi_costs_out)
        assert(grad_lo_costs_out.dim() == 1)
        assert(grad_lo_costs_out.shape == grad_hi_costs_out.shape)

        lo_costs_batch, hi_costs_batch, def_mm_batch = ctx.saved_tensors
        grad_deff_mm_diff_in = torch.empty_like(grad_lo_costs_out)
        solvers = ctx.solvers
        layer_start = 0
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), def_mm_batch[layer_start].data_ptr())
            try:
                solver.grad_distribute_delta(grad_lo_costs_out[layer_start].data_ptr(), grad_hi_costs_out[layer_start].data_ptr(), grad_deff_mm_diff_in[layer_start].data_ptr())
            except:
                print(f'Error in grad_distribute_delta.')

            layer_start += solver.nr_layers()
        # Jacobian of lo_costs w.r.t lo_costs is identity so just pass the received gradients of lo_costs backward. (same for hi_costs.)
        return None, grad_lo_costs_out, grad_hi_costs_out, grad_deff_mm_diff_in

class ComputeAllMinMarginalsDiff(torch.autograd.Function):
    # Make sure deferred min-marginals are zero.
    @staticmethod
    def forward(ctx, solvers, lo_costs_batch, hi_costs_batch):
        valid_input_format(lo_costs_batch, hi_costs_batch)
        assert(lo_costs_batch.dim() == 1)
        assert(lo_costs_batch.shape == hi_costs_batch.shape)

        ctx.set_materialize_grads(False)
        ctx.solvers = solvers
        ctx.save_for_backward(lo_costs_batch, hi_costs_batch)
        mm_diff_batch = torch.zeros_like(lo_costs_batch) # Deferred min-marginals should be zero by this point.
        layer_start = 0
        for (b, solver) in enumerate(solvers):  
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), mm_diff_batch[layer_start].data_ptr())
            solver.all_min_marginal_differences(mm_diff_batch[layer_start].data_ptr())
            layer_start += solver.nr_layers()
        assert(layer_start == lo_costs_batch.shape[0])
        return mm_diff_batch

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_mm_diff_batch):
        valid_input_format(grad_mm_diff_batch)
        assert(grad_mm_diff_batch.dim() == 1)

        lo_costs_batch, hi_costs_batch = ctx.saved_tensors
        grad_lo_costs_in = torch.empty_like(lo_costs_batch)
        grad_hi_costs_in = torch.empty_like(hi_costs_batch)
        def_mm_batch = torch.zeros_like(grad_mm_diff_batch) # At this point deferred min-marginals were zero.
        solvers = ctx.solvers
        layer_start = 0
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), def_mm_batch[layer_start].data_ptr())
            try:
                solver.grad_all_min_marginal_differences(grad_mm_diff_batch[layer_start].data_ptr(), grad_lo_costs_in[layer_start].data_ptr(), grad_hi_costs_in[layer_start].data_ptr())
            except:
                print(f'Error in grad_all_min_marginal_differences.')

            layer_start += solver.nr_layers()
        # Jacobian of lo_costs w.r.t lo_costs is identity so just pass the received gradients of lo_costs backward. (same for hi_costs.)
        return None, grad_lo_costs_in, grad_hi_costs_in

class PerturbPrimalCosts(torch.autograd.Function):
    @staticmethod
    # Make sure deferred min-marginals are zero.
    def forward(ctx, solvers, lo_costs_pert_batch, hi_costs_pert_batch, lo_costs_batch, hi_costs_batch):
        valid_input_format(lo_costs_pert_batch, hi_costs_pert_batch, lo_costs_batch, hi_costs_batch)
        assert(lo_costs_batch.dim() == 1)
        assert(lo_costs_batch.shape == hi_costs_batch.shape)
        assert(lo_costs_pert_batch.dim() == 1)
        assert(lo_costs_pert_batch.shape == hi_costs_pert_batch.shape)

        ctx.set_materialize_grads(False)
        ctx.solvers = solvers
        ctx.save_for_backward(lo_costs_pert_batch, hi_costs_pert_batch, lo_costs_batch, hi_costs_batch)
        mm_diff_batch = torch.zeros_like(lo_costs_batch) # Deferred MMD should be zero at this point.
        lo_costs_out = torch.empty_like(lo_costs_batch)
        hi_costs_out = torch.empty_like(hi_costs_batch)

        var_start = 0
        layer_start = 0
        for (b, solver) in enumerate(solvers):  
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), mm_diff_batch[layer_start].data_ptr())
            solver.perturb_costs(lo_costs_pert_batch[var_start].data_ptr(), hi_costs_pert_batch[var_start].data_ptr())
            solver.get_solver_costs(lo_costs_out[layer_start].data_ptr(), hi_costs_out[layer_start].data_ptr(), mm_diff_batch[layer_start].data_ptr()) 
            layer_start += solver.nr_layers()
            var_start += solver.nr_primal_variables() + 1
        assert(var_start == lo_costs_pert_batch.shape[0])
        assert(layer_start == lo_costs_batch.shape[0])
        return lo_costs_out, hi_costs_out

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_lo_costs_out, grad_hi_costs_out):
        valid_input_format(grad_lo_costs_out, grad_hi_costs_out)
        assert(grad_lo_costs_out.dim() == 1)
        assert(grad_lo_costs_out.shape == grad_hi_costs_out.shape)

        lo_costs_pert_batch, hi_costs_pert_batch, lo_costs_batch, hi_costs_batch = ctx.saved_tensors

        grad_lo_costs_pert_in = torch.empty_like(lo_costs_pert_batch)
        grad_hi_costs_pert_in = torch.empty_like(hi_costs_pert_batch)
        def_mm_batch = torch.zeros_like(grad_lo_costs_out) # At this point deferred min-marginals were zero.
        solvers = ctx.solvers
        var_start = 0
        layer_start = 0
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), def_mm_batch[layer_start].data_ptr())
            try:
                solver.grad_cost_perturbation(grad_lo_costs_out[layer_start].data_ptr(), grad_hi_costs_out[layer_start].data_ptr(), 
                                            grad_lo_costs_pert_in[var_start].data_ptr(), grad_hi_costs_pert_in[var_start].data_ptr())
            except:
                print(f'Error in grad_cost_perturbation.')

            var_start += solver.nr_primal_variables() + 1
            layer_start += solver.nr_layers()
        assert(var_start == lo_costs_pert_batch.shape[0])
        assert(layer_start == lo_costs_batch.shape[0])
        return None, grad_lo_costs_pert_in, grad_hi_costs_pert_in

class ComputeLowerBoundperBDD(torch.autograd.Function):
    @staticmethod
    def forward(ctx, solvers, lo_costs_batch, hi_costs_batch):
        valid_input_format(lo_costs_batch, hi_costs_batch)
        assert(lo_costs_batch.dim() == 1)
        assert(lo_costs_batch.shape == hi_costs_batch.shape)

        ctx.set_materialize_grads(False)
        ctx.solvers = solvers
        ctx.save_for_backward(lo_costs_batch, hi_costs_batch)
        mm_diff_batch = torch.zeros_like(lo_costs_batch)
        lb_per_bdd_batch = []
        layer_start = 0
        for (b, solver) in enumerate(solvers):  
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), mm_diff_batch[layer_start].data_ptr())
            lb_per_bdd = torch.empty((solver.nr_bdds()), device = lo_costs_batch.device, dtype = torch.float32)
            solver.lower_bound_per_bdd(lb_per_bdd.data_ptr())
            lb_per_bdd_batch.append(lb_per_bdd)
            layer_start += solver.nr_layers()
        assert(layer_start == lo_costs_batch.shape[0])
        lb_per_bdd_batch = torch.cat(lb_per_bdd_batch)
        return lb_per_bdd_batch

    @staticmethod
    @once_differentiable
    def backward(ctx, grad_lb_per_bdd_batch):
        # grad_lb_per_bdd_batch = grad_lb_per_bdd_batch.contiguous()
        valid_input_format(grad_lb_per_bdd_batch)
        assert(grad_lb_per_bdd_batch.dim() == 1)

        lo_costs_batch, hi_costs_batch = ctx.saved_tensors
        solvers = ctx.solvers
        grad_lo_costs_in = torch.zeros_like(lo_costs_batch)
        grad_hi_costs_in = torch.empty_like(hi_costs_batch)
        layer_start = 0
        bdd_start = 0
        for (b, solver) in enumerate(solvers):
            solver.set_solver_costs(lo_costs_batch[layer_start].data_ptr(), hi_costs_batch[layer_start].data_ptr(), grad_lo_costs_in[layer_start].data_ptr())
            try:
                solver.grad_lower_bound_per_bdd(grad_lb_per_bdd_batch[bdd_start].data_ptr(), grad_lo_costs_in[layer_start].data_ptr(), grad_hi_costs_in[layer_start].data_ptr())
            except:
                print(f'Error in grad_lower_bound_per_bdd.')

            bdd_start += solver.nr_bdds()
            layer_start += solver.nr_layers()
        
        return None, grad_lo_costs_in, grad_hi_costs_in