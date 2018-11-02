/*
 * dg_navier_stokes_pressure_correction.h
 *
 *  Created on: Oct 26, 2016
 *      Author: fehn
 */

#ifndef INCLUDE_INCOMPRESSIBLE_NAVIER_STOKES_SPATIAL_DISCRETIZATION_DG_NAVIER_STOKES_PRESSURE_CORRECTION_H_
#define INCLUDE_INCOMPRESSIBLE_NAVIER_STOKES_SPATIAL_DISCRETIZATION_DG_NAVIER_STOKES_PRESSURE_CORRECTION_H_

#include "dg_navier_stokes_projection_methods.h"
#include "velocity_convection_diffusion_operator.h"

#include "../preconditioners/multigrid_preconditioner_navier_stokes.h"

#include "../../solvers_and_preconditioners/newton/newton_solver.h"
#include "../../solvers_and_preconditioners/preconditioner/inverse_mass_matrix_preconditioner.h"
#include "../../solvers_and_preconditioners/preconditioner/jacobi_preconditioner.h"

namespace IncNS
{
template<int dim, int degree_u, int degree_p = degree_u - 1, typename Number = double>
class DGNavierStokesPressureCorrection
  : public DGNavierStokesProjectionMethods<dim, degree_u, degree_p, Number>
{
public:
  typedef DGNavierStokesBase<dim, degree_u, degree_p, Number> BASE;

  typedef DGNavierStokesProjectionMethods<dim, degree_u, degree_p, Number> PROJECTION_METHODS_BASE;

  typedef typename PROJECTION_METHODS_BASE::VectorType VectorType;

  typedef typename PROJECTION_METHODS_BASE::Postprocessor Postprocessor;

  typedef DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number> THIS;

  DGNavierStokesPressureCorrection(parallel::distributed::Triangulation<dim> const & triangulation,
                                   InputParameters<dim> const &                      parameters_in,
                                   std::shared_ptr<Postprocessor> postprocessor_in)
    : PROJECTION_METHODS_BASE(triangulation, parameters_in, postprocessor_in),
      rhs_vector(nullptr),
      evaluation_time(0.0),
      scaling_factor_time_derivative_term(1.0)
  {
  }

  virtual ~DGNavierStokesPressureCorrection()
  {
  }

  void
  setup_solvers(double const & time_step_size, double const & scaling_factor_time_derivative_term);

  // momentum step: linear system of equations (Stokes equations or convective term treated
  // explicitly)
  void
  solve_linear_momentum_equation(VectorType &       solution,
                                 VectorType const & rhs,
                                 double const &     scaling_factor_mass_matrix_term,
                                 unsigned int &     linear_iterations);

  // momentum step: nonlinear system of equations (convective term treated implicitly)
  void
  solve_nonlinear_momentum_equation(VectorType &       dst,
                                    VectorType const & rhs_vector,
                                    double const &     eval_time,
                                    double const &     scaling_factor_mass_matrix_term,
                                    unsigned int &     newton_iterations,
                                    unsigned int &     linear_iterations);

  // apply velocity convection-diffusion operator
  void
  apply_velocity_conv_diff_operator(VectorType &       dst,
                                    VectorType const & src,
                                    VectorType const & solution_linearization);


  /*
   * The implementation of the Newton solver requires that the underlying operator
   * implements a function called "evaluate_nonlinear_residual"
   */
  void
  evaluate_nonlinear_residual(VectorType & dst, VectorType const & src);

  /*
   * The implementation of the Newton solver requires that the underlying operator
   * implements a function called "initialize_vector_for_newton_solver"
   */
  void
  initialize_vector_for_newton_solver(VectorType & src) const
  {
    this->initialize_vector_velocity(src);
  }

  // rhs pressure gradient
  void
  rhs_pressure_gradient_term(VectorType & dst, double const evaluation_time) const;

  // body forces
  void
  evaluate_add_body_force_term(VectorType & dst, double const evaluation_time) const;


  // apply inverse pressure mass matrix
  void
  apply_inverse_pressure_mass_matrix(VectorType & dst, VectorType const & src) const;

  void
  do_postprocessing(VectorType const & velocity,
                    VectorType const & pressure,
                    double const       time,
                    unsigned int const time_step_number);

  void
  do_postprocessing_steady_problem(VectorType const & velocity, VectorType const & pressure);

private:
  // setup of solvers
  void
  setup_momentum_solver(double const & scaling_factor_time_derivative_term);

  void
  setup_inverse_mass_matrix_operator_pressure();

  // momentum equation
  VelocityConvDiffOperator<dim, degree_u, Number> velocity_conv_diff_operator;

  // required for multigrid (if multigrid is applied to HelmholtzOperator only)
  VelocityConvDiffOperatorData<dim> multigrid_operator_data;

  std::shared_ptr<PreconditionerBase<Number>>      momentum_preconditioner;
  std::shared_ptr<IterativeSolverBase<VectorType>> momentum_linear_solver;

  std::shared_ptr<NewtonSolver<VectorType,
                               THIS,
                               VelocityConvDiffOperator<dim, degree_u, Number>,
                               IterativeSolverBase<VectorType>>>
    momentum_newton_solver;

  InverseMassMatrixOperator<dim, degree_p, Number, 1> inverse_mass_matrix_operator_pressure;

  VectorType         temp_vector;
  VectorType const * rhs_vector;

  double evaluation_time;
  double scaling_factor_time_derivative_term;
};

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::setup_solvers(
  double const & time_step_size,
  double const & scaling_factor_time_derivative_term)
{
  ConditionalOStream pcout(std::cout, Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0);
  pcout << std::endl << "Setup solvers ..." << std::endl;

  setup_momentum_solver(scaling_factor_time_derivative_term);

  this->setup_pressure_poisson_solver(time_step_size);

  this->setup_projection_solver();

  setup_inverse_mass_matrix_operator_pressure();

  pcout << std::endl << "... done!" << std::endl;
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::setup_momentum_solver(
  double const & scaling_factor_time_derivative_term)
{
  // setup velocity convection-diffusion operator
  VelocityConvDiffOperatorData<dim> vel_conv_diff_operator_data;

  // unsteady problem
  vel_conv_diff_operator_data.unsteady_problem = true;

  vel_conv_diff_operator_data.scaling_factor_time_derivative_term =
    scaling_factor_time_derivative_term;

  // convective problem
  if(this->param.equation_type == EquationType::NavierStokes &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Implicit)
  {
    vel_conv_diff_operator_data.convective_problem = true;
  }
  else
  {
    vel_conv_diff_operator_data.convective_problem = false;
  }

  vel_conv_diff_operator_data.dof_index      = this->get_dof_index_velocity();
  vel_conv_diff_operator_data.quad_index_std = this->get_quad_index_velocity_linear();

  vel_conv_diff_operator_data.use_cell_based_loops = this->param.use_cell_based_face_loops;
  vel_conv_diff_operator_data.implement_block_diagonal_preconditioner_matrix_free =
    this->param.implement_block_diagonal_preconditioner_matrix_free;

  vel_conv_diff_operator_data.mass_matrix_operator_data = this->mass_matrix_operator_data;
  vel_conv_diff_operator_data.viscous_operator_data     = this->viscous_operator_data;
  vel_conv_diff_operator_data.convective_operator_data  = this->convective_operator_data;

  velocity_conv_diff_operator.initialize(this->get_data(),
                                         vel_conv_diff_operator_data,
                                         this->mass_matrix_operator,
                                         this->viscous_operator,
                                         this->convective_operator);


  // setup preconditioner for momentum equation
  if(this->param.preconditioner_momentum == MomentumPreconditioner::InverseMassMatrix)
  {
    momentum_preconditioner.reset(new InverseMassMatrixPreconditioner<dim, degree_u, Number>(
      this->data, this->get_dof_index_velocity(), this->get_quad_index_velocity_linear()));
  }
  else if(this->param.preconditioner_momentum == MomentumPreconditioner::PointJacobi)
  {
    momentum_preconditioner.reset(
      new JacobiPreconditioner<VelocityConvDiffOperator<dim, degree_u, Number>>(
        velocity_conv_diff_operator));
  }
  else if(this->param.preconditioner_momentum == MomentumPreconditioner::BlockJacobi)
  {
    momentum_preconditioner.reset(
      new BlockJacobiPreconditioner<VelocityConvDiffOperator<dim, degree_u, Number>>(
        velocity_conv_diff_operator));
  }
  else if(this->param.preconditioner_momentum == MomentumPreconditioner::VelocityDiffusion)
  {
    typedef float MultigridNumber;

    typedef MyMultigridPreconditionerVelocityDiffusion<
      dim,
      Number,
      VelocityConvDiffOperator<dim, degree_u, MultigridNumber>,
      VelocityConvDiffOperator<dim, degree_u, Number>>
      MULTIGRID;

    momentum_preconditioner.reset(new MULTIGRID());

    std::shared_ptr<MULTIGRID> mg_preconditioner =
      std::dynamic_pointer_cast<MULTIGRID>(momentum_preconditioner);

    multigrid_operator_data = velocity_conv_diff_operator.get_operator_data();
    // multgrid is only applied to reaction-diffusion operator so the convective term has to be
    // deactivated
    multigrid_operator_data.convective_problem = false;

    mg_preconditioner->initialize(
      this->param.multigrid_data_momentum,
      this->dof_handler_u,
      this->mapping,
      /*velocity_conv_diff_operator.get_operator_data().bc->dirichlet_bc,*/
      (void *)&multigrid_operator_data);
  }
  else if(this->param.preconditioner_momentum ==
          MomentumPreconditioner::VelocityConvectionDiffusion)
  {
    typedef float MultigridNumber;

    typedef MyMultigridPreconditionerVelocityConvectionDiffusion<
      dim,
      Number,
      VelocityConvDiffOperator<dim, degree_u, MultigridNumber>,
      VelocityConvDiffOperator<dim, degree_u, Number>>
      MULTIGRID;

    momentum_preconditioner.reset(new MULTIGRID());

    std::shared_ptr<MULTIGRID> mg_preconditioner =
      std::dynamic_pointer_cast<MULTIGRID>(momentum_preconditioner);

    mg_preconditioner->initialize(
      this->param.multigrid_data_momentum,
      this->get_dof_handler_u(),
      this->get_mapping(),
      /*velocity_conv_diff_operator.get_operator_data().bc->dirichlet_bc,*/
      (void *)&velocity_conv_diff_operator.get_operator_data());
  }

  // setup linear solver for momentum equation
  if(this->param.solver_momentum == SolverMomentum::PCG)
  {
    // setup solver data
    CGSolverData solver_data;
    solver_data.solver_tolerance_abs = this->param.abs_tol_momentum_linear;
    solver_data.solver_tolerance_rel = this->param.rel_tol_momentum_linear;
    solver_data.max_iter             = this->param.max_iter_momentum_linear;

    if(this->param.preconditioner_momentum == MomentumPreconditioner::PointJacobi ||
       this->param.preconditioner_momentum == MomentumPreconditioner::BlockJacobi ||
       this->param.preconditioner_momentum == MomentumPreconditioner::InverseMassMatrix ||
       this->param.preconditioner_momentum == MomentumPreconditioner::VelocityDiffusion ||
       this->param.preconditioner_momentum == MomentumPreconditioner::VelocityConvectionDiffusion)
    {
      solver_data.use_preconditioner = true;
    }
    solver_data.update_preconditioner = this->param.update_preconditioner_momentum;

    // setup solver
    momentum_linear_solver.reset(
      new CGSolver<VelocityConvDiffOperator<dim, degree_u, Number>,
                   PreconditionerBase<Number>,
                   VectorType>(velocity_conv_diff_operator, *momentum_preconditioner, solver_data));
  }
  else if(this->param.solver_momentum == SolverMomentum::GMRES)
  {
    // setup solver data
    GMRESSolverData solver_data;
    solver_data.solver_tolerance_abs  = this->param.abs_tol_momentum_linear;
    solver_data.solver_tolerance_rel  = this->param.rel_tol_momentum_linear;
    solver_data.max_iter              = this->param.max_iter_momentum_linear;
    solver_data.right_preconditioning = this->param.use_right_preconditioning_momentum;
    solver_data.max_n_tmp_vectors     = this->param.max_n_tmp_vectors_momentum;
    solver_data.compute_eigenvalues   = false;

    if(this->param.preconditioner_momentum == MomentumPreconditioner::PointJacobi ||
       this->param.preconditioner_momentum == MomentumPreconditioner::BlockJacobi ||
       this->param.preconditioner_momentum == MomentumPreconditioner::InverseMassMatrix ||
       this->param.preconditioner_momentum == MomentumPreconditioner::VelocityDiffusion ||
       this->param.preconditioner_momentum == MomentumPreconditioner::VelocityConvectionDiffusion)
    {
      solver_data.use_preconditioner = true;
    }
    solver_data.update_preconditioner = this->param.update_preconditioner_momentum;

    // setup solver
    momentum_linear_solver.reset(new GMRESSolver<VelocityConvDiffOperator<dim, degree_u, Number>,
                                                 PreconditionerBase<Number>,
                                                 VectorType>(velocity_conv_diff_operator,
                                                             *momentum_preconditioner,
                                                             solver_data));
  }
  else if(this->param.solver_momentum == SolverMomentum::FGMRES)
  {
    FGMRESSolverData solver_data;
    solver_data.max_iter             = this->param.max_iter_momentum_linear;
    solver_data.solver_tolerance_abs = this->param.abs_tol_momentum_linear;
    solver_data.solver_tolerance_rel = this->param.rel_tol_momentum_linear;
    solver_data.max_n_tmp_vectors    = this->param.max_n_tmp_vectors_momentum;

    if(this->param.preconditioner_momentum == MomentumPreconditioner::PointJacobi ||
       this->param.preconditioner_momentum == MomentumPreconditioner::BlockJacobi ||
       this->param.preconditioner_momentum == MomentumPreconditioner::InverseMassMatrix ||
       this->param.preconditioner_momentum == MomentumPreconditioner::VelocityDiffusion ||
       this->param.preconditioner_momentum == MomentumPreconditioner::VelocityConvectionDiffusion)
    {
      solver_data.use_preconditioner = true;
    }
    solver_data.update_preconditioner = this->param.update_preconditioner_momentum;

    momentum_linear_solver.reset(new FGMRESSolver<VelocityConvDiffOperator<dim, degree_u, Number>,
                                                  PreconditionerBase<Number>,
                                                  VectorType>(velocity_conv_diff_operator,
                                                              *momentum_preconditioner,
                                                              solver_data));
  }
  else
  {
    AssertThrow(this->param.solver_momentum == SolverMomentum::PCG ||
                  this->param.solver_momentum == SolverMomentum::GMRES ||
                  this->param.solver_momentum == SolverMomentum::FGMRES,
                ExcMessage(
                  "Specified Viscous Solver not implemented - possibilities are PCG and GMRES"));
  }


  // Navier-Stokes equations with an implicit treatment of the convective term
  if(this->param.equation_type == EquationType::NavierStokes &&
     this->param.treatment_of_convective_term == TreatmentOfConvectiveTerm::Implicit)
  {
    // initialize temp vector
    this->initialize_vector_velocity(temp_vector);

    // setup Newton solver
    momentum_newton_solver.reset(
      new NewtonSolver<VectorType,
                       THIS,
                       VelocityConvDiffOperator<dim, degree_u, Number>,
                       IterativeSolverBase<VectorType>>(this->param.newton_solver_data_momentum,
                                                        *this,
                                                        velocity_conv_diff_operator,
                                                        *momentum_linear_solver));
  }
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::
  setup_inverse_mass_matrix_operator_pressure()
{
  // inverse mass matrix operator pressure (needed for pressure update in case of rotational
  // formulation)
  inverse_mass_matrix_operator_pressure.initialize(this->data,
                                                   this->get_dof_index_pressure(),
                                                   this->get_quad_index_pressure());
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::solve_linear_momentum_equation(
  VectorType &       solution,
  VectorType const & rhs,
  double const &     scaling_factor_mass_matrix_term,
  unsigned int &     linear_iterations)
{
  // Set scaling_factor_time_derivative_term for linear operator (=velocity_conv_diff_operator).
  velocity_conv_diff_operator.set_scaling_factor_time_derivative_term(
    scaling_factor_mass_matrix_term);

  // Note that there is no need to set the evaluation time for the velocity_conv_diff_operator
  // in this because because this function is only called if the convective term is not considered
  // in the velocity_conv_diff_operator (Stokes eq. or explicit treatment of convective term).

  linear_iterations = momentum_linear_solver->solve(solution, rhs);
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::evaluate_add_body_force_term(
  VectorType & dst,
  double const evaluation_time) const
{
  this->body_force_operator.evaluate_add(dst, evaluation_time);
}


template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::
  solve_nonlinear_momentum_equation(VectorType &       dst,
                                    VectorType const & rhs_vector,
                                    double const &     eval_time,
                                    double const &     scaling_factor_mass_matrix_term,
                                    unsigned int &     newton_iterations,
                                    unsigned int &     linear_iterations)
{
  // Set rhs_vector, this variable is used when evaluating the nonlinear residual
  this->rhs_vector = &rhs_vector;

  // Set evaluation_time for nonlinear operator (=DGNavierStokesPressureCorrection)
  evaluation_time = eval_time;
  // Set scaling_time_derivative_term for nonlinear operator (=DGNavierStokesPressureCorrection)
  scaling_factor_time_derivative_term = scaling_factor_mass_matrix_term;

  // Set correct evaluation time for linear operator (=velocity_conv_diff_operator).
  velocity_conv_diff_operator.set_evaluation_time(eval_time);
  // Set scaling_factor_time_derivative_term for linear operator (=velocity_conv_diff_operator).
  velocity_conv_diff_operator.set_scaling_factor_time_derivative_term(
    scaling_factor_mass_matrix_term);

  // Solve nonlinear problem
  momentum_newton_solver->solve(dst, newton_iterations, linear_iterations);

  // Reset rhs_vector
  this->rhs_vector = nullptr;
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::evaluate_nonlinear_residual(
  VectorType &       dst,
  VectorType const & src)
{
  // set dst to zero
  dst = 0.0;

  // mass matrix term
  temp_vector.equ(scaling_factor_time_derivative_term, src);
  this->mass_matrix_operator.apply_add(dst, temp_vector);

  // always evaluate convective term since this function is only called
  // if a nonlinear problem has to be solved, i.e., if the convective operator
  // has to be considered
  this->convective_operator.evaluate_add(dst, src, evaluation_time);

  // viscous term
  this->viscous_operator.evaluate_add(dst, src, evaluation_time);

  // rhs vector
  dst.add(-1.0, *rhs_vector);
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::
  apply_velocity_conv_diff_operator(VectorType &       dst,
                                    VectorType const & src,
                                    VectorType const & solution_linearization)
{
  velocity_conv_diff_operator.set_solution_linearization(solution_linearization);
  velocity_conv_diff_operator.vmult(dst, src);
}


template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::rhs_pressure_gradient_term(
  VectorType & dst,
  double const evaluation_time) const
{
  this->gradient_operator.rhs(dst, evaluation_time);
}


template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::
  apply_inverse_pressure_mass_matrix(VectorType & dst, VectorType const & src) const
{
  inverse_mass_matrix_operator_pressure.apply(dst, src);
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::do_postprocessing(
  VectorType const & velocity,
  VectorType const & pressure,
  double const       time,
  unsigned int const time_step_number)
{
  bool const standard = true;
  if(standard)
  {
    this->postprocessor->do_postprocessing(velocity,
                                           velocity, // intermediate_velocity
                                           pressure,
                                           time,
                                           time_step_number);
  }
  else // consider velocity and pressure errors instead
  {
    VectorType velocity_error;
    this->initialize_vector_velocity(velocity_error);

    VectorType pressure_error;
    this->initialize_vector_pressure(pressure_error);

    this->prescribe_initial_conditions(velocity_error, pressure_error, time);

    velocity_error.add(-1.0, velocity);
    pressure_error.add(-1.0, pressure);

    this->postprocessor->do_postprocessing(velocity_error, // error!
                                           velocity,       // intermediate_velocity
                                           pressure_error, // error!
                                           time,
                                           time_step_number);
  }
}

template<int dim, int degree_u, int degree_p, typename Number>
void
DGNavierStokesPressureCorrection<dim, degree_u, degree_p, Number>::do_postprocessing_steady_problem(
  VectorType const & velocity,
  VectorType const & pressure)
{
  this->postprocessor->do_postprocessing(velocity,
                                         velocity, // intermediate_velocity
                                         pressure);
}

} // namespace IncNS

#endif /* INCLUDE_INCOMPRESSIBLE_NAVIER_STOKES_SPATIAL_DISCRETIZATION_DG_NAVIER_STOKES_PRESSURE_CORRECTION_H_ \
        */
