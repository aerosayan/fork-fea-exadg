/*
 * driver.h
 *
 *  Created on: 01.04.2020
 *      Author: fehn
 */

#ifndef INCLUDE_FLUID_STRUCTURE_INTERACTION_DRIVER_H_
#define INCLUDE_FLUID_STRUCTURE_INTERACTION_DRIVER_H_

// application
#include "user_interface/application_base.h"

// IncNS: postprocessor
#include "../incompressible_navier_stokes/postprocessor/postprocessor.h"

// IncNS: spatial discretization
#include "../incompressible_navier_stokes/spatial_discretization/dg_coupled_solver.h"
#include "../incompressible_navier_stokes/spatial_discretization/dg_dual_splitting.h"
#include "../incompressible_navier_stokes/spatial_discretization/dg_pressure_correction.h"

// IncNS: temporal discretization
#include "../incompressible_navier_stokes/time_integration/time_int_bdf_coupled_solver.h"
#include "../incompressible_navier_stokes/time_integration/time_int_bdf_dual_splitting.h"
#include "../incompressible_navier_stokes/time_integration/time_int_bdf_pressure_correction.h"

// Poisson: spatial discretization
#include "../poisson/spatial_discretization/operator.h"

// Structure: spatial discretization
#include "../structure/spatial_discretization/operator.h"

// Structure: time integration
#include "../structure/time_integration/time_int_gen_alpha.h"

// grid
#include "../grid/mapping_degree.h"
#include "../grid/moving_mesh_elasticity.h"
#include "../grid/moving_mesh_poisson.h"

// matrix-free
#include "../matrix_free/matrix_free_wrapper.h"

// functionalities
#include "../functions_and_boundary_conditions/interface_coupling.h"
#include "../functions_and_boundary_conditions/verify_boundary_conditions.h"
#include "../utilities/print_general_infos.h"
#include "../utilities/timings_hierarchical.h"

namespace FSI
{
struct FixedPointData
{
  FixedPointData() : abs_tol(1.e-12), rel_tol(1.e-3), omega_init(0.1), partitioned_iter_max(100)
  {
  }

  double       abs_tol;
  double       rel_tol;
  double       omega_init;
  unsigned int partitioned_iter_max;
};

template<int dim, typename Number>
class Driver
{
private:
  typedef LinearAlgebra::distributed::Vector<Number> VectorType;

public:
  Driver(std::string const & input_file, MPI_Comm const & comm);

  void
  setup(std::shared_ptr<ApplicationBase<dim, Number>> application,
        unsigned int const &                          degree_fluid,
        unsigned int const &                          degree_ale,
        unsigned int const &                          degree_structure,
        unsigned int const &                          refine_space_fluid,
        unsigned int const &                          refine_space_structure);

  void
  solve() const;

  void
  print_statistics(double const total_time) const;

private:
  void
  print_header() const;

  void
  set_start_time() const;

  void
  synchronize_time_step_size() const;

  void
  coupling_structure_to_ale(VectorType & displacement_structure, bool const extrapolate) const;

  void
  solve_ale() const;

  void
  coupling_structure_to_fluid(bool const extrapolate) const;

  void
  coupling_fluid_to_structure() const;

  double
  calculate_residual(VectorType & residual) const;

  void
  update_relaxation_parameter(double &           omega,
                              unsigned int const i,
                              VectorType const & residual,
                              VectorType const & residual_last) const;

  void
  print_solver_info_header(unsigned int const i) const;

  void
  print_solver_info_converged(unsigned int const i) const;

  void
  print_partitioned_iterations() const;

  // MPI communicator
  MPI_Comm const & mpi_comm;

  // output to std::cout
  ConditionalOStream pcout;

  // application
  std::shared_ptr<ApplicationBase<dim, Number>> application;

  /**************************************** STRUCTURE *****************************************/

  // input parameters
  Structure::InputParameters structure_param;

  // triangulation
  std::shared_ptr<parallel::TriangulationBase<dim>> structure_triangulation;

  // mapping
  std::shared_ptr<Mesh<dim>> structure_mesh;

  // periodic boundaries
  std::vector<GridTools::PeriodicFacePair<typename Triangulation<dim>::cell_iterator>>
    structure_periodic_faces;

  // material descriptor
  std::shared_ptr<Structure::MaterialDescriptor> structure_material_descriptor;

  // boundary conditions
  std::shared_ptr<Structure::BoundaryDescriptor<dim>> structure_boundary_descriptor;

  // field functions
  std::shared_ptr<Structure::FieldFunctions<dim>> structure_field_functions;

  // matrix-free
  std::shared_ptr<MatrixFreeData<dim, Number>> structure_matrix_free_data;
  std::shared_ptr<MatrixFree<dim, Number>>     structure_matrix_free;

  // spatial discretization
  std::shared_ptr<Structure::Operator<dim, Number>> structure_operator;

  // temporal discretization
  std::shared_ptr<Structure::TimeIntGenAlpha<dim, Number>> structure_time_integrator;

  // postprocessor
  std::shared_ptr<Structure::PostProcessor<dim, Number>> structure_postprocessor;

  /**************************************** STRUCTURE *****************************************/


  /****************************************** FLUID *******************************************/

  // triangulation
  std::shared_ptr<parallel::TriangulationBase<dim>> fluid_triangulation;
  std::vector<GridTools::PeriodicFacePair<typename Triangulation<dim>::cell_iterator>>
    fluid_periodic_faces;

  // moving mesh for fluid problem
  std::shared_ptr<Mesh<dim>>                   fluid_mesh;
  std::shared_ptr<MovingMeshBase<dim, Number>> fluid_moving_mesh;

  // parameters
  IncNS::InputParameters fluid_param;

  std::shared_ptr<IncNS::FieldFunctions<dim>>      fluid_field_functions;
  std::shared_ptr<IncNS::BoundaryDescriptorU<dim>> fluid_boundary_descriptor_velocity;
  std::shared_ptr<IncNS::BoundaryDescriptorP<dim>> fluid_boundary_descriptor_pressure;

  // matrix-free
  std::shared_ptr<MatrixFreeData<dim, Number>> fluid_matrix_free_data;
  std::shared_ptr<MatrixFree<dim, Number>>     fluid_matrix_free;

  // spatial discretization
  std::shared_ptr<IncNS::DGNavierStokesBase<dim, Number>>          fluid_operator;
  std::shared_ptr<IncNS::DGNavierStokesCoupled<dim, Number>>       fluid_operator_coupled;
  std::shared_ptr<IncNS::DGNavierStokesDualSplitting<dim, Number>> fluid_operator_dual_splitting;
  std::shared_ptr<IncNS::DGNavierStokesPressureCorrection<dim, Number>>
    fluid_operator_pressure_correction;

  // temporal discretization
  std::shared_ptr<IncNS::TimeIntBDF<dim, Number>> fluid_time_integrator;

  // Postprocessor
  std::shared_ptr<IncNS::PostProcessorBase<dim, Number>> fluid_postprocessor;

  /****************************************** FLUID *******************************************/


  /************************************ ALE - MOVING MESH *************************************/

  // use a PDE solver for moving mesh problem
  std::shared_ptr<Mesh<dim>>                   ale_mesh;
  std::shared_ptr<MatrixFreeData<dim, Number>> ale_matrix_free_data;
  std::shared_ptr<MatrixFree<dim, Number>>     ale_matrix_free;

  // Poisson-type mesh smoothing
  Poisson::InputParameters ale_poisson_param;

  std::shared_ptr<Poisson::FieldFunctions<dim>>        ale_poisson_field_functions;
  std::shared_ptr<Poisson::BoundaryDescriptor<1, dim>> ale_poisson_boundary_descriptor;

  std::shared_ptr<Poisson::Operator<dim, Number, dim>> ale_poisson_operator;

  // elasticity-type mesh smoothing
  Structure::InputParameters ale_elasticity_param;

  std::shared_ptr<Structure::FieldFunctions<dim>>     ale_elasticity_field_functions;
  std::shared_ptr<Structure::BoundaryDescriptor<dim>> ale_elasticity_boundary_descriptor;
  std::shared_ptr<Structure::MaterialDescriptor>      ale_elasticity_material_descriptor;

  std::shared_ptr<Structure::Operator<dim, Number>> ale_elasticity_operator;

  /************************************ ALE - MOVING MESH *************************************/


  /******************************* FLUID - STRUCTURE - INTERFACE ******************************/

  std::shared_ptr<InterfaceCoupling<dim, dim, Number>> structure_to_fluid;
  std::shared_ptr<InterfaceCoupling<dim, dim, Number>> structure_to_ale;
  std::shared_ptr<InterfaceCoupling<dim, dim, Number>> fluid_to_structure;

  /******************************* FLUID - STRUCTURE - INTERFACE ******************************/

  /*
   *  Fixed-point iteration.
   */
  FixedPointData fixed_point_data;

  mutable VectorType residual_last;
  mutable VectorType displacement_last;

  /*
   * Computation time (wall clock time).
   */
  mutable TimerTree timer_tree;

  mutable std::pair<unsigned int, unsigned long long> partitioned_iterations;
};

} // namespace FSI


#endif /* INCLUDE_FLUID_STRUCTURE_INTERACTION_DRIVER_H_ */
