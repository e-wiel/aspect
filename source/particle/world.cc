/*
 Copyright (C) 2011 - 2015 by the authors of the ASPECT code.

 This file is part of ASPECT.

 ASPECT is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2, or (at your option)
 any later version.

 ASPECT is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with ASPECT; see the file doc/COPYING.  If not see
 <http://www.gnu.org/licenses/>.
 */

#include <aspect/particle/world.h>
#include <aspect/global.h>
#include <aspect/utilities.h>

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/grid/grid_tools.h>
#include <boost/serialization/map.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

namespace aspect
{
  namespace Particle
  {
    template <>
    World<2>::World()
    {
      data_offset = aspect::Utilities::signaling_nan<unsigned int>();
      integrator = NULL;
      aspect::internals::SimulatorSignals::register_connector_function_2d (std_cxx11::bind(&World<2>::connector_function,std_cxx11::ref(*this),std_cxx11::_1));
    }

    template <>
    World<3>::World()
    {
      data_offset = aspect::Utilities::signaling_nan<unsigned int>();
      integrator = NULL;
      aspect::internals::SimulatorSignals::register_connector_function_3d (std_cxx11::bind(&World<3>::connector_function,std_cxx11::ref(*this),std_cxx11::_1));
    }

    template <int dim>
    World<dim>::~World()
    {}

    template <int dim>
    void
    World<dim>::initialize()
    {}

    template <int dim>
    void
    World<dim>::set_integrator(Integrator::Interface<dim> *new_integrator)
    {
      integrator = new_integrator;
    }

    template <int dim>
    void
    World<dim>::set_manager(Property::Manager<dim> *new_manager)
    {
      property_manager = new_manager;
    }

    template <int dim>
    const Property::Manager<dim> &
    World<dim>::get_manager() const
    {
      return *property_manager;
    }

    template <int dim>
    void
    World<dim>::set_max_particles_per_cell(const unsigned int max_part_per_cell)
    {
      max_particles_per_cell = max_part_per_cell;
    }

    template <int dim>
    void
    World<dim>::add_particle(const Particle<dim> &particle, const LevelInd &cell)
    {
      const typename parallel::distributed::Triangulation<dim>::active_cell_iterator it
      (&(this->get_triangulation()), cell.first, cell.second);
      AssertThrow(it != this->get_triangulation().end(),
                  ExcMessage("Particles may only be added to cells in local subdomain."));
      particles.insert(std::make_pair(cell, particle));
    }

    template <int dim>
    std::multimap<LevelInd, Particle<dim> > &
    World<dim>::get_particles()
    {
      return particles;
    }

    template <int dim>
    const std::multimap<LevelInd, Particle<dim> > &
    World<dim>::get_particles() const
    {
      return particles;
    }

    template <int dim>
    unsigned int
    World<dim>::get_global_particle_count() const
    {
      return dealii::Utilities::MPI::sum (particles.size(), this->get_mpi_communicator());
    }

    template <int dim>
    unsigned int
    World<dim>::get_global_max_tracer_per_cell() const
    {
      unsigned int local_max_tracer_per_cell(0);
      typename parallel::distributed::Triangulation<dim>::active_cell_iterator cell = this->get_triangulation().begin_active();
      for (; cell!=this->get_triangulation().end(); ++cell)
        if (cell->is_locally_owned())
          {
            const LevelInd found_cell = std::make_pair<int, int> (cell->level(),cell->index());
            const std::pair<typename std::multimap<LevelInd, Particle<dim> >::const_iterator, typename std::multimap<LevelInd, Particle<dim> >::const_iterator> particles_in_cell
              = particles.equal_range(found_cell);
            const unsigned int tracers_in_cell = std::distance(particles_in_cell.first,particles_in_cell.second);
            local_max_tracer_per_cell = std::max(local_max_tracer_per_cell,
                                                 tracers_in_cell);
          }

      return dealii::Utilities::MPI::max(local_max_tracer_per_cell,this->get_mpi_communicator());
    }

    template <int dim>
    void
    World<dim>::connector_function(aspect::SimulatorSignals<dim> &signals)
    {
      signals.pre_refinement_store_user_data.connect(std_cxx11::bind(&World<dim>::register_store_callback_function,
                                                                     std_cxx11::ref(*this),
                                                                     std_cxx11::_1));
      signals.post_refinement_load_user_data.connect(std_cxx11::bind(&World<dim>::register_load_callback_function,
                                                                     std_cxx11::ref(*this),
                                                                     std_cxx11::_1));
    }

    template <int dim>
    void
    World<dim>::register_store_callback_function(typename parallel::distributed::Triangulation<dim> &triangulation)
    {
      // Only save and load tracers if there are any, we might get here for
      // example before the tracer generation in timestep 0, or if somebody
      // selected the tracer postprocessor but generated 0 tracers
      const unsigned int max_tracers_per_cell = get_global_max_tracer_per_cell();

      if (max_tracers_per_cell > 0)
        {
          const std_cxx11::function<void(const typename parallel::distributed::Triangulation<dim>::cell_iterator &,
                                         const typename parallel::distributed::Triangulation<dim>::CellStatus, void *) > callback_function
            = std_cxx11::bind(&aspect::Particle::World<dim>::store_tracers,
                              std_cxx11::ref(*this),
                              std_cxx11::_1,
                              std_cxx11::_2,
                              std_cxx11::_3);

          // We need to transfer the number of tracers for this cell and
          // the tracer data itself and we need to provide 2^dim times the
          // space for the data in case a cell is coarsened
          const std::size_t transfer_size_per_cell = sizeof (unsigned int) +
                                                     (property_manager->get_particle_size() * max_tracers_per_cell)
                                                     *  std::pow(2,dim);
          data_offset = triangulation.register_data_attach(transfer_size_per_cell,callback_function);
        }
    }

    template <int dim>
    void
    World<dim>::register_load_callback_function(typename parallel::distributed::Triangulation<dim> &triangulation)
    {
      Assert(particles.size() == 0,
             ExcMessage("We are in the process of mesh refinement. All tracers "
                        "should have been serialized and stored, but there are still some "
                        "around. Is there a bug in the storage function?"));

      // Check if something was stored
      if (data_offset != aspect::Utilities::signaling_nan<unsigned int>())
        {
          const std_cxx11::function<void(const typename parallel::distributed::Triangulation<dim>::cell_iterator &,
                                         const typename parallel::distributed::Triangulation<dim>::CellStatus,
                                         const void *) > callback_function
            = std_cxx11::bind(&aspect::Particle::World<dim>::load_tracers,
                              std_cxx11::ref(*this),
                              std_cxx11::_1,
                              std_cxx11::_2,
                              std_cxx11::_3);

          triangulation.notify_ready_to_unpack(data_offset,callback_function);
          data_offset = aspect::Utilities::signaling_nan<unsigned int>();
        }
    }

    template <int dim>
    void
    World<dim>::store_tracers(const typename parallel::distributed::Triangulation<dim>::cell_iterator &cell,
                              const typename parallel::distributed::Triangulation<dim>::CellStatus status,
                              void *data)
    {
      unsigned int n_particles_in_cell(0);

      if (status == parallel::distributed::Triangulation<dim>::CELL_PERSIST
          || status == parallel::distributed::Triangulation<dim>::CELL_REFINE)
        {
          const LevelInd found_cell = std::make_pair<int, int> (cell->level(),cell->index());
          const std::pair<typename std::multimap<LevelInd, Particle<dim> >::iterator, typename std::multimap<LevelInd, Particle<dim> >::iterator> particles_in_cell
            = particles.equal_range(found_cell);
          n_particles_in_cell = std::distance(particles_in_cell.first,particles_in_cell.second);

          unsigned int *ndata = static_cast<unsigned int *> (data);
          *ndata++ = n_particles_in_cell;
          data = static_cast<void *> (ndata);

          for (typename std::multimap<LevelInd, Particle<dim> >::iterator particle = particles_in_cell.first;
               particle != particles_in_cell.second; ++particle)
            {
              particle->second.write_data(data);
            }
          particles.erase(particles_in_cell.first,particles_in_cell.second);
        }
      else if (status == parallel::distributed::Triangulation<dim>::CELL_COARSEN)
        {
          const unsigned int coarsen_factor = (dim == 3) ? 8 : 4;

          for (unsigned int child_index = 0; child_index < cell->number_of_children(); ++child_index)
            {
              const typename parallel::distributed::Triangulation<dim>::cell_iterator child = cell->child(child_index);
              const LevelInd found_cell = std::make_pair<int, int> (child->level(),child->index());
              const std::pair<typename std::multimap<LevelInd, Particle<dim> >::iterator, typename std::multimap<LevelInd, Particle<dim> >::iterator> particles_in_cell
                = particles.equal_range(found_cell);
              n_particles_in_cell += std::distance(particles_in_cell.first,particles_in_cell.second);
            }

          bool reduce_tracers = false;
          if ((max_particles_per_cell > 0) && (n_particles_in_cell > max_particles_per_cell))
            {
              n_particles_in_cell /= coarsen_factor;
              reduce_tracers = true;
            }

          unsigned int *ndata = static_cast<unsigned int *> (data);
          *ndata++ = n_particles_in_cell;

          data = static_cast<void *> (ndata);

          unsigned int particle_index = 0;

          for (unsigned int child_index = 0; child_index < cell->number_of_children(); ++child_index)
            {
              const typename parallel::distributed::Triangulation<dim>::cell_iterator child = cell->child(child_index);
              const LevelInd found_cell = std::make_pair<int, int> (child->level(),child->index());
              const std::pair<typename std::multimap<LevelInd, Particle<dim> >::iterator, typename std::multimap<LevelInd, Particle<dim> >::iterator>
              particles_in_cell = particles.equal_range(found_cell);

              for (typename std::multimap<LevelInd, Particle<dim> >::iterator particle = particles_in_cell.first;
                   particle != particles_in_cell.second; ++particle, ++particle_index)
                {
                  if (!reduce_tracers || (particle_index % coarsen_factor == 0))
                    particle->second.write_data(data);
                }

              particles.erase(particles_in_cell.first,particles_in_cell.second);
            }
        }
    }

    template <int dim>
    void
    World<dim>::load_tracers(const typename parallel::distributed::Triangulation<dim>::cell_iterator &cell,
                             const typename parallel::distributed::Triangulation<dim>::CellStatus status,
                             const void *data)
    {
      const unsigned int *n_particles_in_cell = static_cast<const unsigned int *> (data);
      const unsigned int particles_in_cell = *n_particles_in_cell++;
      const void *pdata = reinterpret_cast<const void *> (n_particles_in_cell);

      for (unsigned int i = 0; i < particles_in_cell; ++i)
        {
          Particle<dim> p(pdata,property_manager->get_data_len());

          if (status == parallel::distributed::Triangulation<dim>::CELL_COARSEN
              || status == parallel::distributed::Triangulation<dim>::CELL_PERSIST)
            particles.insert(std::make_pair(std::make_pair(cell->level(),cell->index()),p));
          else if (status == parallel::distributed::Triangulation<dim>::CELL_REFINE)
            {
              for (unsigned int child_index = 0; child_index < cell->number_of_children(); ++child_index)
                {
                  const typename parallel::distributed::Triangulation<dim>::cell_iterator child = cell->child(child_index);
                  try
                    {
                      const Point<dim> p_unit = this->get_mapping().transform_real_to_unit_cell(child, p.get_location());
                      if (GeometryInfo<dim>::is_inside_unit_cell(p_unit))
                        {
                          particles.insert(std::make_pair(std::make_pair(child->level(),child->index()),p));
                        }
                    }
                  catch (...)
                    {}
                }
            }
        }
    }

    template <int dim>
    std::vector<types::subdomain_id>
    World<dim>::find_neighbors() const
    {
      std::vector<types::subdomain_id> neighbors;

      for (typename Triangulation<dim>::active_cell_iterator
           cell = this->get_triangulation().begin_active();
           cell != this->get_triangulation().end(); ++cell)
        {
          if (cell->is_ghost())
            {
              bool neighbor_already_marked = false;

              for (unsigned int i = 0; i < neighbors.size(); ++i)
                if (neighbors[i] == cell->subdomain_id())
                  {
                    neighbor_already_marked = true;
                    break;
                  }

              if (!neighbor_already_marked)
                neighbors.push_back(cell->subdomain_id());
            }
        }
      return neighbors;
    }

    template <int dim>
    void
    World<dim>::find_all_cells()
    {
      std::multimap<types::subdomain_id, Particle<dim> > lost_particles;
      std::multimap<LevelInd, Particle<dim> > moved_particles;

      // Find the cells that the particles moved to.
      // Note that the iterator in the following loop is increased in a
      // very particular way, because it is changed, if elements
      // get erased. A change can result in invalid memory access.
      typename std::multimap<LevelInd, Particle<dim> >::iterator   it;
      for (it=particles.begin(); it!=particles.end();)
        {
          if (it->first != std::make_pair(-1,-1))
            {
              const typename parallel::distributed::Triangulation<dim>::active_cell_iterator
              old_cell (&(this->get_triangulation()), it->first.first, it->first.second);

              try
                {
                  const Point<dim> p_unit = this->get_mapping().transform_real_to_unit_cell(old_cell, it->second.get_location());
                  if (GeometryInfo<dim>::is_inside_unit_cell(p_unit))
                    {
                      ++it;
                      continue;
                    }
                }
              catch (...)
                {}
            }

          typename parallel::distributed::Triangulation<dim>::active_cell_iterator cell;
          try
            {
              cell = (GridTools::find_active_cell_around_point<> (this->get_mapping(), this->get_triangulation(), it->second.get_location())).first;
            }
          catch (GridTools::ExcPointNotFound<dim> &)
            {
              // If we can find no cell for this particle it has left the domain due
              // to an integration error or an open boundary. Simply remove the
              // tracer in this case.
              particles.erase(it++);
              continue;
            }


          // Reinsert the particle into our domain if we found its cell
          // Mark it for MPI transfer otherwise
          if (cell->is_locally_owned())
            {
              const LevelInd found_cell = std::make_pair(cell->level(),cell->index());
              moved_particles.insert(std::make_pair(found_cell, it->second));
            }
          else
            lost_particles.insert(std::make_pair(cell->subdomain_id(),it->second));

          particles.erase(it++);
        }
      particles.insert(moved_particles.begin(),moved_particles.end());

      // If particles fell out of the mesh, put them back in at the closest point in the mesh
      move_particles_back_in_mesh();

      // Swap lost particles between processors if we have more than one process
      if (dealii::Utilities::MPI::n_mpi_processes(this->get_mpi_communicator()) > 1)
        send_recv_particles(lost_particles);
    }

    template <int dim>
    void
    World<dim>::move_particles_back_in_mesh()
    {
      // TODO: fix this to work with arbitrary meshes
    }

    template <int dim>
    void
    World<dim>::send_recv_particles(const std::multimap<types::subdomain_id,Particle <dim> > &send_particles)
    {
      // Determine the communication pattern
      const std::vector<types::subdomain_id> neighbors = find_neighbors();
      const unsigned int num_neighbors = neighbors.size();
      const unsigned int self_rank  = dealii::Utilities::MPI::this_mpi_process(this->get_mpi_communicator());
      const unsigned int particle_size = property_manager->get_particle_size() + integrator->data_length() * sizeof(double);

      // Determine the amount of data we will send to other processors
      std::vector<int> num_send_data(num_neighbors);
      std::vector<int> num_recv_data(num_neighbors);

      std::vector<int> send_offsets(num_neighbors);
      std::vector<int> recv_offsets(num_neighbors);

      // Allocate space for sending and receiving particle data
      std::vector<char> send_data(send_particles.size() * particle_size);
      void *data = static_cast<void *> (&send_data.front());

      int total_send_data = 0;
      for (types::subdomain_id neighbor_id = 0; neighbor_id < num_neighbors; ++neighbor_id)
        {
          send_offsets[neighbor_id] = total_send_data;

          std::pair< const typename std::multimap<types::subdomain_id,Particle <dim> >::const_iterator,
              const typename std::multimap<types::subdomain_id,Particle <dim> >::const_iterator>
              send_particle_range = send_particles.equal_range(neighbors[neighbor_id]);

          int num_send_particles = std::distance(send_particle_range.first,send_particle_range.second);
          num_send_data[neighbor_id] = num_send_particles * particle_size;
          total_send_data += num_send_particles * particle_size;

          // Copy the particle data into the send array
          typename std::multimap<types::subdomain_id,Particle<dim> >::const_iterator particle = send_particle_range.first;
          for (; particle != send_particle_range.second; ++particle)
            {
              particle->second.write_data(data);
              integrator->write_data(data, particle->second.get_id());
            }
        }

      AssertThrow(data == &(send_data.back())+1,
                  ExcMessage("The amount of data written into the array that is send to other processes "
                             "is inconsistent with the number and size of particles."));

      // Notify other processors how many particles we will send
      MPI_Request *num_requests = new MPI_Request[2*num_neighbors];
      for (unsigned int i=0; i<num_neighbors; ++i)
        MPI_Irecv(&(num_recv_data[i]), 1, MPI_INT, neighbors[i], 0, this->get_mpi_communicator(), &(num_requests[2*i]));
      for (unsigned int i=0; i<num_neighbors; ++i)
        MPI_Isend(&(num_send_data[i]), 1, MPI_INT, neighbors[i], 0, this->get_mpi_communicator(), &(num_requests[2*i+1]));
      MPI_Waitall(2*num_neighbors,num_requests,MPI_STATUSES_IGNORE);
      delete num_requests;

      // Determine how many particles and data we will receive
      int total_recv_data = 0;
      for (unsigned int neighbor_id=0; neighbor_id<num_neighbors; ++neighbor_id)
        {
          recv_offsets[neighbor_id] = total_recv_data;
          total_recv_data += num_recv_data[neighbor_id];
        }
      const int num_recv_particles = total_recv_data / particle_size;

      // Set up the space for the received particle data
      std::vector<char> recv_data(total_recv_data);

      // Exchange the particle data between domains
      MPI_Request *requests = new MPI_Request[2*num_neighbors];
      unsigned int send_ops = 0;
      unsigned int recv_ops = 0;

      for (unsigned int i=0; i<num_neighbors; ++i)
        if (num_recv_data[i] > 0)
          {
            MPI_Irecv(&(recv_data[recv_offsets[i]]), num_recv_data[i], MPI_CHAR, neighbors[i], 1, this->get_mpi_communicator(),&(requests[send_ops]));
            send_ops++;
          }

      for (unsigned int i=0; i<num_neighbors; ++i)
        if (num_send_data[i] > 0)
          {
            MPI_Isend(&(send_data[send_offsets[i]]), num_send_data[i], MPI_CHAR, neighbors[i], 1, this->get_mpi_communicator(),&(requests[send_ops+recv_ops]));
            recv_ops++;
          }
      MPI_Waitall(send_ops+recv_ops,requests,MPI_STATUSES_IGNORE);
      delete requests;

      // Put the received particles into the domain if they are in the triangulation
      const void *recv_data_it = static_cast<const void *> (&recv_data.front());

      for (int i=0; i<num_recv_particles; ++i)
        {
          Particle<dim> recv_particle(recv_data_it,property_manager->get_data_len());
          integrator->read_data(recv_data_it, recv_particle.get_id());

          typename parallel::distributed::Triangulation<dim>::active_cell_iterator cell;
          try
            {
              cell = (GridTools::find_active_cell_around_point<> (this->get_mapping(), this->get_triangulation(), recv_particle.get_location())).first;
            }
          catch (GridTools::ExcPointNotFound<dim> &)
            {
              // If we can find no cell for this particle it has left the domain due
              // to an integration error or open boundary. Simply ignore the
              // tracer in this case.
              continue;
            }

          if (cell->is_locally_owned())
            {
              const LevelInd found_cell = std::make_pair(cell->level(),cell->index());
              // Detect if we need to reduce the number of tracers in this cell,
              // we first reduce the incoming tracers, because they likely came from
              // a region, where the particle density is higher than in this cell
              // (otherwise this would not have been triggered).
              const unsigned int coarsen_factor = (dim == 3) ? 8 : 4;
              const bool reduce_tracers = (max_particles_per_cell > 0) && (particles.count(found_cell) == max_particles_per_cell);
              if ( !reduce_tracers || (i % coarsen_factor == 0))
                {
                  particles.insert(std::make_pair(found_cell, recv_particle));
                }
            }
        }

      AssertThrow(recv_data_it == &recv_data.back()+1,
                  ExcMessage("The amount of data that was read into new particles "
                             "does not match the amount of data sent around."));
    }

    template <int dim>
    void
    World<dim>::local_initialize_particles(const typename DoFHandler<dim>::active_cell_iterator &cell,
                                           const typename std::multimap<LevelInd, Particle<dim> >::iterator &begin_particle,
                                           const typename std::multimap<LevelInd, Particle<dim> >::iterator &end_particle)
    {
      const unsigned int particles_in_cell = std::distance(begin_particle,end_particle);
      const unsigned int solution_components = this->introspection().n_components;

      Vector<double> value(solution_components);
      std::vector<Tensor<1,dim> > gradient (solution_components,Tensor<1,dim>());

      std::vector<Vector<double> >  values(particles_in_cell,value);
      std::vector<std::vector<Tensor<1,dim> > > gradients(particles_in_cell,gradient);

      std::vector<Point<dim> >     particle_points(particles_in_cell);

      typename std::multimap<LevelInd, Particle<dim> >::iterator it = begin_particle;
      for (unsigned int i = 0; it!=end_particle; ++it,++i)
        {
          const Point<dim> position = it->second.get_location();
          particle_points[i] = this->get_mapping().transform_real_to_unit_cell(cell, position);
        }

      const std::vector< double > weights(particles_in_cell, 1./((double) particles_in_cell));
      const Quadrature<dim> quadrature_formula(particle_points, weights);
      FEValues<dim> fe_value (this->get_mapping(),
                              this->get_fe(),
                              quadrature_formula,
                              update_values |
                              update_gradients);

      fe_value.reinit (cell);
      fe_value.get_function_values (this->get_solution(),
                                    values);
      fe_value.get_function_gradients (this->get_solution(),
                                       gradients);

      it = begin_particle;
      for (unsigned int i = 0; it!=end_particle; ++it,++i)
        {
          property_manager->initialize_particle(it->second,
                                                values[i],
                                                gradients[i]);
        }
    }

    template <int dim>
    void
    World<dim>::local_update_particles(const typename DoFHandler<dim>::active_cell_iterator &cell,
                                       const typename std::multimap<LevelInd, Particle<dim> >::iterator &begin_particle,
                                       const typename std::multimap<LevelInd, Particle<dim> >::iterator &end_particle)
    {
      const unsigned int particles_in_cell = std::distance(begin_particle,end_particle);
      const unsigned int solution_components = this->introspection().n_components;

      Vector<double> value(solution_components);
      std::vector<Tensor<1,dim> > gradient (solution_components,Tensor<1,dim>());

      std::vector<Vector<double> >  values(particles_in_cell,value);
      std::vector<std::vector<Tensor<1,dim> > > gradients(particles_in_cell,gradient);

      std::vector<Point<dim> >     particle_points(particles_in_cell);

      typename std::multimap<LevelInd, Particle<dim> >::iterator it = begin_particle;
      for (unsigned int i = 0; it!=end_particle; ++it,++i)
        {
          const Point<dim> position = it->second.get_location();
          particle_points[i] = this->get_mapping().transform_real_to_unit_cell(cell, position);
        }

      const std::vector< double > weights(particles_in_cell, 1./((double) particles_in_cell));
      const Quadrature<dim> quadrature_formula(particle_points, weights);
      FEValues<dim> fe_value (this->get_mapping(),
                              this->get_fe(),
                              quadrature_formula,
                              update_values |
                              update_gradients);

      fe_value.reinit (cell);
      fe_value.get_function_values (this->get_solution(),
                                    values);
      fe_value.get_function_gradients (this->get_solution(),
                                       gradients);

      it = begin_particle;
      for (unsigned int i = 0; it!=end_particle; ++it,++i)
        {
          property_manager->update_particle(it->second,
                                            values[i],
                                            gradients[i]);
        }
    }

    template <int dim>
    void
    World<dim>::local_advect_particles(const typename DoFHandler<dim>::active_cell_iterator &cell,
                                       const typename std::multimap<LevelInd, Particle<dim> >::iterator &begin_particle,
                                       const typename std::multimap<LevelInd, Particle<dim> >::iterator &end_particle)
    {
      const unsigned int particles_in_cell = std::distance(begin_particle,end_particle);

      std::vector<Tensor<1,dim> >  result(particles_in_cell);
      std::vector<Tensor<1,dim> >  old_result(particles_in_cell);
      std::vector<Point<dim> >     particle_points(particles_in_cell);

      typename std::multimap<LevelInd, Particle<dim> >::iterator it = begin_particle;
      for (unsigned int i = 0; it!=end_particle; ++it,++i)
        {
          const Point<dim> position = it->second.get_location();
          particle_points[i] = this->get_mapping().transform_real_to_unit_cell(cell, position);
        }

      const std::vector< double > weights(particles_in_cell, 1./((double) particles_in_cell));
      const Quadrature<dim> quadrature_formula(particle_points, weights);
      FEValues<dim> fe_value (this->get_mapping(),
                              this->get_fe(),
                              quadrature_formula,
                              update_values);

      fe_value.reinit (cell);
      fe_value[this->introspection().extractors.velocities].get_function_values (this->get_solution(),
                                                                                 result);
      fe_value[this->introspection().extractors.velocities].get_function_values (this->get_old_solution(),
                                                                                 old_result);

      integrator->local_integrate_step(begin_particle,
                                       end_particle,
                                       old_result,
                                       result,
                                       this->get_old_timestep());
    }


    template <int dim>
    void
    World<dim>::initialize_particles()
    {
      // Loop over all cells and initialize the particles cell-wise
      typename DoFHandler<dim>::active_cell_iterator
      cell = this->get_dof_handler().begin_active(),
      endc = this->get_dof_handler().end();

      for (; cell!=endc; ++cell)
        if (cell->is_locally_owned())
          {
            std::pair< const typename std::multimap<LevelInd,Particle <dim> >::iterator,
                const typename std::multimap<LevelInd,Particle <dim> >::iterator>
                particle_range_in_cell = particles.equal_range(std::make_pair(cell->level(),cell->index()));

            // Only initialize particles, if there are any in this cell
            if (particle_range_in_cell.first != particle_range_in_cell.second)
              local_initialize_particles(cell,
                                         particle_range_in_cell.first,
                                         particle_range_in_cell.second);
          }
    }

    template <int dim>
    void
    World<dim>::update_particles()
    {
      // Loop over all cells and update the particles cell-wise
      typename DoFHandler<dim>::active_cell_iterator
      cell = this->get_dof_handler().begin_active(),
      endc = this->get_dof_handler().end();

      for (; cell!=endc; ++cell)
        if (cell->is_locally_owned())
          {
            std::pair< const typename std::multimap<LevelInd,Particle <dim> >::iterator,
                const typename std::multimap<LevelInd,Particle <dim> >::iterator>
                particle_range_in_cell = particles.equal_range(std::make_pair(cell->level(),cell->index()));

            // Only update particles, if there are any in this cell
            if (particle_range_in_cell.first != particle_range_in_cell.second)
              local_update_particles(cell,
                                     particle_range_in_cell.first,
                                     particle_range_in_cell.second);
          }
    }

    template <int dim>
    void
    World<dim>::advect_particles()
    {
      // Loop over all cells and advect the particles cell-wise
      typename DoFHandler<dim>::active_cell_iterator
      cell = this->get_dof_handler().begin_active(),
      endc = this->get_dof_handler().end();

      for (; cell!=endc; ++cell)
        if (cell->is_locally_owned())
          {
            std::pair< const typename std::multimap<LevelInd,Particle <dim> >::iterator,
                const typename std::multimap<LevelInd,Particle <dim> >::iterator>
                particle_range_in_cell = particles.equal_range(std::make_pair(cell->level(),cell->index()));

            // Only advect particles, if there are any in this cell
            if (particle_range_in_cell.first != particle_range_in_cell.second)
              local_advect_particles(cell,
                                     particle_range_in_cell.first,
                                     particle_range_in_cell.second);
          }
    }

    template <int dim>
    void
    World<dim>::advance_timestep()
    {
      // Keep calling the integrator until it indicates it is finished
      do
        {
          advect_particles();

          // Find the cells that the particles moved to
          find_all_cells();

          integrator->advance_step();
        }
      while (integrator->continue_integration());

      // Update particle properties
      if (property_manager->need_update() == Property::update_time_step)
        update_particles();
    }
  }
}


// explicit instantiation of the functions we implement in this file
namespace aspect
{
  namespace Particle
  {
#define INSTANTIATE(dim) \
  template class World<dim>;

    ASPECT_INSTANTIATE(INSTANTIATE)
  }
}
