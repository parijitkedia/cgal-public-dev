// Copyright (c) 2017, 2018 GeometryFactory (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
// You can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// Licensees holding a valid commercial license may use this file in
// accordance with the commercial license agreement provided with the software.
//
// This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
// WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
//
// Author(s)     : Mael Rouxel-Labbé

#ifndef CGAL_POLYLINE_TRACING_MOTORCYCLE_GRAPH_H
#define CGAL_POLYLINE_TRACING_MOTORCYCLE_GRAPH_H

#define CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE // @tmp

#include <CGAL/Polyline_tracing/Dictionary.h>
#include <CGAL/Polyline_tracing/Motorcycle.h>
#include <CGAL/Polyline_tracing/Motorcycle_priority_queue.h>
#include <CGAL/Polyline_tracing/internal/robust_collinear.h>
#include <CGAL/Polyline_tracing/internal/robust_intersections.h>
#include <CGAL/Polyline_tracing/internal/VPM_selector.h>

#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_tree.h>
#include <CGAL/array.h>
#include <CGAL/assertions.h>
#include <CGAL/boost/graph/iterator.h>
#include <CGAL/Cartesian_converter.h>
#include <CGAL/enum.h>
#include <CGAL/iterator.h>
#include <CGAL/number_utils.h>
#include <CGAL/Polygon_mesh_processing/locate.h>
#include <CGAL/result_of.h>

#include <boost/optional.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/unordered_map.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace CGAL {

namespace Polyline_tracing {

namespace internal {

// This struct regroups all useful information on a potential intersection
template<typename Motorcycle_graph>
struct Collision_information
{
  typedef typename Motorcycle_graph::Triangle_mesh           Triangle_mesh;
  typedef typename Motorcycle_graph::FT                      FT;
  typedef typename Motorcycle_graph::DEC_it                  DEC_it;
  typedef typename Motorcycle_graph::face_descriptor         face_descriptor;
  typedef typename Motorcycle_graph::Face_location           Face_location;
  typedef typename Motorcycle_graph::Barycentric_coordinates Barycentric_coordinates;

  // Constructor
  Collision_information(const FT max_time_at_collision)
    :
      maximum_time_at_collision(max_time_at_collision),

      // information related to the current face
      is_closest_collision_already_in_dictionary(false),
      closest_collision(),
      closest_collision_location(std::make_pair(boost::graph_traits<Triangle_mesh>::null_face(),
                                                Barycentric_coordinates())),
      time_at_closest_collision(std::numeric_limits<FT>::max()),

      // information related to the neighboring foreign face
      fmc_id(-1),
      is_foreign_motorcycle_in_different_face(false),
      foreign_motorcycle_face(boost::graph_traits<Triangle_mesh>::null_face()),
      foreign_time_at_closest_collision(std::numeric_limits<FT>::max()),

      add_intersection_without_moving_motorcycles(false)
  { }

  // Functions
  bool found_collision() const
  {
    // Either a DEC_it is provided, or the location should be provided
    return (is_closest_collision_already_in_dictionary ||
            closest_collision_location.first != boost::graph_traits<Triangle_mesh>::null_face());
  }

  // Check if the times provided passed in arguments correspond to a collision
  // earlier than the current best, bounded by the maximum time (time at closest target)
  bool is_collision_earlier_than_current_best(const FT time_at_collision,
                                              const FT foreign_time_at_collision) const
  {
    if(time_at_collision > maximum_time_at_collision)
      return false;

    const bool is_collision_earlier = (time_at_collision < time_at_closest_collision);
    const bool is_equal_collision_time_with_earlier_foreign_collision =
                 (time_at_collision == time_at_closest_collision &&
                  foreign_time_at_collision < foreign_time_at_closest_collision);

    const bool is_better = is_collision_earlier || is_equal_collision_time_with_earlier_foreign_collision;

    if(is_better)
    {
      std::cout << "New earliest collision times: " << time_at_collision << " || "
                                                    << foreign_time_at_collision;
      std::cout << " [previously: " << time_at_closest_collision << " || "
                                    << foreign_time_at_closest_collision << "]" << std::endl;
    }

    return is_better;
  }

  void reset()
  {
    // information related to the current face
    is_closest_collision_already_in_dictionary = false;
    closest_collision = DEC_it();
    closest_collision_location = std::make_pair(boost::graph_traits<Triangle_mesh>::null_face(),
                                                Barycentric_coordinates());
    time_at_closest_collision = std::numeric_limits<FT>::max();

    // information related to the neighboring foreign face
    fmc_id = -1;
    is_foreign_motorcycle_in_different_face = false,
    foreign_motorcycle_face = boost::graph_traits<Triangle_mesh>::null_face();
    foreign_time_at_closest_collision = std::numeric_limits<FT>::max();

    add_intersection_without_moving_motorcycles = false;
  }

public:
  const FT maximum_time_at_collision;

  bool is_closest_collision_already_in_dictionary;
  DEC_it closest_collision;
  Face_location closest_collision_location;
  FT time_at_closest_collision;

  std::size_t fmc_id;
  bool is_foreign_motorcycle_in_different_face;
  face_descriptor foreign_motorcycle_face;
  FT foreign_time_at_closest_collision;

  bool add_intersection_without_moving_motorcycles;
};

} // namespace internal

// @todo define global snapping values
// @todo some type of looping detection mechanism (?)

template<typename MotorcycleGraphTraits>
class Motorcycle_graph
{
  typedef Motorcycle_graph<MotorcycleGraphTraits>             Self;

  typedef internal::Collision_information<Self>               Collision_information;
  typedef typename MotorcycleGraphTraits::Kernel              K;

public:
  typedef MotorcycleGraphTraits                               Geom_traits;
  typedef typename Geom_traits::Triangle_mesh                 Triangle_mesh;

  // Geometric types
  typedef typename Geom_traits::FT                            FT;

  typedef typename Geom_traits::Point_2                       Point_2;
  typedef typename Geom_traits::Segment_2                     Segment_2;
  typedef typename Geom_traits::Vector_2                      Vector_2;

  typedef typename Geom_traits::Point_d                       Point;
  typedef typename Geom_traits::Segment_d                     Segment;
  typedef typename Geom_traits::Vector_d                      Vector;
  typedef typename Geom_traits::Ray_d                         Ray;

  typedef typename Geom_traits::Bbox_d                        Bbox;

  // Point type
  typedef Dictionary<Geom_traits>                             Dictionary;
  typedef typename Dictionary::DEC_it                         DEC_it;

  typedef typename Geom_traits::Barycentric_coordinates       Barycentric_coordinates;
  typedef typename Geom_traits::Face_location                 Face_location;

  // Motorcycle
  typedef Motorcycle_impl_base<Geom_traits>                   Motorcycle;
  typedef boost::shared_ptr<Motorcycle>                       Motorcycle_ptr;
  typedef std::vector<Motorcycle_ptr>                         Motorcycle_container;

  typedef Motorcycle_priority_queue<Geom_traits>              Motorcycle_PQ;
  typedef Motorcycle_priority_queue_entry<Geom_traits>        Motorcycle_PQE;

  // BGL
  typedef typename Geom_traits::vertex_descriptor             vertex_descriptor;
  typedef typename Geom_traits::halfedge_descriptor           halfedge_descriptor;
  typedef typename Geom_traits::edge_descriptor               edge_descriptor;
  typedef typename Geom_traits::face_descriptor               face_descriptor;
  typedef boost::variant<vertex_descriptor,
                         halfedge_descriptor,
                         face_descriptor>                     descriptor_variant;
  typedef typename Geom_traits::face_iterator                 face_iterator;

  // Collision (collision point, time at collision, foreign mc, foreign time)
  typedef boost::tuple<DEC_it, FT, int, FT>                   Collision;

  // Tracks
  typedef typename Motorcycle::Track                          Track;
  typedef typename Motorcycle::TPC_iterator                   TPC_iterator;

  // face_id, source, time_at_source, destination, time_at_destination
  typedef boost::tuple<std::size_t, DEC_it, FT, DEC_it, FT>   Track_segment;
  typedef std::list<Track_segment>                            Track_segment_container;
  typedef boost::unordered_map<face_descriptor,
                               Track_segment_container>       Track_face_map;
  typedef typename Track_face_map::iterator                   TFM_iterator;
  typedef typename Track_face_map::const_iterator             TFM_const_iterator;

  // Location helper
  typedef typename Motorcycle::Point_or_location              Point_or_location;
  typedef CGAL::internal::P2_or_P3_to_P3<Triangle_mesh>       P2_or_P3_to_P3;
  typedef CGAL::P2_to_P3_VPM<Triangle_mesh>                   AABB_tree_VPM;

  typedef CGAL::AABB_face_graph_triangle_primitive<Triangle_mesh, AABB_tree_VPM>
                                                              AABB_face_graph_primitive;
  typedef CGAL::AABB_traits<K, AABB_face_graph_primitive>     AABB_face_graph_traits;
  typedef CGAL::AABB_tree<AABB_face_graph_traits>             AABB_tree;

  // Access
  const Geom_traits& geom_traits() const { return gt; }

  Triangle_mesh& mesh() { return mesh_; }
  const Triangle_mesh& mesh() const { return mesh_; }

  Motorcycle& motorcycle(const std::size_t id)
  {
    CGAL_precondition(id >= 0 && id < motorcycles.size());
    return *(motorcycles[id]);
  }
  const Motorcycle& motorcycle(const std::size_t id) const
  {
    CGAL_precondition(id >= 0 && id < motorcycles.size());
    return *(motorcycles[id]);
  }
  std::size_t number_of_motorcycles() const { return motorcycles.size(); }

  // Constructor
  Motorcycle_graph(Triangle_mesh& mesh, const Geom_traits& gt = Geom_traits());

  // Functions
  void add_motorcycle(Motorcycle_ptr mc);
  void add_motorcycle(Motorcycle_ptr mc, std::size_t new_id);

  template<typename MotorcycleContainerIterator>
  void add_motorcycles(MotorcycleContainerIterator mit, MotorcycleContainerIterator beyond);

  /// \param fd face in which the segment belongs
  /// \param id the id of the motorcycle
  /// \param s the source of the oriented segment
  /// \param t the target of the oriented segment
  ///
  /// \return iterator in the tracking map
  TFM_iterator add_track_segment_to_map(face_descriptor fd, std::size_t id,
                                        DEC_it s, const FT time_at_s,
                                        DEC_it t, const FT time_at_t);

  // returns point and time at point
  std::pair<DEC_it, FT>
  compute_destination(Motorcycle& mc, const boost::optional<Point_or_location>& input_destination);

  /// \param p first point
  /// \param p_time time at first point
  /// \param q second point
  /// \param q_time time at second point
  ///
  /// \return new point and time at the new point
  std::pair<DEC_it, FT> compute_halving_point(const Motorcycle& mc,
                                              DEC_it p, const FT p_time,
                                              DEC_it q, const FT q_time);

  /// \param p first point
  /// \param p_time time at first point
  /// \param q second point
  /// \param q_time time at second point
  ///
  /// \return new point and time at the new point
  std::pair<DEC_it, FT> compute_middle_point(DEC_it p, const FT p_time,
                                             DEC_it q, const FT q_time);

  bool compute_motorcycle_next_path(Motorcycle& mc);
  void crash_motorcycle(Motorcycle& mc);
  void crash_motorcycles_with_same_source_and_direction();
  void drive_to_closest_target(Motorcycle& mc);

  //@todo return a bool to indicate if we found a new better intersection
  // Collisions between two motorcycles in different faces
  void find_collision_with_foreign_motorcycles(Motorcycle& mc, Collision_information& tc);

  // Below, only the target of the tentative track is on a border
  // ---------------------------------------------------------------------------------
  // collect the different faces in which we seek a collision depending on the location 'dv'
  void find_collision_with_tentative_track_target_on_border(const Motorcycle& mc,
                                                            const descriptor_variant dv,
                                                            Collision_information& tc) const;
  // collect the motorcycles and tracks that we need to seek collisions with in the face 'ffd'
  void find_collision_with_tentative_track_target_on_border(const Motorcycle& mc,
                                                            const descriptor_variant dv,
                                                            const face_descriptor ffd,
                                                            Collision_information& tc) const;
  // discard 'fmc' if it is improper and build the foreign track
  void find_collision_with_live_motorcycle_on_foreign_face(const Motorcycle& mc,
                                                           const descriptor_variant dv,
                                                           const face_descriptor ffd,
                                                           const Motorcycle& fmc,
                                                           Collision_information& tc) const;
  // try to find a collision between the tentative tracks's target and the foreign track
  void find_collision_with_track_on_foreign_face(const Motorcycle& mc,
                                                 const descriptor_variant ct_dv,
                                                 const Track_segment& fmc_track,
                                                 Collision_information& tc) const;
  // ---------------------------------------------------------------------------------

  // Below, both the source and the target of the tentative track are on the same halfedge
  // ---------------------------------------------------------------------------------
  // collect the motorcycles and tracks that we need to seek collisions with in the face 'opposite(hd, mesh)'
  void find_foreign_collision_with_tentative_track_on_border(const Motorcycle& mc,
                                                             const halfedge_descriptor hd,
                                                             Collision_information& tc);
  // discard 'fmc' if it's improper and build the foreign track
  void find_collision_with_live_motorcycle_on_foreign_face(const Motorcycle& mc,
                                                           const halfedge_descriptor hd,
                                                           const Motorcycle& fmc,
                                                           Collision_information& tc) const;
  // distinguish between collinear tracks and foreign tracks with a single extremity on the halfedge
  void find_collision_with_track_on_foreign_face(const Motorcycle& mc,
                                                 const halfedge_descriptor hd,
                                                 const Track_segment& fmc_track,
                                                 const bool is_fmc_moving_on_track,
                                                 Collision_information& tc) const;
  // case of a foreign track and mc's tentative tracks being collinear
  void find_collision_with_collinear_tracks_on_different_faces(const Motorcycle& mc,
                                                               const halfedge_descriptor hd,
                                                               const Track_segment& fmc_track,
                                                               const bool is_fmc_moving_on_track,
                                                               Collision_information& tc) const;
  // case of a foreign track only having a single extremity on the halfedge 'opposite(hd, mesh)'
  void find_collision_with_foreign_track_extremity(const Motorcycle& mc,
                                                   const halfedge_descriptor hd,
                                                   const Motorcycle& fmc,
                                                   const DEC_it foreign_extremity,
                                                   const FT foreign_time_at_collision,
                                                   Collision_information& tc) const;
  // ---------------------------------------------------------------------------------

  // collisions between two motorcycles in the same face
  void find_collision_at_tentative_track_destination(const Motorcycle& mc,
                                                     const Motorcycle& fmc,
                                                     const FT fmc_visiting_time,
                                                     Collision_information& tc) const;
  void find_collision_at_tentative_track_source(const Motorcycle& mc,
                                                const Motorcycle& fmc,
                                                const FT fmc_visiting_time,
                                                Collision_information& tc) const;
  void  find_collision_between_collinear_tracks(const Motorcycle& mc,
                                                const Segment_2& mcs,
                                                const Motorcycle& fmc,
                                                const Track_segment& fmc_track,
                                                const Segment_2& fmcs,
                                                const bool is_fmc_moving_on_track,
                                                Collision_information& tc) const;
  void find_collision_between_tracks(const Motorcycle& mc,
                                     const Segment_2& mcs,
                                     const Motorcycle& fmc,
                                     const Track_segment& fmc_track,
                                     const bool is_fmc_moving_on_track,
                                     Collision_information& tc) const;
  void find_collision_with_complete_track(Motorcycle& mc,
                                          const Segment_2& mcs,
                                          const Track_segment& fmc_track,
                                          Collision_information& tc);
  void find_collision_with_live_motorcycle(Motorcycle& mc,
                                           const Segment_2& mcs,
                                           const Motorcycle& fmc,
                                           Collision_information& tc);

  /// \return collision point (if any), time at the collision for `mc`, id of
  ///         the foreign intersecting motorcycle, time at the collision for
  ///         the foreign motorcycle.
  Collision_information find_collision(Motorcycle& mc);

  void generate_enclosing_face();
  bool has_motorcycle_reached_crashing_point(const Motorcycle& mc) const;
  bool has_motorcycle_reached_final_destination(const Motorcycle& mc) const;
  void initialize_motorcycles();
  bool is_aabb_tree_needed() const;
  bool is_motorcycle_position_blocked(const Motorcycle& mc) const;
  Face_location locate(const Point& p, const AABB_tree& tree, const AABB_tree_VPM vpm) const;

  template<typename MotorcycleContainerIterator>
  void trace_graph(MotorcycleContainerIterator mit, MotorcycleContainerIterator beyond);
  void treat_collision(Motorcycle& mc, const Collision_information& collision);
  void treat_collision(Motorcycle& mc, DEC_it collision_point, const FT time_at_collision_point,
                       Motorcycle& fmc, DEC_it foreign_collision_point, const FT foreign_time_at_collision_point);

  std::pair<DEC_it, bool>
  try_to_snap_location_to_existing_point(const Face_location& location, const Point& p, const FT tolerance) const;

  void visit_point(Motorcycle& mc, Motorcycle& fmc, DEC_it collision,
                   FT time_at_collision, FT foreign_time_at_collision, const FT tolerance);

  // Post-tracing checks
  bool is_valid() const;

  // Output
  void output_all_dictionary_points() const;
  void output_motorcycles_sources_and_destinations() const;

private:
  Geom_traits gt;

  Dictionary points; // All the points that will be used throughout the algorithm
  Motorcycle_container motorcycles;
  Motorcycle_PQ motorcycle_pq; // motorcycle priority queue

  bool using_enclosing_bbox; // indicates whether a mesh is passed input
  Triangle_mesh& mesh_; // not 'const' in case we need to create it

  // map to store the completed tracks of the motorcycles for each face of the mesh
  Track_face_map track_face_map;
};

// -----------------------------------------------------------------------------
template<typename MotorcycleGraphTraits>
Motorcycle_graph<MotorcycleGraphTraits>::
Motorcycle_graph(Triangle_mesh& mesh, const Geom_traits& gt)
  :
    gt(gt),
    points(),
    motorcycles(),
    motorcycle_pq(),
    using_enclosing_bbox(false),
    mesh_(mesh),
    track_face_map()
{
  if(num_vertices(mesh_) == 0)
  {
    std::cerr << " Warning: empty mesh in input" << std::endl;
    using_enclosing_bbox = true;
  }
  else
  {
    // Input must be a mesh with triangle faces
    CGAL_precondition(CGAL::is_triangle_mesh(mesh_));
  }

  //@tmp disabled while I find out what to do with the "no mesh provided option"
  // The issue is that the points are identified by a location described with barycentric
  // coordinates. I guess, I could generate a bbox, then a triangle that includes
  // the box ? Pretty ugly, though...
  CGAL_assertion(!using_enclosing_bbox);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
add_motorcycle(Motorcycle_ptr mc)
{
  return add_motorcycle(mc, motorcycles.size());
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
add_motorcycle(Motorcycle_ptr mc, std::size_t new_id)
{
  mc->set_id(new_id);

  boost::optional<Point_or_location>& destination_point = mc->input_destination();
  boost::optional<Vector>& direction = mc->direction();

  if(destination_point == boost::none && direction == boost::none)
    std::cerr << "Warning: Neither destination nor direction are provided." << std::endl;

  motorcycles.push_back(mc);
}

template<typename MotorcycleGraphTraits>
template<typename MotorcycleContainerIterator>
void
Motorcycle_graph<MotorcycleGraphTraits>::
add_motorcycles(MotorcycleContainerIterator mit, MotorcycleContainerIterator beyond)
{
  if(!motorcycles.empty())
    std::cerr << "Warning: motorcycle container was not empty before calling add_motorcycles()" << std::endl;

  motorcycles.reserve(motorcycles.size() + std::distance(mit, beyond));

  // unique motorcycle id, starting at motorcycles.size() in case we have
  // already added some motorcycles
  std::size_t counter = motorcycles.size();

  while(mit != beyond)
    add_motorcycle(*mit++, counter++);
}

template<typename MotorcycleGraphTraits>
typename Motorcycle_graph<MotorcycleGraphTraits>::TFM_iterator
Motorcycle_graph<MotorcycleGraphTraits>::
add_track_segment_to_map(face_descriptor fd, std::size_t id,
                         DEC_it s, const FT time_at_s,
                         DEC_it t, const FT time_at_t)
{
  CGAL_precondition(s->location().first == fd);
  CGAL_precondition(t->location().first == fd);
  CGAL_precondition(id >= 0 && id < motorcycles.size());
  CGAL_precondition(time_at_s <= time_at_t);

  Track_segment tr = boost::make_tuple(id, s, time_at_s, t, time_at_t);
  Track_segment_container l;
  l.push_back(tr);

  std::pair<typename Motorcycle_graph<MotorcycleGraphTraits>::TFM_iterator, bool>
    is_insert_success = track_face_map.insert(std::make_pair(fd, l));

  if(!is_insert_success.second)
    is_insert_success.first->second.push_back(tr);

  return is_insert_success.first;
}

template<typename MotorcycleGraphTraits>
std::pair<typename Motorcycle_graph<MotorcycleGraphTraits>::DEC_it,
          typename Motorcycle_graph<MotorcycleGraphTraits>::FT>
Motorcycle_graph<MotorcycleGraphTraits>::
compute_destination(Motorcycle& mc,
                    const boost::optional<Point_or_location>& input_destination)
{
  // At the start of this function, mc.source() is already initialized
  CGAL_precondition(mc.source() != DEC_it());

  DEC_it destination;
  FT time_at_source = mc.current_time(), time_at_destination;

  if(input_destination == boost::none) // A destination was not provided
  {
    boost::tuple<bool, DEC_it, DEC_it, FT, bool> res =
      mc.compute_next_destination(points, mesh_);

    if(!res.template get<0>())
    {
      // Couldn't find an initial destination ==> the motorcycle instantly crashes
      mc.set_destination_finality(true);
      return std::make_pair(mc.source(), time_at_source);
    }
    else // A destination was found
    {
      // The location algorithm might change the source to ensure that the
      // source and destination are on the same face
      if(mc.source() != res.template get<1>())
      {
        std::cerr << "Source has changed!" << std::endl
                  << "Previously: " << std::endl << *(mc.source()) << std::endl
                  << "Now: " << std::endl << *(res.template get<1>()) << std::endl;

        // The source change must only be a change of Face_location, not of actual position
        CGAL_assertion(mc.source()->point() == res.template get<1>()->point());

        mc.source() = res.template get<1>();
        mc.current_position() = mc.source();
        CGAL_assertion(mc.source()->has_motorcycle(mc.id(), time_at_source));
      }

      destination = res.template get<2>();
      time_at_destination = res.template get<3>();

      mc.set_destination_finality(res.template get<4>());
    }

    mc.input_destination() = destination->point();
    destination->add_motorcycle(mc.id(), time_at_destination);
  }
  else // The destination is known, the time of arrival must be computed
  {
    Face_location source_location = mc.source()->location();
    Face_location destination_location;

    if(input_destination->which() == 0) // A 'Point' was provided in input
    {
      const Point& input_destination_point = boost::get<Point>(*input_destination);
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Input destination point: " << input_destination_point << std::endl;
#endif

      // If the source is on the border of the mesh, we must find a common face
      if(CGAL::Polygon_mesh_processing::is_on_face_border(source_location, mesh_))
      {
        CGAL::Polygon_mesh_processing::locate_in_common_face(
          input_destination_point, source_location, destination_location, mesh_);

        // 'source_location' might have changed to find a common face
        if(source_location != mc.source()->location())
        {
          std::cerr << "Warning: source has changed!" << std::endl;
          const Point input_source_point = mc.source()->point();

          std::pair<DEC_it, bool> new_source = points.insert(source_location,
                                                             input_source_point,
                                                             mc.id(), time_at_source,
                                                             mesh_);
          mc.source() = new_source.first;
          mc.current_position() = new_source.first;
          CGAL_assertion(mc.source()->has_motorcycle(mc.id(), time_at_source));
        }
      }
      else // The source is located strictly within a face
      {
        // Must ensure that source and destination are on the same face
        destination_location = CGAL::Polygon_mesh_processing::locate(
                                 source_location.first, input_destination_point, mesh_);
      }
    }
    else // A 'Face_location' was provided in input
    {
      //@todo move from which() to pointer casts (across the whole file)
      CGAL_assertion(input_destination->which() == 1);
      destination_location = boost::get<Face_location>(*input_destination);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Input source location fd: " << destination_location.first
                << "bc: [" << destination_location.second[0] << " "
                           << destination_location.second[1] << " "
                           << destination_location.second[2] << "]" << std::endl;
#endif

      // source and destination must live in a common face
      if(source_location.first != destination_location.first)
      {
        CGAL::Polygon_mesh_processing::locate_in_common_face(
          source_location, destination_location, mesh_);

        // 'source_location' might have changed to find a common face
        if(source_location != mc.source()->location())
        {
          std::cerr << "Warning: source has changed!" << std::endl;
          const Point input_source_point = mc.source()->point();

          std::pair<DEC_it, bool> new_source = points.insert(source_location,
                                                             input_source_point,
                                                             mc.id(), time_at_source,
                                                             mesh_);
          mc.source() = new_source.first;
          mc.current_position() = new_source.first;
        }
      }
    }

    std::pair<DEC_it, bool> destination_entry = points.insert(destination_location, mesh_);
    destination = destination_entry.first;

    const FT speed = mc.speed();
    const Point source_point = mc.source()->point();
    const Point destination_point = destination->point();

    time_at_destination = time_at_source +
      CGAL::sqrt(CGAL::squared_distance(source_point,
                                        destination_point)) / speed;

    destination->add_motorcycle(mc.id(), time_at_destination);
  }

  return std::make_pair(destination, time_at_destination);
}

template<typename MotorcycleGraphTraits>
std::pair<typename Motorcycle_graph<MotorcycleGraphTraits>::DEC_it,
          typename Motorcycle_graph<MotorcycleGraphTraits>::FT>
Motorcycle_graph<MotorcycleGraphTraits>::
compute_halving_point(const Motorcycle& m, DEC_it p, const FT p_time,
                                           DEC_it q, const FT q_time)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "***/***" << std::endl;
  std::cout << " Computing halving point on motorcycle #" << m.id() << "'s track."
            << " Points are:" << std::endl << "  " << *p << std::endl
                                           << "  " << *q << std::endl;
#endif
  CGAL_precondition(p != q);
  CGAL_precondition(p->location().first == q->location().first);

#ifdef CGAL_MOTORCYCLE_GRAPH_USE_ADVANCED_HALVING
  // interface with the halving data structure @todo
#else
  return compute_middle_point(p, p_time, q, q_time);
#endif
}

template<typename MotorcycleGraphTraits>
std::pair<typename Motorcycle_graph<MotorcycleGraphTraits>::DEC_it,
          typename Motorcycle_graph<MotorcycleGraphTraits>::FT>
Motorcycle_graph<MotorcycleGraphTraits>::
compute_middle_point(DEC_it p, const FT p_time, DEC_it q, const FT q_time)
{
  if(p->location().first != q->location().first)
  {
    std::cerr << "Error: middle point computation with different faces" << std::endl;
    // asserting because using p.loc().first is too dangerous if q is not guaranteed
    // to be on p's face
    CGAL_assertion(false);
  }

  const Barycentric_coordinates& p_coords = p->location().second;
  const Barycentric_coordinates& q_coords = q->location().second;

  Barycentric_coordinates middle_coords = CGAL::make_array(0.5*(p_coords[0] + q_coords[0]),
                                                           0.5*(p_coords[1] + q_coords[1]),
                                                           0.5*(p_coords[2] + q_coords[2]));
  Face_location middle_loc = std::make_pair(p->location().first, middle_coords);
  const FT time_at_r = 0.5 * (p_time + q_time);
  std::pair<DEC_it, bool> entry = points.insert(middle_loc, mesh_);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "  New middle point: (" << entry.first->point()
                                       << ") at time: " << time_at_r << std::endl;
  std::cout << "    Location: " << p->location().first
            << " bc: " << middle_coords[0] << " "
                       << middle_coords[1] << " "
                       << middle_coords[2] << std::endl;
#endif

  return std::make_pair(entry.first, time_at_r);
}

template<typename MotorcycleGraphTraits>
bool
Motorcycle_graph<MotorcycleGraphTraits>::
compute_motorcycle_next_path(Motorcycle& mc)
{
  boost::tuple<bool, DEC_it, DEC_it, FT, bool> next_path =
    mc.compute_next_destination(points, mesh_);

  if(!next_path.template get<0>()) // couldn't find a next path
    return false;

  const DEC_it& next_source = next_path.template get<1>();
  const DEC_it& next_destination = next_path.template get<2>();
  const FT time_at_next_destination = next_path.template get<3>();
  const bool is_destination_final = next_path.template get<4>();

  // If 'next source' is different from the current position, it should only
  // be a location change, not a position change
  CGAL_assertion_code(if(next_source != mc.current_position()))
  CGAL_assertion(mc.current_position()->is_sibling(next_source->location()));

  mc.source() = next_source;
  mc.time_at_source() = mc.current_time();
  mc.current_position() = mc.source();

  mc.destination() = next_destination;
  mc.set_destination_finality(is_destination_final);

  if(next_source != next_destination)
  {
    // No need to add the same information twice
    mc.add_target(next_destination, time_at_next_destination);
    next_destination->add_motorcycle(mc.id(), time_at_next_destination);
  }

  // Add the next source as target, even if it is equal to the current position.
  // This allows the new path to be treated with highest priority.
  mc.add_target(next_source, mc.current_time());

  return true;
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
crash_motorcycle(Motorcycle& mc)
{
  if(mc.is_crashed())
    return;

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~X" << std::endl;
  std::cout << "Crashing " << mc;
#endif

  mc.clear_targets();
  mc.crash();
  motorcycle_pq.erase(mc);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
crash_motorcycles_with_same_source_and_direction()
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "Checking for motorcycles with same sources and directions" << std::endl;
#endif

  // @todo handle motorcycles starting from the same point & with same directions
  // but not on the same face...

  // brute force, for now
  // A smarter version is to sort motorcycles by direction (slope),
  // and check for consecutive entries @todo
  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t mc_id=0; mc_id<number_of_motorcycles; ++mc_id)
  {
    Motorcycle& mc = motorcycle(mc_id);

    if(mc.source() == mc.destination() || mc.is_crashed())
      continue;

    for(std::size_t fmc_id=0; fmc_id<number_of_motorcycles; ++fmc_id)
    {
      Motorcycle& fmc = motorcycle(fmc_id);

      // Note: not ignoring crashed motorcycles in case of > 2 motorcycles with
      // same source and destination

      if(fmc.id() == mc.id() ||
         fmc.source() == fmc.destination() || // a degenerate track does not block anything
         mc.source() != fmc.source()) // must have identical sources
        continue;

      CGAL_assertion(mc.current_location().first == fmc.current_location().first);

      Point_2 bcs_mc_s(mc.source()->location().second[0], mc.source()->location().second[1]);
      Point_2 bcs_mc_d(mc.destination()->location().second[0], mc.destination()->location().second[1]);
      Point_2 bcs_fmc_d(fmc.destination()->location().second[0], fmc.destination()->location().second[1]);
      Point_2 bcs_fmc_s(fmc.source()->location().second[0], fmc.source()->location().second[1]);

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
      // Add some tolerance to the definition of "collinearity"
      Vector_2 bcs_mc_v(bcs_mc_s, bcs_mc_d);
      Vector_2 bcs_fmc_v(bcs_fmc_s, bcs_fmc_d);

      FT mc_v_n = bcs_mc_v * bcs_mc_v;
      FT fmc_v_n = bcs_fmc_v * bcs_fmc_v;

      FT sp = gt.compute_scalar_product_2_object()(bcs_mc_v, bcs_fmc_v);

      std::cout << "SProduct: " << sp << std::endl;
      std::cout << "SProduct normalized " << sp * sp / (fmc_v_n * mc_v_n ) << std::endl;

      // @fixme hardcoded value, but numeric_limits::eps not small enough
      // due to the multiple intermediary computations
      if( CGAL::abs( 1 - sp * sp / (fmc_v_n * mc_v_n) ) < 1e-15)
      {
        std::cout << "Crashing degenerate motorcycles: "
                  << mc.id() << " and " << fmc.id() << std::endl;
        crash_motorcycle(mc);
        crash_motorcycle(fmc);
        break;
      }
#else
      // only aligned tracks block one another
      if(!gt.collinear_2_object()(bcs_mc_s, // == fmc.source()->point()
                                  bcs_mc_d, bcs_fmc_d))
        continue;

      std::cout << "Collinear tracks with the same source" << std::endl;

      // Moving away from each other from the same point is allowed.
      if(gt.angle_2_object()(bcs_mc_s, bcs_mc_d, bcs_fmc_s, bcs_fmc_d) == CGAL::ACUTE)
      {
        std::cout << "Crashing degenerate motorcycles: "
                  << mc.id() << " and " << fmc.id() << std::endl;
        crash_motorcycle(mc);
        crash_motorcycle(fmc);
        break;
      }
#endif
    }
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
drive_to_closest_target(Motorcycle& mc)
{
  CGAL_assertion(!mc.is_crashed());
  CGAL_assertion(!mc.targets().empty());

  DEC_it closest_target = mc.closest_target();

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~>" << std::endl;
    std::cout << "Driving " << mc;
#endif

  mc.current_position() = closest_target;
  mc.current_time() = mc.targets().begin()->second;
  mc.track().insert(std::make_pair(closest_target, mc.current_time()));
  mc.remove_closest_target_from_targets();

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "  now at: (" << mc.current_position()->point() << ")" << std::endl;
#endif
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_foreign_motorcycles(Motorcycle& mc, Collision_information& tc)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "~~~~~~~~~X ?" << std::endl;
  std::cout << "Checking for collisions on motorcycle #" << mc.id() << "'s track"
            << " with foreign faces" << std::endl;
#endif
  namespace PMP = CGAL::Polygon_mesh_processing;

  // We can look only at collisions with the closest target, except if the whole
  // segment "position -- closest_target" is on the same border halfedge.

  descriptor_variant target_dv = PMP::get_descriptor_from_location(
    mc.closest_target()->location(), mesh_);

  // If the target is not on the border, there's simply nothing to do because
  // we don't care about the intersections at the source.
  if(target_dv.which() == 2)
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << " Tentative track's target is not on border" << std::endl;
#endif
    return;
  }

  descriptor_variant source_dv = PMP::get_descriptor_from_location(
      mc.current_position()->location(), mesh_);

  if(source_dv.which() == 2) // tentative track's source is not on a border
  {
    // Small skip: if we have already found an intersection strictly within the face,
    // there's no point to check adjacent faces, since the intersection will be
    // at a later time.
    if(tc.time_at_closest_collision < mc.time_at_closest_target())
      return;

    find_collision_with_tentative_track_target_on_border(mc, target_dv, tc);
  }
  else // tentative track's source and closest target are on a border
  {
    // check if source and targets lie on the same halfedge
    halfedge_descriptor hd = halfedge(mc.current_location().first, mesh_), done(hd);
    bool are_on_same_halfedge = false;

    do
    {
      if(PMP::is_on_halfedge(mc.current_position()->location(), hd, mesh_) &&
         PMP::is_on_halfedge(mc.closest_target()->location(), hd, mesh_))
      {
        are_on_same_halfedge = true;
        break;
      }

      hd = next(hd, mesh_);
    } while(hd != done);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "Tentative track on the same halfedge: " << are_on_same_halfedge << std::endl;
#endif

    if(are_on_same_halfedge)
    {
      // same halfedge, means that we must consider the full segment and look
      // for intersections in the opposite face
      find_foreign_collision_with_tentative_track_on_border(mc, hd, tc);

      if(target_dv.which() == 0) // closest target is on a vertex
      {
        // need to also check the incident faces at 'vd'...
        find_collision_with_tentative_track_target_on_border(mc, target_dv, tc);
      }
    }
    else // not on the same halfedge, only look at the destination
    {
      find_collision_with_tentative_track_target_on_border(mc, target_dv, tc);
    }
  }
}

// Below, only the target of the tentative track is on a border
// ---------------------------------------------------------------------------------
template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_tentative_track_target_on_border(const Motorcycle& mc,
                                                     const descriptor_variant dv,
                                                     Collision_information& tc) const
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤ Find collision with tentative track target of motorcycle #" << mc.id() << " on border" << std::endl;
#endif

  CGAL_precondition(dv == CGAL::Polygon_mesh_processing::
                    get_descriptor_from_location(mc.closest_target()->location(), mesh_));

  if(dv.which() == 0) // mc's closest target is on a vertex
  {
    vertex_descriptor vd = boost::get<vertex_descriptor>(dv);

    // check all incident faces at 'vd' and intersections at vd
    halfedge_descriptor hd = halfedge(vd, mesh_);
    BOOST_FOREACH(face_descriptor ffd, CGAL::faces_around_target(hd, mesh_))
    {
      if(ffd == mc.current_location().first ||
         ffd == boost::graph_traits<Triangle_mesh>::null_face())
        continue;

      find_collision_with_tentative_track_target_on_border(mc, dv, ffd, tc);
    }
  }
  else // mc's closest target is on a halfedge
  {
    CGAL_assertion(dv.which() == 1);
    halfedge_descriptor hd = boost::get<halfedge_descriptor>(dv);

    if(is_border(edge(hd, mesh_), mesh_))
      return;

    // check opposite face for intersection at the mc.closest_target()
    face_descriptor ffd = face(opposite(hd, mesh_), mesh_);
    find_collision_with_tentative_track_target_on_border(mc, dv, ffd, tc);
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_tentative_track_target_on_border(const Motorcycle& mc,
                                                     const descriptor_variant dv,
                                                     const face_descriptor ffd,
                                                     Collision_information& tc) const
{
  CGAL_precondition(ffd != boost::graph_traits<Triangle_mesh>::null_face());
  CGAL_precondition(mc.current_location().first != ffd);

  // Step 1: check complete tracks
  TFM_const_iterator it = track_face_map.find(ffd);
  if(it != track_face_map.end())
  {
    const Track_segment_container& face_tracks = it->second;

    typename Track_segment_container::const_iterator tl_it = face_tracks.begin();
    typename Track_segment_container::const_iterator tl_end = face_tracks.end();
    for(; tl_it!=tl_end; ++tl_it)
    {
      const Track_segment& track = *tl_it;
      find_collision_with_track_on_foreign_face(mc, dv, track, tc);

      if(tc.add_intersection_without_moving_motorcycles)
        return;
    }
  }

  // Step 2: check incomplete tracks (path of a motorcycle currently moving in the same face)
  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t fmc_id = 0; fmc_id<number_of_motorcycles; ++fmc_id)
  {
    const Motorcycle& fmc = motorcycle(fmc_id);
    find_collision_with_live_motorcycle_on_foreign_face(mc, dv, ffd, fmc, tc);

    if(tc.add_intersection_without_moving_motorcycles)
      return;
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_live_motorcycle_on_foreign_face(const Motorcycle& mc,
                                                    const descriptor_variant dv,
                                                    const face_descriptor ffd,
                                                    const Motorcycle& fmc,
                                                    Collision_information& tc) const
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤ Checking for foreign intersection with live motorcycle #" << fmc.id() << std::endl;
#endif
  CGAL_precondition(ffd != boost::graph_traits<Triangle_mesh>::null_halfedge());
  CGAL_precondition(mc.current_location().first != ffd);

  if(// the foreign motorcycle must be in the foreign face 'ffd'
     fmc.current_location().first != ffd ||
     // the foreign motorcycle must be in motion
     fmc.is_crashed())
  {
    std::cout << " ignoring 'fmc' in foreign face... " << std::endl;
    std::cout << "  > motorcycles #" << mc.id() << " and #" << fmc.id() << std::endl;
    std::cout << "  > faces: " << fmc.current_location().first << " and " << fmc.current_location().first << std::endl;
    std::cout << "  > crashed status: " << fmc.is_crashed() << std::endl;
    return;
  }

  CGAL_assertion(fmc.id() != mc.id());

  Track_segment fmc_track = boost::make_tuple(fmc.id(),
                                              fmc.source(), fmc.time_at_source(),
                                              fmc.closest_target(), fmc.time_at_closest_target());

  return find_collision_with_track_on_foreign_face(mc, dv, fmc_track, tc);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_track_on_foreign_face(const Motorcycle& mc,
                                          const descriptor_variant ct_dv,
                                          const Track_segment& fmc_track,
                                          Collision_information& tc) const
{
  namespace PMP = CGAL::Polygon_mesh_processing;

  const std::size_t fmc_id = fmc_track.template get<0>();

  const Motorcycle& fmc = motorcycle(fmc_id);
  const DEC_it fmc_track_source = fmc_track.template get<1>();
  const DEC_it fmc_track_destination = fmc_track.template get<3>();

  const face_descriptor ffd = fmc_track_source->location().first;
  CGAL_assertion(ffd == fmc_track_destination->location().first);

  const DEC_it ct = mc.closest_target();
  const Face_location& ct_in_ffd = ct->sibling(ffd);

  // All locations must now be on the same face
  CGAL_assertion(fmc_track_source->location().first == ct_in_ffd.first);
  CGAL_assertion(fmc_track_destination->location().first == ct_in_ffd.first);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤¤ Checking collision with single point on border "
            << "of foreign motorcycle #" << fmc_id << std::endl;
  std::cout << " + closest target: " << &*ct << std::endl << *ct << std::endl;
  std::cout << " + location in foreign face: " << " " << ct_in_ffd.first << " bc: "
                                               << ct_in_ffd.second[0] << " "
                                               << ct_in_ffd.second[1] << " "
                                               << ct_in_ffd.second[2] << std::endl;
  std::cout << " + source: " << &*fmc_track_source << std::endl << *fmc_track_source << std::endl;
  std::cout << " + target: " << &*fmc_track_destination << std::endl << *fmc_track_destination << std::endl;
#endif

  const FT time_at_collision = mc.time_at_closest_target();
  const FT time_at_fmc_track_source = fmc_track.template get<2>();
  const FT time_at_fmc_track_destination = fmc_track.template get<4>();

  // @todo rework has_motorcycle to return pair<FT, bool>
  FT foreign_visiting_time;
  if(ct->has_motorcycle(fmc.id(), time_at_fmc_track_source,
                        time_at_fmc_track_destination, foreign_visiting_time))
  {
    // @todo use a generalized version of 'find_collision_at_tentative_track_destination()'

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "  /!\\ Tentative path collides with track on foreign face of motorcycle #: " << fmc.id()
              << " at the closest target. Time: " << time_at_collision << std::endl;
#endif

    if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_visiting_time))
    {
      tc.reset();
      tc.is_closest_collision_already_in_dictionary = true;
      tc.closest_collision = mc.closest_target();
      tc.time_at_closest_collision = time_at_collision;

      tc.fmc_id = fmc.id();
      tc.is_foreign_motorcycle_in_different_face = true;
      tc.foreign_motorcycle_face = ffd;
      tc.foreign_time_at_closest_collision = foreign_visiting_time;
    }
  }
  else if(ct_dv.which() == 0)
  {
    // If ct_dv.which() == 0 (the closest target is on a vertex_descriptor), then
    // the only possible intersection is with 'fmc_track_source' or 'fmc_track_destination'
    // and it will (should) have been found with the check above if it exists.
    return;
  }
  else if(ct_dv.which() == 1)
  {
    halfedge_descriptor hd = boost::get<halfedge_descriptor>(ct_dv);
    // Need to check that the track [fmc_track_source, fmc_track_destination]
    // does not contain mc.closest_target()

    // If the extremities of the foreign track are not on a border halfedge,
    // then there can't be an intersection with a point on the border (except
    // for source or destination, which have been checked above)

    // check if source and targets lie on the same halfedge
    // @todo use find_common_entity() from PMP::locate.h
    halfedge_descriptor cfhd = halfedge(ffd, mesh_), done(cfhd);
    bool are_on_same_halfedge = false;

    do
    {
      if(PMP::is_on_halfedge(fmc_track_source->location(), cfhd, mesh_) &&
         PMP::is_on_halfedge(fmc_track_destination->location(), cfhd, mesh_))
      {
        are_on_same_halfedge = true;
        break;
      }

      cfhd = next(cfhd, mesh_);
    } while(cfhd != done);

    if(!are_on_same_halfedge)
      return;

    // 'hd' is in the non-foreign face, and we want the halfedge in the foreign face
    halfedge_descriptor opp_hd = opposite(hd, mesh_);

    if(cfhd != opp_hd)
      return;

    // We are now in the configuration of 'mc' having a single point on a border,
    // and the foreign track is on the opposite border

    const Point_2 s = gt.construct_point_2_object()(fmc_track_source->location().second[0],
                                                    fmc_track_source->location().second[1]);
    const Point_2 t = gt.construct_point_2_object()(fmc_track_destination->location().second[0],
                                                    fmc_track_destination->location().second[1]);
    const Point_2 ct2 = gt.construct_point_2_object()(ct_in_ffd.second[0],
                                                      ct_in_ffd.second[1]);

    std::cout << "s-ct2-t: " << s << " || " << ct2 << " || " << t << std::endl;

    CGAL_assertion(s != ct2 && t != ct2);

    // Below might fail due to numerical errors, but it is supposed to be 'true'
#ifdef CGAL_POLYLINE_TRACING_ENABLE_RIGOROUS_PRECONDITIONS
    CGAL_assertion(gt.collinear_2_object()(s, ct2, t));
#endif

    // Check if the closest target is in between the source and the destination
    if(!gt.collinear_are_strictly_ordered_along_line_2_object()(s, ct2, t))
      return;

    // From here on, 'ct2' is strictly in between 's' and 't'

    // No choice but to compute the foreign time
    const FT time_at_fmc_track_source = fmc_track.template get<2>();
    const FT foreign_time_at_collision = time_at_fmc_track_source +
      CGAL::sqrt(CGAL::squared_distance(fmc_track_source->point(),
                                         mc.closest_target()->point())) / fmc.speed();

    if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
    {
      tc.reset();
      tc.is_closest_collision_already_in_dictionary = true;
      tc.closest_collision = mc.closest_target();
      tc.time_at_closest_collision = time_at_collision;

      tc.fmc_id = fmc_id;
      tc.is_foreign_motorcycle_in_different_face = true;
      tc.foreign_motorcycle_face = ffd;
      tc.foreign_time_at_closest_collision = foreign_time_at_collision;
    }
  }
  else if(ct_dv.which() == 2)
  {
    // If ct_dv.which() == 2, we are not on a border and we should not be here...
    CGAL_assertion(false);
  }
}
// ---------------------------------------------------------------------------------

// Below, both the source and the target of the tentative track are on the same halfedge
// ---------------------------------------------------------------------------------
template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_foreign_collision_with_tentative_track_on_border(const Motorcycle& mc,
                                                      const halfedge_descriptor hd,
                                                      Collision_information& tc)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤ Checking collision with tentative track on border" << std::endl;
#endif

  namespace PMP = CGAL::Polygon_mesh_processing;

  const halfedge_descriptor opp_hd = opposite(hd, mesh_);
  if(is_border(opp_hd, mesh_))
    return;

  const face_descriptor ffd = face(opp_hd, mesh_);

  // Step 1: check complete tracks
  TFM_const_iterator it = track_face_map.find(ffd);
  if(it != track_face_map.end())
  {
    const Track_segment_container& face_tracks = it->second;

    typename Track_segment_container::const_iterator tl_it = face_tracks.begin();
    typename Track_segment_container::const_iterator tl_end = face_tracks.end();
    for(; tl_it!=tl_end; ++tl_it)
    {
      const Track_segment& track = *tl_it;
      find_collision_with_track_on_foreign_face(mc, hd, track,
                                                false /*is_fmc_moving_on_track*/,
                                                tc);

      if(tc.add_intersection_without_moving_motorcycles)
        return;
    }
  }

  // Step 2: check incomplete tracks (path of a motorcycle currently moving in the same face)
  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t fmc_id = 0; fmc_id<number_of_motorcycles; ++fmc_id)
  {
    Motorcycle& fmc = motorcycle(fmc_id);
    find_collision_with_live_motorcycle_on_foreign_face(mc, hd, fmc, tc);

    // @todo make 'add_intersection_without_moving_motorcycles' not as ugly! (return an enum in find_collision)
    if(tc.add_intersection_without_moving_motorcycles)
      return;
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_live_motorcycle_on_foreign_face(const Motorcycle& mc,
                                                    const halfedge_descriptor hd,
                                                    const Motorcycle& fmc,
                                                    Collision_information& tc) const
{
  const face_descriptor ffd = face(opposite(hd, mesh_), mesh_);
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤ Checking for foreign intersection with live motorcycle #" << fmc.id()
            << " in foreign face: " << ffd << std::endl;
#endif

  CGAL_precondition(!is_border(edge(hd, mesh_), mesh_));
  CGAL_precondition(mc.current_location().first != ffd);

  if(// the foreign motorcycle must be in the foreign face 'ffd'
     fmc.current_location().first != ffd ||
     // the foreign motorcycle must be in motion
     fmc.is_crashed())
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << " ignoring 'fmc' in foreign face... " << std::endl;
    std::cout << "  > motorcycles #" << mc.id() << " and #" << fmc.id() << std::endl;
    std::cout << "  > faces: " << mc.current_location().first << " and " << fmc.current_location().first << std::endl;
    std::cout << "  > crashed status: " << fmc.is_crashed() << std::endl;
#endif
    return;
  }

  CGAL_assertion(fmc.id() != mc.id());

  Track_segment fmc_track = boost::make_tuple(fmc.id(),
                                              fmc.source(), fmc.time_at_source(),
                                              fmc.closest_target(), fmc.time_at_closest_target());

  return find_collision_with_track_on_foreign_face(mc, hd, fmc_track,
                                                   true /*is_fmc_moving_on_track*/,
                                                   tc);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_track_on_foreign_face(const Motorcycle& mc,
                                          const halfedge_descriptor hd,
                                          const Track_segment& fmc_track,
                                          const bool is_fmc_moving_on_track,
                                          Collision_information& tc) const
{
  const std::size_t fmc_id = fmc_track.template get<0>();
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤¤ Checking collision with tentative track on border "
            << "and foreign motorcycle #" << fmc_id << std::endl;
#endif
  namespace PMP = CGAL::Polygon_mesh_processing;

  CGAL_precondition(!is_border(edge(hd, mesh_), mesh_));

  // @todo fix target/destination across the whole file

  const Motorcycle& fmc = motorcycle(fmc_id);
  const DEC_it fmc_track_source = fmc_track.template get<1>();
  const DEC_it fmc_track_destination = fmc_track.template get<3>();

  const halfedge_descriptor opp_hd = opposite(hd, mesh_);

  bool is_fts_on_opp_hd = PMP::is_on_halfedge(fmc_track_source->location(), opp_hd, mesh_);
  bool is_ftd_on_opp_hd = PMP::is_on_halfedge(fmc_track_destination->location(), opp_hd, mesh_);

  if(is_fts_on_opp_hd)
  {
    if(is_ftd_on_opp_hd)
    {
      // foreign track is a subset (or the whole) of 'opp_hd'
      find_collision_with_collinear_tracks_on_different_faces(mc, hd, fmc_track,
                                                              is_fmc_moving_on_track, tc);
    }
    else // is_fts_on_opp_hd && !is_ftd_on_opp_hd
    {
      // only possible intersection is at the source
      const DEC_it fmc_track_source = fmc_track.template get<1>();
      const FT time_at_fmc_track_source = fmc_track.template get<2>();
      find_collision_with_foreign_track_extremity(mc, hd, fmc, fmc_track_source,
                                                  time_at_fmc_track_source, tc);
    }
  }
  else if(is_ftd_on_opp_hd) // !is_fts_on_opp_hd && is_ftd_on_opp_hd
  {
    // only possible intersection is at the destination
    const DEC_it fmc_track_destination = fmc_track.template get<3>();
    const FT time_at_fmc_track_destination = fmc_track.template get<4>();
    find_collision_with_foreign_track_extremity(mc, hd, fmc, fmc_track_destination,
                                                time_at_fmc_track_destination, tc);
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_collinear_tracks_on_different_faces(const Motorcycle& mc,
                                                        const halfedge_descriptor hd,
                                                        const Track_segment& fmc_track,
                                                        const bool is_fmc_moving_on_track,
                                                        Collision_information& tc) const
{
  const std::size_t fmc_id = fmc_track.template get<0>();
  const Motorcycle& fmc = motorcycle(fmc_id);
  const DEC_it fmc_track_source = fmc_track.template get<1>();
  const DEC_it fmc_track_destination = fmc_track.template get<3>();

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤¤¤ Find collision between collinear tracks of motorcycles #"
            << mc.id() << " and #" << fmc.id() << std::endl;
  std::cout << "   foreign track: " << std::endl << *fmc_track_source << std::endl
                                                 << *fmc_track_destination << std::endl;
#endif

  namespace PMP = CGAL::Polygon_mesh_processing;

  CGAL_precondition(PMP::is_on_halfedge(mc.current_position()->location(), hd, mesh_));
  CGAL_precondition(PMP::is_on_halfedge(mc.closest_target()->location(), hd, mesh_));

  const halfedge_descriptor opp_hd = opposite(hd, mesh_);
  CGAL_precondition(!is_border(opp_hd, mesh_));
  const face_descriptor ffd = face(opp_hd, mesh_);

  const Face_location& cp_in_ffd = mc.current_position()->sibling(ffd);
  const Face_location& ct_in_ffd = mc.closest_target()->sibling(ffd);

  const Point_2 s = gt.construct_point_2_object()(cp_in_ffd.second[0],
                                                  cp_in_ffd.second[1]);
  const Point_2 t = gt.construct_point_2_object()(ct_in_ffd.second[0],
                                                  ct_in_ffd.second[1]);
  const Segment_2 mcs = gt.construct_segment_2_object()(s, t);

  const Point_2 fs = gt.construct_point_2_object()(fmc_track_source->location().second[0],
                                                   fmc_track_source->location().second[1]);
  const Point_2 ft = gt.construct_point_2_object()(fmc_track_destination->location().second[0],
                                                   fmc_track_destination->location().second[1]);
  const Segment_2 fmcs = gt.construct_segment_2_object()(fs, ft);

  find_collision_between_collinear_tracks(mc, mcs, fmc, fmc_track, fmcs,
                                          is_fmc_moving_on_track, tc);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_foreign_track_extremity(const Motorcycle& mc,
                                            const halfedge_descriptor hd,
                                            const Motorcycle& fmc,
                                            const DEC_it foreign_extremity,
                                            const FT foreign_time_at_collision,
                                            Collision_information& tc) const
{
  // this is the case of 'mc' tentative track being on a border, and a foreign
  // track with a single point on this same border

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤¤¤ Checking collision with tentative track on border"
            << " with foreign motorcycle " << fmc.id()
            << " and single foreign point on border: " << std::endl;
#endif

  namespace PMP = CGAL::Polygon_mesh_processing;

  // mc's track is non-degenerate
  CGAL_precondition(mc.current_position() != mc.closest_target());
  // mc's track in on the halfedge
  CGAL_precondition(PMP::is_on_halfedge(mc.current_position()->location(), hd, mesh_));
  CGAL_precondition(PMP::is_on_halfedge(mc.closest_target()->location(), hd, mesh_));
  // the foreign extremity is on a halfedge
  CGAL_precondition(PMP::get_descriptor_from_location(foreign_extremity->location(), mesh_).which() != 2);

  std::cout << "foreign extremity: " << &*foreign_extremity
                                     << " (" << foreign_extremity->point() << ")" << std::endl;

  const halfedge_descriptor opp_hd = opposite(hd, mesh_);
  CGAL_precondition(!is_border(opp_hd, mesh_));
  const face_descriptor ffd = face(opp_hd, mesh_);
  CGAL_precondition(foreign_extremity->location().first == ffd);

  const Face_location& cp_in_ffd = mc.current_position()->sibling(ffd);
  const Face_location& ct_in_ffd = mc.closest_target()->sibling(ffd);

  const Point_2 s = gt.construct_point_2_object()(cp_in_ffd.second[0],
                                                  cp_in_ffd.second[1]);
  const Point_2 t = gt.construct_point_2_object()(ct_in_ffd.second[0],
                                                  ct_in_ffd.second[1]);
  const Point_2 e = gt.construct_point_2_object()(foreign_extremity->location().second[0],
                                                  foreign_extremity->location().second[1]);

  if(s == e) // intersection at mc's current_position
  {
    // ignore it, 'mc' would have been stopped before if that intersection was meaningful
    std::cout << "    s == e" << std::endl;
    return;
  }
  else if(t == e) // intersection at mc's closest target
  {
    std::cout << "    t == e" << std::endl;
    const FT time_at_collision = mc.time_at_closest_target();

    // Compare to current tentative collision to keep the closest intersection
    if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
    {
      tc.reset();
      tc.is_closest_collision_already_in_dictionary = true;
      tc.closest_collision = mc.closest_target();
      tc.time_at_closest_collision = time_at_collision;

      tc.fmc_id = fmc.id();
      tc.is_foreign_motorcycle_in_different_face = true;
      tc.foreign_motorcycle_face = ffd;
      tc.foreign_time_at_closest_collision = foreign_time_at_collision;
    }
  }
  else // general case
  {
    // the assertion below might fail due to numerical errors, but it is, logically,
    // a correct statement (case of three points on the same halfedge)
#ifdef CGAL_POLYLINE_TRACING_ENABLE_RIGOROUS_PRECONDITIONS
      CGAL_assertion(gt.collinear_2_object()(s, e, t));
#endif

    std::cout << "    general case" << std::endl;

    if(!gt.collinear_are_strictly_ordered_along_line_2_object()(s, e, t))
      return;

    // From here on, e is on ]s;t[
    std::cout << "    e is on ]s;t[" << std::endl;

    Point collision_point = foreign_extremity->point();
    const FT time_at_collision = mc.current_time() +
      CGAL::sqrt(CGAL::squared_distance(mc.current_position()->point(),
                                        collision_point)) / mc.speed();

    if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
    {
      tc.reset();
      tc.is_closest_collision_already_in_dictionary = true;
      tc.closest_collision = foreign_extremity;
      tc.time_at_closest_collision = time_at_collision;

      tc.fmc_id = fmc.id();
      tc.is_foreign_motorcycle_in_different_face = true;
      tc.foreign_motorcycle_face = ffd;
      tc.foreign_time_at_closest_collision = foreign_time_at_collision;
    }
  }
}

// collisions between two motorcycles in the same face
template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_at_tentative_track_destination(const Motorcycle& mc,
                                              const Motorcycle& fmc,
                                              const FT fmc_visiting_time,
                                              Collision_information& tc) const
{
  FT time_at_collision = mc.time_at_closest_target();
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "  /!\\ Tentative path collides with track: " << fmc.id()
            << " at the closest target. Time: " << time_at_collision << std::endl;
#endif

  if(tc.is_collision_earlier_than_current_best(time_at_collision, fmc_visiting_time))
  {
    tc.reset();
    tc.is_closest_collision_already_in_dictionary = true;
    tc.closest_collision = mc.closest_target();
    tc.time_at_closest_collision = time_at_collision;

    tc.fmc_id = fmc.id();
    tc.foreign_time_at_closest_collision = fmc_visiting_time;
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_at_tentative_track_source(const Motorcycle& mc,
                                         const Motorcycle& fmc,
                                         const FT fmc_visiting_time,
                                         Collision_information& tc) const
{
  FT time_at_collision = mc.current_time();
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "  /!\\ Tentative path collides with track: " << fmc.id() << " at its source."
            << " Times: " << time_at_collision << " " << fmc_visiting_time << std::endl;
#endif

  if(tc.is_collision_earlier_than_current_best(time_at_collision, fmc_visiting_time))
  {
    tc.reset();
    tc.is_closest_collision_already_in_dictionary = true;

    tc.closest_collision = mc.current_position();
    tc.time_at_closest_collision = time_at_collision;

    tc.fmc_id = fmc.id();
    tc.foreign_time_at_closest_collision = fmc_visiting_time;
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_between_collinear_tracks(const Motorcycle& mc,
                                        const Segment_2& mcs,
                                        const Motorcycle& fmc,
                                        const Track_segment& fmc_track,
                                        const Segment_2& fmcs,
                                        const bool is_fmc_moving_on_track,
                                        Collision_information& tc) const
{
  // Below might fail due to numerical errors, but we are treating here the
  // case of two collinear tracks, possibly on different faces of the same edge.
#ifdef CGAL_POLYLINE_TRACING_ENABLE_RIGOROUS_PRECONDITIONS
  CGAL_precondition(gt.collinear_2_object()(mcs.source(), fmcs.source(), mcs.target()));
  CGAL_precondition(gt.collinear_2_object()(mcs.source(), fmcs.target(), mcs.target()));
#endif

  // Many different configurations exist, e.g. (_S is for source, _T for target):
  //  MC_S  ---- FMC_S ---- FMC_T ---- MC_T
  //  FMC_T ---- MC_S  ---- FMC_S ---- MC_T
  // etc.
  // If, on the ray MC_S->MC_T,
  // - FMC_S is "before" MC_S, then it doesn't matter for MC whichever respective
  //   direction the motorcycles are moving in.
  // - FMC_S is MC_S, then it only matters if they are moving in the same direction
  //   but this already treated before the algorithm starts, in the function
  //   'crash_motorcycles_with_same_source_and_direction()'
  // - FMC_S is "after" MC_S, then it depends on the motorcycles' directions.

  if(mcs.source() == fmcs.source())
    return;

  bool is_fmcs_degenerate = gt.is_degenerate_2_object()(fmcs);

  // Compute the respective direction of the two motorcycles:
  CGAL_precondition(mcs.source() != mcs.target());
  bool are_motorcycles_moving_in_the_same_direction =
    (is_fmcs_degenerate ||
     gt.angle_2_object()(mcs.source(), mcs.target(),
                         fmcs.source(), fmcs.target()) == CGAL::ACUTE);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "  is degen: " << is_fmcs_degenerate << std::endl;
  std::cout << "  angle: " << gt.angle_2_object()(mcs.source(), mcs.target(),
                                                  fmcs.source(), fmcs.target()) << std::endl;
  std::cout << "  are motorcycles moving in the same direction: "
            << are_motorcycles_moving_in_the_same_direction << std::endl;
#endif

  FT time_at_collision = 0.;
  const DEC_it fmc_track_source = fmc_track.template get<1>();
  const FT time_at_fmc_track_source = fmc_track.template get<2>();
  const DEC_it fmc_track_destination = fmc_track.template get<3>();
  const FT time_at_fmc_track_destination = fmc_track.template get<4>();
  const face_descriptor ffd = fmc_track_source->location().first;
  CGAL_assertion(ffd == fmc_track_destination->location().first);

  const bool are_motorcycles_on_the_same_face =
    (mc.current_location().first == fmc_track_source->location().first);

  // Some sanity checks -----
  CGAL_assertion(fmc_track_source->location().first == fmc_track_destination->location().first);
  CGAL_assertion(time_at_fmc_track_source <= time_at_fmc_track_destination);

  if(!are_motorcycles_on_the_same_face)
  {
    // Check that all track points are on the same halfedge
    CGAL_precondition_code
    (
      boost::optional<halfedge_descriptor> hd =
        CGAL::Polygon_mesh_processing::internal::common_halfedge(fmc_track_source->location().first,
                                                                 mc.current_location().first,
                                                                 mesh_);
    )
    CGAL_precondition(bool(hd));
    CGAL_precondition_code(halfedge_descriptor opp_hd = opposite(*hd, mesh_);)
    CGAL_precondition(CGAL::Polygon_mesh_processing::is_on_halfedge(fmc_track_source->location(), *hd, mesh_));
    CGAL_precondition(CGAL::Polygon_mesh_processing::is_on_halfedge(fmc_track_destination->location(), *hd, mesh_));
    CGAL_precondition(CGAL::Polygon_mesh_processing::is_on_halfedge(mc.current_position()->location(), opp_hd, mesh_));
    CGAL_precondition(CGAL::Polygon_mesh_processing::is_on_halfedge(mc.closest_target()->location(), opp_hd, mesh_));
  }
  // end of sanity checks -----

  // The motorcycles move in the same direction
  if(are_motorcycles_moving_in_the_same_direction)
  {
    // If there's an intersection, 'mc' will impact fmcs' source.

    // The weird configuration of both motorcycles moving in the same direction
    // AND with the same source is handled by crashing motorcycles at the very
    // beginning, see function: 'crash_motorcycles_with_same_source_and_direction()'
    CGAL_assertion(is_fmcs_degenerate || mcs.source() != fmcs.source());

    if(mcs.target() == fmcs.source())
    {
      time_at_collision = mc.time_at_closest_target();
    }
    // Note that here, we know that fmcs.source() != mcs.source() and mcs.target()
    else if(gt.collinear_are_strictly_ordered_along_line_2_object()(mcs.source(),
                                                                    fmcs.source(),
                                                                    mcs.target()))
    {
      time_at_collision = mc.current_time() +
        CGAL::sqrt(CGAL::squared_distance(mc.current_position()->point(),
                                          fmc_track_source->point())) / mc.speed();
    }
    else
    {
      // fmcs.source() is either 'before' mcs.source() or 'after' mcs.target().
      // Either way, we don't care about any potential intersection.
      return;
    }

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "  Motorcycles #" << mc.id() << " crashes into the source of Motorcycle #"
              << fmc.id() << " at time: " << time_at_collision << std::endl;
#endif

    if(tc.is_collision_earlier_than_current_best(time_at_collision, time_at_fmc_track_source))
    {
      tc.reset();
      tc.is_closest_collision_already_in_dictionary = true;
      tc.closest_collision = fmc_track_source;
      tc.time_at_closest_collision = time_at_collision;

      tc.fmc_id = fmc.id();
      tc.is_foreign_motorcycle_in_different_face = !are_motorcycles_on_the_same_face;
      tc.foreign_motorcycle_face = ffd;
      tc.foreign_time_at_closest_collision = time_at_fmc_track_source;
      CGAL_assertion(!tc.is_foreign_motorcycle_in_different_face ||
                     mc.current_location().first != ffd);
    }
  }
  else // Motorcycles are moving in opposite directions
  {
    // Note that here we know that:
    // - fmcs is not degenerate
    // - mcs.source() != fmcs.source()

    // If the foreign source is 'before' mc's source, then there is no intersection
    if(gt.collinear_are_strictly_ordered_along_line_2_object()(fmcs.source(),
                                                               mcs.source(),
                                                               mcs.target()))
      return;

    // If mc's target is in [mcs, fmcs], then there is no intersection
    if(mcs.target() != fmcs.target() && // to be able to use strictly
       gt.collinear_are_strictly_ordered_along_line_2_object()(mcs.target(),
                                                               fmcs.target(),
                                                               fmcs.source()))
      return;

    // Now, we know that on the imaginary axis on which 'mc' is driving:
    // - fmcs is in ]mcs; infinity[
    // - fmct is in ]-infinity; mct]
    // - fmct is 'before' fmcs
    // Thus there is an intersection (except if fmcs = mcs, but we have already
    // discarded that case).
    // There are two cases to distinguish: moving 'fmc' and stationary 'fmc'.

    if(!is_fmc_moving_on_track) // stationary 'fmc'
    {
      // The foreign motorcycle is not moving on its track, thus 'mc' crashes
      // into the final position of the foreign track.

      // Check some known cases to avoid having to compute the collision time
      if(mcs.target() == fmcs.target())
      {
        time_at_collision = mc.time_at_closest_target();
      }
      else if(mcs.source() == fmcs.target())
      {
        time_at_collision = mc.current_time();
      }
      // Note that we know that fmcs.target() != mcs.source() and mcs.target()
      else if(gt.collinear_are_strictly_ordered_along_line_2_object()(mcs.source(),
                                                                      fmcs.target(),
                                                                      mcs.target()))
      {
        // No choice but to compute the collision time
        time_at_collision = mc.current_time() +
          CGAL::sqrt(CGAL::squared_distance(mc.current_position()->point(),
                                            fmc_track_destination->point())) / mc.speed();

        CGAL_assertion(!mc.has_target_at_time(time_at_collision).second);
      }
      else
      {
        // fmcs.target() can't be 'before' mcs.source() because 'not_moving' means
        // that we are on a confirmed track and if fmcs.target() is 'after' mcs.target(),
        // then there is no intersection.
        return;
      }


#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "  Motorcycles #" << mc.id() << " crashes into the final position of Motorcycle #"
              << fmc.id() << " at time: " << time_at_collision << std::endl;
#endif

      if(tc.is_collision_earlier_than_current_best(time_at_collision, time_at_fmc_track_destination))
      {
        tc.reset();
        tc.is_closest_collision_already_in_dictionary = true;
        tc.closest_collision = fmc_track_destination;
        tc.time_at_closest_collision = time_at_collision;

        tc.fmc_id = fmc.id();
        tc.foreign_time_at_closest_collision = time_at_fmc_track_destination;
        tc.is_foreign_motorcycle_in_different_face = !are_motorcycles_on_the_same_face;
        tc.foreign_motorcycle_face = ffd;
        CGAL_assertion(!tc.is_foreign_motorcycle_in_different_face ||
                       mc.current_location().first != ffd);
      }
    }
    else // The foreign motorcycle is (also) moving
    {
      // The collision is at the middle point and both motorcycles reach it at the same time.
      // Note that this point might not actually be reached by either motorcycle,
      // e.g. if a motorcycle crashes before reaching it.

      // @todo, if speeds are ever allowed to change, the speed of fmc here
      // must be changed to the speed on the track segment 'fmc_track'
      const FT sqd = CGAL::squared_distance(mc.current_position()->point(),
                                            fmc_track_source->point());
      time_at_collision = mc.current_time() +
        (CGAL::sqrt(sqd) - fmc.speed() * (mc.current_time() - time_at_fmc_track_source)) / (mc.speed() + fmc.speed());

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "  sqd: " << sqd << std::endl;
      std::cout << "  speeds: " << mc.speed() << " " << fmc.speed() << std::endl;
      std::cout << "  current times: " << mc.current_time() << " " << time_at_fmc_track_source << std::endl;
      std::cout << "  final time: " << time_at_collision << std::endl;
      std::cout << "  § mc and fmc would meet at time: " << time_at_collision << std::endl;
#endif

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
      // The time and foreign_time by construction should be greater
      // than the times at the sources of the tracks. Some numerical errors
      // can sneak it, if so, snap the time.
      //
      // It should only be a numerical error, that is a very small error
      FT tolerance = std::numeric_limits<FT>::epsilon();
      if(time_at_collision < mc.current_time())
      {
        CGAL_precondition(time_at_collision + tolerance >= mc.current_time());
        time_at_collision = mc.current_time();
        CGAL_postcondition(time_at_collision >= time_at_fmc_track_source);
      }
      else if(time_at_collision < time_at_fmc_track_source)
      {
        CGAL_precondition(time_at_collision + tolerance >= time_at_fmc_track_source);
        time_at_collision = time_at_fmc_track_source;
        CGAL_postcondition(time_at_collision >= mc.current_time());
      }
#endif

      if(tc.is_collision_earlier_than_current_best(time_at_collision, time_at_collision))
      {
        // both values are used later when we snap times/points
        const FT time_at_closest_collision_memory = tc.time_at_closest_collision;
        const FT foreign_time_at_closest_collision_memory = tc.foreign_time_at_closest_collision;

        tc.reset();
        tc.time_at_closest_collision = time_at_collision;

        tc.fmc_id = fmc.id();
        tc.is_foreign_motorcycle_in_different_face = !are_motorcycles_on_the_same_face;
        tc.foreign_motorcycle_face = ffd;
        tc.foreign_time_at_closest_collision = time_at_collision;

        // Temporal snapping ---------------------------------------------------
        // Try to find the collision point by checking if any of the motorcycles
        // has a point at that time.
        bool used_temporal_snapping = false; //@todo rename that, there's no snapping here

        std::pair<TPC_iterator, bool> mc_res = mc.has_target_at_time(time_at_collision);
        if(mc_res.second) // there is already a target at that time
        {
          std::cout << "Motorcycle #" << mc.id() << " already has a target at time: " << time_at_collision << std::endl;

          TPC_iterator target_point = mc_res.first;
          CGAL_assertion(target_point->second == time_at_collision);
          DEC_it alternate_collision = target_point->first;

          tc.is_closest_collision_already_in_dictionary = true;
          tc.closest_collision = alternate_collision;

          used_temporal_snapping = true;
        }

        // Same check with the foreign time at collision
        std::pair<TPC_iterator, bool> fmc_res = fmc.has_target_at_time(time_at_collision);
        if(fmc_res.second) // there is already a target at that time
        {
          std::cout << "Motorcycle #" << fmc.id() << " already has a target at time: " << time_at_collision << std::endl;

          TPC_iterator target_point = fmc_res.first;
          DEC_it alternate_foreign_collision = target_point->first;
          CGAL_assertion(alternate_foreign_collision->location().first == fmc.current_location().first);
          CGAL_assertion(target_point->second == time_at_collision);

          if(used_temporal_snapping)
          {
            // If the collision has already been snapped for time_at_collision reasons, assert
            // that the two snapped locations correspond to the same point.
            CGAL_assertion(alternate_foreign_collision->location() ==
                           tc.closest_collision->sibling(fmc.current_location().first));
          }
          else // temporal snapping hasn't been used yet
          {
            tc.is_closest_collision_already_in_dictionary = true;
            tc.closest_collision = alternate_foreign_collision;
          }

          used_temporal_snapping = true;
        }

        if(!used_temporal_snapping)
        {
          // No choice but to construct the collision location
          const Vector_2 mcv(mcs);
          const FT ratio = (time_at_collision - mc.current_time()) /
                           (mc.time_at_closest_target() - mc.current_time());
          const Point_2 collision = mcs.source() + ratio * mcv;

          Face_location collision_location = std::make_pair(fmc_track_source->location().first,
                                               CGAL::make_array(collision[0], collision[1],
                                                                1. - collision[0] - collision[1]));
#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
          // 1-x-y can result in some nasty "1e-17" imprecisions...
          CGAL::Polygon_mesh_processing::internal::snap_location_to_border<Triangle_mesh>(collision_location);
#endif

          // Couldn't find it through visiting times, but check if the new location
          // is already visited by 'mc' or 'fmc' (can happen due to numerical imprecisions)
          std::pair<DEC_it, bool> collision_entry = points.find(collision_location);
          if(collision_entry.second) // the point already existed
          {
            CGAL_assertion(collision_entry.first != DEC_it());

            tc.is_closest_collision_already_in_dictionary = true;
            tc.closest_collision = collision_entry.first;

            // We previously searched by time but couldn't find anything but the
            // point existed. Check if that point is visited by either 'mc' or 'fmc';
            // if it's the case, we need to repare the time to be that of the existing
            // point.

            // Add a small tolerance on the time since we previously didn't find any target at the exact time
            FT tolerance = std::numeric_limits<FT>::epsilon();

            FT visiting_time; // will be filled by the call to 'has_motorcycle'
            if(collision_entry.first->has_motorcycle(mc.id(), time_at_collision - tolerance,
                                                     time_at_collision + tolerance, visiting_time))
            {
              std::cout << "Motorcycle #" << mc.id() << " already has a target at time: " << visiting_time << std::endl;

              // Assert that we are still the closest collision (not sure what to do otherwise) @todo
              CGAL_assertion(visiting_time < time_at_closest_collision_memory);

              tc.time_at_closest_collision = visiting_time;
              tc.foreign_time_at_closest_collision = visiting_time; // times are equal in this configuration
              return;
            }

            // Try with 'fmc'
            FT foreign_visiting_time;
            if(collision_entry.first->has_motorcycle(fmc.id(), time_at_collision - tolerance,
                                                     time_at_collision + tolerance, foreign_visiting_time))
            {
              std::cout << "Foreign motorcycle #" << fmc.id() << " already has a target at time: " << foreign_visiting_time << std::endl;

              // Assert that we are still the closest collision (not sure what to do otherwise)
              CGAL_assertion_code(if(tc.time_at_closest_collision == time_at_closest_collision_memory))
              CGAL_assertion(foreign_visiting_time < foreign_time_at_closest_collision_memory);

              std::cout << "found: fmc.id(): " << fmc.id() << " in pt: " << std::endl << *(collision_entry.first) << std::endl;
              std::cout << "foreign_visiting_time: " << foreign_visiting_time << std::endl;
              tc.foreign_time_at_closest_collision = foreign_visiting_time;
              tc.time_at_closest_collision = foreign_visiting_time; // times are equal in this configuration
              return;
            }
          }
          else
          {
            // At this point, we have a new location at an unknown time...
#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
            // But maybe there exists another point that is very close! Check for it,
            // and if needed, snap the new location (and the time) to it.

            Point collision_point = CGAL::Polygon_mesh_processing::location_to_point(collision_location, mesh_);

            // @todo tolerance should be 'snapping_tolerance' and should be global
            const FT tolerance = 2 * std::numeric_limits<FT>::epsilon();
            std::pair<DEC_it, bool> snapped =
              try_to_snap_location_to_existing_point(collision_location, collision_point, tolerance);
            if(snapped.second) // successful snapping
            {
              FT visiting_time = time_at_collision;

              // the call to this function will modify 'visiting_time' if the point of snapping is already is visited by 'mc'
              const FT min_visiting_time = time_at_collision - tolerance;
              const FT max_visiting_time = time_at_collision + tolerance;
              if(!snapped.first->has_motorcycle(mc.id(), min_visiting_time, max_visiting_time, visiting_time))
              {
                // While trying to get the visiting time, if the snapped point is
                // not visited by 'mc', check if it is visited by 'fmc'
                snapped.first->has_motorcycle(fmc.id(), min_visiting_time, max_visiting_time, visiting_time);
              }

              // We have snapped so we are igoring times that we had set up as best, but
              // we need to make sure it is still better then the previous one.
              CGAL_assertion(visiting_time <= time_at_closest_collision_memory);
              CGAL_assertion(visiting_time < time_at_closest_collision_memory ||
                             visiting_time < foreign_time_at_closest_collision_memory);

              tc.add_intersection_without_moving_motorcycles = true;

              tc.is_closest_collision_already_in_dictionary = true;
              tc.closest_collision = snapped.first;
              tc.time_at_closest_collision = visiting_time;
              tc.foreign_time_at_closest_collision = visiting_time;
              return;
            }
#endif

            // Couldn't snap to anything, 'collision_location' is definitely a new point
            tc.is_closest_collision_already_in_dictionary = false;
            tc.closest_collision_location = collision_location;
          }
        }
      }
    }
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_between_tracks(const Motorcycle& mc, // @todo just reshape it to have two track_segment
                              const Segment_2& mcs,
                              const Motorcycle& fmc,
                              const Track_segment& fmc_track,
                              const bool is_fmc_moving_on_track,
                              // below are out parameters
                              Collision_information& tc) const
{
  // Non degenerate mc segment
  CGAL_precondition(mc.current_position() != mc.closest_target());
  CGAL_precondition(mcs.source() != mcs.target());

  const DEC_it fmc_track_source = fmc_track.template get<1>();
  const FT time_at_fmc_track_source = fmc_track.template get<2>();
  const DEC_it fmc_track_destination = fmc_track.template get<3>();
  const FT time_at_fmc_track_destination = fmc_track.template get<4>();

  // Both tracks must be on the same face
  CGAL_precondition(fmc_track_source->location().first == fmc_track_destination->location().first);

  const Point_2 s = gt.construct_point_2_object()(fmc_track_source->location().second[0],
                                                  fmc_track_source->location().second[1]);
  const Point_2 t = gt.construct_point_2_object()(fmc_track_destination->location().second[0],
                                                  fmc_track_destination->location().second[1]);
  const Segment_2 fmcs = gt.construct_segment_2_object()(s, t);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "¤¤ Checking collision with track of motorcycle #" << fmc.id() << std::endl;
  std::cout << " + source: " << &*fmc_track_source << std::endl << *fmc_track_source << std::endl;
  std::cout << " + target: " << &*fmc_track_destination << std::endl << *fmc_track_destination << std::endl;
#endif

  // Ignore the case of a degenerate fmc track starting at the same source as mc's
  bool is_fmcs_degenerate = gt.is_degenerate_2_object()(fmcs);
  if(is_fmcs_degenerate)
  {
    if(mcs.source() == fmcs.source())
    {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "degenerate fmc and mcs.source() == fmcs.source()" << std::endl;
#endif
      return;
    }
#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
    else if(internal::are_logically_collinear_on_border<Geom_traits>(
              mc.current_position()->location(), fmc_track_source->location(), mc.closest_target()->location()))
    {
      return find_collision_between_collinear_tracks(mc, mcs, fmc, fmc_track, fmcs,
                                                     is_fmc_moving_on_track, tc);
    }
#endif
  }

  // Detect whether the motorcycles share the same supporting line.
  // Note that we know that 'mcs' is not degenerate.
  // @todo should this have a tolerance ?
  if(gt.collinear_2_object()(mcs.source(), mcs.target(), fmcs.source()) &&
     gt.collinear_2_object()(mcs.source(), mcs.target(), fmcs.target()))
  {
    std::cout << "  /!\\ Tracks are aligned" << std::endl;
    return find_collision_between_collinear_tracks(mc, mcs, fmc, fmc_track, fmcs,
                                                   is_fmc_moving_on_track, tc);
  }

  // --- From here on, the tracks are not collinear ---

  // Below are a bunch of checks to branch out easily without computing an explicit
  // intersection.
  // - #1: Check if the current position of mc is a known intersection with the foreign track
  // - #2: Check if the closest target of mc is a known intersection with the foreign track
  // - #3: Robustness for intersections on halfedge

  // Check #1: known collision at current_position
  std::cout << "  check #1: motorcycle #" << fmc.id() << " between "
            << time_at_fmc_track_source << " " << time_at_fmc_track_destination << std::endl;
  if(mc.current_position()->has_motorcycle(fmc.id(), time_at_fmc_track_source, time_at_fmc_track_destination))
  {
    // Ignore this intersection: since we are seeking collision in the tentative track,
    // it means that the position was not blocked
    return;
  }

  // Check #2: known collision at closest_target
  std::cout << "  check #2: collisition at tentative's track destination ?" << std::endl;
  FT foreign_visiting_time; // will be filled by 'has_motorcycle' if fmc visits 'closest_target'
  if(mc.closest_target()->has_motorcycle(fmc.id(), time_at_fmc_track_source,
                                         time_at_fmc_track_destination, foreign_visiting_time))
  {
    return find_collision_at_tentative_track_destination(mc, fmc, foreign_visiting_time, tc);
  }

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
  // Check #3: collision at destination, with foreign track on an edge
  // Catch some annoying numerical issue: the configuration of FMCS on a halfedge
  // and the motorcycle destination on the same edge (but somehow, do_intersect_2()
  // does not find it...).
  // Only doing it for the closest_target because we don't care about the source.
  std::cout << "  check #3: foreign track and target on the same border" << std::endl;
  CGAL_assertion(fmc_track_source != mc.closest_target());
  CGAL_assertion(fmc_track_destination != mc.closest_target());

  if(internal::are_logically_collinear_on_border<Geom_traits>(
      fmc_track_source->location(), mc.closest_target()->location(), fmc_track_destination->location()))
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "  foreign track and target are logically collinear on border" << std::endl;
#endif

    if(gt.collinear_are_strictly_ordered_along_line_2_object()(s, mcs.target(), t))
    {
      const FT time_at_collision = mc.time_at_closest_target();
      const FT foreign_time_at_collision = time_at_fmc_track_source +
        CGAL::sqrt(CGAL::squared_distance(fmc_track_source->point(),
                                          mc.closest_target()->point())) / fmc.speed();

      if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
      {
        tc.reset();
        tc.is_closest_collision_already_in_dictionary = true;
        tc.closest_collision = mc.closest_target();
        tc.time_at_closest_collision = time_at_collision;

        tc.fmc_id = fmc.id();
        tc.foreign_time_at_closest_collision = foreign_time_at_collision;
      }
    }

    return;
  }

  // Check #4: collision at foreign destination, with track and foreign destination
  // on the same halfedge.
  std::cout << "  check #4: track and foreign destination on a same halfedge" << std::endl;
  CGAL_assertion(fmc_track_destination != mc.current_position());
  CGAL_assertion(fmc_track_destination != mc.closest_target());
  if(internal::are_logically_collinear_on_border<Geom_traits>(
      fmc_track_destination->location(), mc.closest_target()->location(), mc.current_location()))
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "  track and foreign target are logically collinear on border" << std::endl;
#endif

    if(gt.collinear_are_strictly_ordered_along_line_2_object()(mcs.source(), t, mcs.target()))
    {
      const FT sqd = CGAL::squared_distance(mc.current_position()->point(),
                                            fmc_track_destination->point());
      const FT time_at_collision = mc.current_time() + CGAL::sqrt(sqd) / mc.speed();
      const FT foreign_time_at_collision = time_at_fmc_track_destination;

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "  foreign target in ] track [ " << std::endl;
      std::cout << "  Pts: (" << mc.current_position()->point() << ") -- ("
                              << fmc_track_destination->point() << ")" << std::endl;
      std::cout << "  current time: " << mc.current_time() << std::endl;
      std::cout << "  sqd: " << sqd << std::endl;
      std::cout << "  time at collision: " << time_at_collision << std::endl;
#endif

      if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
      {
        tc.reset();
        tc.is_closest_collision_already_in_dictionary = true;
        tc.closest_collision = fmc_track_destination;
        tc.time_at_closest_collision = time_at_collision;

        tc.fmc_id = fmc.id();
        tc.foreign_time_at_closest_collision = foreign_time_at_collision;
      }
    }

    return;
  }

  // Check #4bis: collision at foreign source, with track and foreign source
  // on the same halfedge.
  CGAL_assertion(fmc_track_source != mc.current_position());
  CGAL_assertion(fmc_track_source != mc.closest_target());
  std::cout << "  check #4: track and foreign source on a same halfedge" << std::endl;
  if(internal::are_logically_collinear_on_border<Geom_traits>(
      fmc_track_source->location(), mc.closest_target()->location(), mc.current_location()))
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "  track and foreign source are logically collinear on border" << std::endl;
#endif

    if(gt.collinear_are_strictly_ordered_along_line_2_object()(mcs.source(), s, mcs.target()))
    {
      const FT time_at_collision = mc.current_time() +
        CGAL::sqrt(CGAL::squared_distance(mc.current_position()->point(),
                                          fmc_track_source->point())) / mc.speed();
      const FT foreign_time_at_collision = time_at_fmc_track_source;

      std::cout << "  foreign source in ] track [, "
                << "time at collision: " << time_at_collision << std::endl;

      if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
      {
        tc.reset();
        tc.is_closest_collision_already_in_dictionary = true;
        tc.closest_collision = fmc_track_source;
        tc.time_at_closest_collision = time_at_collision;

        tc.fmc_id = fmc.id();
        tc.foreign_time_at_closest_collision = foreign_time_at_collision;
      }
    }

    return;
  }
#endif

  // --- The general case: the intersection must be computed ---
  std::cout << "  general case..." << std::endl;

  // Ignoring the case of a degenerate fmcs because if there is an intersection,
  // it will have been caught by the first part of that function,
  // branching: "collinear > moving in the same direction"
  if(is_fmcs_degenerate)
  {
    CGAL_assertion(!gt.do_intersect_2_object()(mcs, fmcs));
    std::cout << "  No intersection with degenerate fmcs track" << std::endl;
    return;
  }

  if(!gt.do_intersect_2_object()(mcs, fmcs))
  {
    // No intersection, move to the next motorcycle
    std::cout << "  No intersection (general case)" << std::endl;
    return;
  }

  // Below computes the intersection in the barycentric coordinates system
  Point_2 collision = internal::robust_intersection<Geom_traits>(mcs, fmcs, gt);

  // Convert it to a location in the ambiant dimension
  Barycentric_coordinates coords = CGAL::make_array(collision[0], collision[1],
                                                    1. - collision[0] - collision[1]);
  Face_location collision_location = std::make_pair(mc.current_location().first, coords);

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
  // 1-x-y can result in some nasty "1e-17" imprecisions...
  CGAL::Polygon_mesh_processing::internal::snap_location_to_border<Triangle_mesh>(collision_location);
#endif

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "  /!\\ collision between motorcycles #" << mc.id() << " and #" << fmc.id() << std::endl;
  std::cout << "Collision location: " << collision_location.first << " bc: "
            << collision_location.second[0] << " " << collision_location.second[1] << " " << collision_location.second[2] << std::endl;
#endif

  // Although we might not have known that these two tracks do intersect,
  // their intersection might be a point that has already been used
  std::pair<DEC_it, bool> is_already_in_dictionary = points.find(collision_location);
  if(is_already_in_dictionary.second)
  {
    DEC_it collision_point = is_already_in_dictionary.first;
    FT time_at_collision = 0.;

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "Already in the dictionary at: " << &*collision_point << std::endl << *collision_point << std::endl;
#endif

    // Check if 'mc' already visits the known collision point
    if(collision_point == mc.closest_target())
    {
      time_at_collision = mc.time_at_closest_target();
    }
    else if(collision_point == mc.current_position())
    {
      time_at_collision = mc.current_time();
    }
    else // 'collision_point' is a known point but has not (yet) been visited by 'mc'
    {
      // The tentative track of 'mc' can only be intersected at a known point that has 'mc'
      // if that known point is the current position or the closest target.
      CGAL_assertion(!collision_point->has_motorcycle(mc.id(), mc.current_time(), mc.time_at_closest_target()));

      // No choice but to compute the visiting time
      time_at_collision = mc.current_time() +
        CGAL::sqrt(CGAL::squared_distance(mc.current_position()->point(),
                                          collision_point->point())) / mc.speed();

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
      // Although we have found an _existing_ point at the location of the intersection,
      // this point was neither the source or the closest target of 'mc'.
      // Global snapping makes sure that points are not too close from one another.
      // Consequently, the times should be different.
      CGAL_assertion(time_at_collision != mc.current_time());
      CGAL_assertion(time_at_collision != mc.time_at_closest_target());
#endif
    }

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "time_at_collision: " << time_at_collision
              << " (closest is: " << tc.time_at_closest_collision << ") " << std::endl;
#endif

    // Partial test of "is_collision_earlier..." to branch out early
    if(time_at_collision <= tc.time_at_closest_collision)
    {
      // Check if 'fmc' already visits the known collision point
      FT foreign_time_at_collision;
      if(collision_point->has_motorcycle(fmc.id(), time_at_fmc_track_source,
                                         time_at_fmc_track_destination, foreign_time_at_collision))
      {
        // The collision point is visited by 'fmc' at time 'foreign_time_at_collision'
      }
      else // 'collision_point' is a known point but has not (yet) been visited by 'fmc'
      {
        // No choice but to compute the foreign visiting time
        const FT sqd = CGAL::squared_distance(fmc_track_source->point(),
                                              collision_point->point());
        foreign_time_at_collision = time_at_fmc_track_source + CGAL::sqrt(sqd) / fmc.speed();

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "  Gotta compute the foreign time " << std::endl;
      std::cout << "  Pts: (" << fmc_track_source->point() << ") -- ("
                              << collision_point->point() << ")" << std::endl;
      std::cout << "  foreign source time: " << time_at_fmc_track_source << std::endl;
      std::cout << "  sqd: " << sqd << std::endl;
      std::cout << "  foreign time at collision: " << foreign_time_at_collision << std::endl;
#endif

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
        // Although we have found an _existing_ point at the location of the intersection,
        // this point was neither the source or the closest target of 'mc'.
        // Global snapping makes sure that points are not too close from one another.
        // Consequently, the times should be different.
        CGAL_assertion(!fmc.has_target_at_time(tc.foreign_time_at_closest_collision).second);
#endif
      }

      if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
      {
        tc.reset();
        tc.is_closest_collision_already_in_dictionary = true;
        tc.closest_collision = collision_point;
        tc.time_at_closest_collision = time_at_collision;

        tc.fmc_id = fmc.id();
        tc.foreign_time_at_closest_collision = foreign_time_at_collision;
      }
    }
  }
  else // The collision location has never been seen before!
  {
    Point collision_point = CGAL::Polygon_mesh_processing::location_to_point(collision_location, mesh_);

    const FT time_at_collision = mc.current_time() +
      CGAL::sqrt(CGAL::squared_distance(mc.current_position()->point(),
                                        collision_point)) / mc.speed();
    const FT foreign_time_at_collision = time_at_fmc_track_source +
      CGAL::sqrt(CGAL::squared_distance(fmc_track_source->point(),
                                        collision_point)) / fmc.speed();

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "Location never seen before, corresponds to point ("
              << collision_point << ") at time: " << time_at_collision << std::endl;
#endif

    CGAL_postcondition(time_at_collision >= mc.current_time());
    CGAL_postcondition(time_at_collision <= mc.time_at_closest_target());
    CGAL_postcondition(foreign_time_at_collision >= time_at_fmc_track_source);
    CGAL_postcondition(foreign_time_at_collision <= time_at_fmc_track_destination);

    if(tc.is_collision_earlier_than_current_best(time_at_collision, foreign_time_at_collision))
    {
      // both values are used later when we snap times/points
      const FT time_at_closest_collision_memory = tc.time_at_closest_collision;
      const FT foreign_time_at_closest_collision_memory = tc.foreign_time_at_closest_collision;

      tc.reset();
      tc.time_at_closest_collision = time_at_collision;
      tc.fmc_id = fmc.id();
      tc.foreign_time_at_closest_collision = foreign_time_at_collision;

      // Although there does not exist a point at the location of the collision,
      // this point might be at the same time from the source of the track
      // as another point due to numerical errors.
      bool used_temporal_snapping = false;

      std::pair<TPC_iterator, bool> res = mc.has_target_at_time(time_at_collision);
      if(res.second)
      {
        std::cout << "Motorcycle #" << mc.id() << " already has a target at time: " << time_at_collision << std::endl;

        TPC_iterator target_point = res.first;
        CGAL_assertion(target_point->second == time_at_collision);
        DEC_it alternate_collision = target_point->first;

        // If the times are equal, the points should be very close
        CGAL_assertion(CGAL::squared_distance(alternate_collision->point(), collision_point)
                       < std::numeric_limits<FT>::epsilon());

        // Temporal snap: the collision is now that existing point instead
        tc.is_closest_collision_already_in_dictionary = true;
        tc.closest_collision = alternate_collision;
        collision_location = alternate_collision->location();

        used_temporal_snapping = true;
        // @todo simply return here ? Useful to keep it to check snapping consistency, though
        // if removed: remove the boolean and symmetry with other 'used_temporal snapping' uses
      }

      std::pair<TPC_iterator, bool> fmc_res = fmc.has_target_at_time(foreign_time_at_collision);
      if(fmc_res.second) // there is already a target at that time
      {
        std::cout << "Foreign motorcycle #" << fmc.id() << " already has a target at time: " << foreign_time_at_collision << std::endl;

        TPC_iterator target_point = fmc_res.first;
        DEC_it alternate_foreign_collision = target_point->first;
        CGAL_assertion(alternate_foreign_collision->location().first == fmc.current_location().first);
        CGAL_assertion(target_point->second == foreign_time_at_collision);

        if(used_temporal_snapping)
        {
          // If the collision has already been snapped for time_at_collision reasons,
          // assert that the two snapped locations correspond to the same point.
          CGAL_assertion(alternate_foreign_collision->location() ==
                         tc.closest_collision->sibling(fmc.current_location().first));
        }
        else // Temporal snapping hasn't been used yet
        {
          tc.is_closest_collision_already_in_dictionary = true;
          tc.closest_collision = alternate_foreign_collision;
          // @todo recompute the non-foreign time?
        }

        used_temporal_snapping = true;
      }

      if(!used_temporal_snapping)
      {
        // At this point, we have a new location at an unknown time...
#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
        // But maybe there exists another point that is very close! Check for it,
        // and if needed, snap the new location (and the time) to it.

        // @todo tolerance should be 'snapping_tolerance' and should be global
        const FT tolerance = 2 * std::numeric_limits<FT>::epsilon();
        std::pair<DEC_it, bool> snapped =
          try_to_snap_location_to_existing_point(collision_location, collision_point, tolerance);
        if(snapped.second) // successful snapping
        {
          FT visiting_time = time_at_collision;

          // the call to this function will modify 'visiting_time' if the point of snapping is already is visited by 'mc'
          snapped.first->has_motorcycle(mc.id(), time_at_collision - tolerance, time_at_collision + tolerance, visiting_time);

          if(visiting_time <= tc.time_at_closest_collision)
          {
            FT foreign_visiting_time = foreign_time_at_collision;
            // the call to this function will modify 'foreign_visiting_time' if the point of snapping is already is visited by 'fmc'
            snapped.first->has_motorcycle(fmc.id(), foreign_time_at_collision - tolerance, foreign_time_at_collision + tolerance, foreign_visiting_time);

            // We have snapped so we are igoring times that we had set up as best, but
            // we need to make sure it is still better then the previous one.
            CGAL_assertion(visiting_time <= time_at_closest_collision_memory);
            CGAL_assertion(visiting_time < time_at_closest_collision_memory ||
                           foreign_visiting_time < foreign_time_at_closest_collision_memory);

            tc.add_intersection_without_moving_motorcycles = true;

            tc.is_closest_collision_already_in_dictionary = true;
            tc.closest_collision = snapped.first;
            tc.time_at_closest_collision = visiting_time;
            tc.foreign_time_at_closest_collision = foreign_visiting_time;
            return;
          }
        }
#endif

        // Couldn't snap to anything, 'collision_location' is definitely a new point
        tc.is_closest_collision_already_in_dictionary = false;
        tc.closest_collision_location = collision_location;
      }
    }
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_complete_track(Motorcycle& mc, const Segment_2& mcs,
                                   const Track_segment& fmc_track,
                                   // below are out parameters
                                   Collision_information& tc)
{
  const std::size_t fmc_id = fmc_track.template get<0>();
  const Motorcycle& fmc = motorcycle(fmc_id);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
std::cout << "¤ Checking for intersection with the complete track of motorcycle #" << fmc.id() << std::endl;
#endif

  // 'false' because the motorcycle is not moving on that track
  return find_collision_between_tracks(mc, mcs, fmc, fmc_track, false, tc);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision_with_live_motorcycle(Motorcycle& mc, const Segment_2& mcs,
                                    const Motorcycle& fmc,
                                    // below are out parameters
                                    Collision_information& tc)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
std::cout << "¤ Checking for intersection with live motorcycle #" << fmc.id() << std::endl;
#endif

  if(// the motorcycles must be different
     mc.id() == fmc.id() ||
     // the motorcycles must be in the same face
     mc.current_location().first != fmc.current_location().first ||
     // the foreign motorcycle must be in motion
     fmc.is_crashed())
  {
    std::cout << " ignoring fmc..." << std::endl;
    std::cout << "  > motorcycles #" << mc.id() << " and #" << fmc.id() << std::endl;
    std::cout << "  > faces: " << mc.current_location().first << " and " << fmc.current_location().first << std::endl;
    std::cout << "  > crashed status: " << fmc.is_crashed() << std::endl;
    return;
  }

  Track_segment fmc_track = boost::make_tuple(fmc.id(), fmc.source(), fmc.time_at_source(),
                                              fmc.closest_target(), fmc.time_at_closest_target());

  // 'true' because fmc is currently moving on that track
  return find_collision_between_tracks(mc, mcs, fmc, fmc_track, true, tc);
}

// search for a possible collision with another motorcycle between the current
// position of mc and the next target
template<typename MotorcycleGraphTraits>
typename Motorcycle_graph<MotorcycleGraphTraits>::Collision_information
Motorcycle_graph<MotorcycleGraphTraits>::
find_collision(Motorcycle& mc)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "~~~~~~~~~X ?" << std::endl;
  std::cout << "Checking for collisions on motorcycle #" << mc.id() << "'s track" << std::endl
            << "Currently on face: " << mc.current_location().first << std::endl;
#endif

  CGAL_precondition(!mc.is_crashed());
  CGAL_precondition(!mc.targets().empty());

  // A bunch of output parameters are regrouped into the 'Collision_information' struct,
  // which describes the best (closest to mc.current_position()) tentative collision.
  Collision_information tc(mc.time_at_closest_target() /*maximum allowed time*/);

  // The motorcycles must be on the same face
  CGAL_precondition(mc.current_location().first == mc.closest_target()->location().first);

  // Use the barycentric coordinate systems to compute intersections
  const Point_2 s = gt.construct_point_2_object()(mc.current_location().second[0],
                                                  mc.current_location().second[1]);
  const Point_2 t = gt.construct_point_2_object()(mc.closest_target()->location().second[0],
                                                  mc.closest_target()->location().second[1]);
  const Segment_2 mc_tentative_track = gt.construct_segment_2_object()(s, t);

  std::cout << "MC tentative track: " << std::endl
            << "source: " << &*(mc.current_position()) << " " << *(mc.current_position()) << std::endl
            << "target: " << &*(mc.closest_target()) << " " << *(mc.closest_target()) << std::endl;

  // A degenerate tentative track has no interesting collisions
  if(mc_tentative_track.is_degenerate())
    return tc;

  // Checking for intersection is done in two steps:
  // - 1: Check with complete tracks in the face
  // - 2: Check the motorcycles that are currently moving in the face
  // - 3: Check for intersections with tracks from foreign faces

  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_ COMPLETE TRACKS _-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;

  // Step 1: check complete tracks
  const face_descriptor mc_fd = mc.current_location().first;
  TFM_iterator it = track_face_map.find(mc_fd);
  if(it != track_face_map.end())
  {
    const Track_segment_container& face_tracks = it->second;

    typename Track_segment_container::const_iterator tl_it = face_tracks.begin();
    typename Track_segment_container::const_iterator tl_end = face_tracks.end();
    for(; tl_it!=tl_end; ++tl_it)
    {
      const Track_segment& track = *tl_it;
      find_collision_with_complete_track(mc, mc_tentative_track, track, tc);

      if(tc.add_intersection_without_moving_motorcycles)
        return tc;
    }
  }

  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_ LIVE MOTOS -_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;

  // Step 2: check incomplete tracks (path of a motorcycle currently moving in the same face)
  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t fmc_id = 0; fmc_id<number_of_motorcycles; ++fmc_id)
  {
    Motorcycle& fmc = motorcycle(fmc_id);
    find_collision_with_live_motorcycle(mc, mc_tentative_track, fmc, tc);

    if(tc.add_intersection_without_moving_motorcycles)
      return tc;
  }

  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_- FOREIGNERS _-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;
  std::cout << "_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-" << std::endl;

  // @todo check if an intersection has already been found and AND the track is
  // not on a border:
  // - the source is not on a halfedge
  // - the source and the destination are on different halfedges
  // And, if so, skip below ?
  // with the reasoning that an intersection within the face is necessarily
  // earlier or at the same time as an intersection on the border.

  // Step 3: check adjacent faces
  find_collision_with_foreign_motorcycles(mc, tc);

  return tc;
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
generate_enclosing_face()
{
  // generate a bbox that includes all known positions and all crash points
  // 2D only for now @todo
  CGAL_precondition(Geom_traits::dimension() == 2);
  CGAL_assertion(false);
#if 0
  Bbox bbox;

  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t mc_id = 0; mc_id<number_of_motorcycles; ++mc_id)
  {
    Motorcycle& mc = motorcycle(mc_id);
    bbox += mc.input_source().bbox();

    if(mc.input_destination() != boost::none)
      bbox += mc.input_destination()->bbox();

    // this part is brute force for now, but can be done in O(nlogn) by sorting
    // according to the slopes (farthest intersections happen when the slopes
    // of the motorcycles are close)
    for(std::size_t fmc_id = 0; fmc_id<number_of_motorcycles; ++fmc_id)
    {
      // segment - segment, segment - ray, or ray-ray intersections @todo
    }
  }

  // Slightly increase the size of the bbox to strictly contain all points

  // Manually create the mesh with Euler operations
#endif
}

template<typename MotorcycleGraphTraits>
bool
Motorcycle_graph<MotorcycleGraphTraits>::
has_motorcycle_reached_crashing_point(const Motorcycle& mc) const
{
  return (// multiple motorcycles will reach mc's current position at the same time
          mc.has_reached_simultaneous_collision_point() ||
          // the current position might be blocked (including in its representations in other faces)
          is_motorcycle_position_blocked(mc));
}

template<typename MotorcycleGraphTraits>
bool
Motorcycle_graph<MotorcycleGraphTraits>::
has_motorcycle_reached_final_destination(const Motorcycle& mc) const
{
  return mc.is_destination_final();
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
initialize_motorcycles()
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "Initialize motorcycles" << std::endl;
#endif
  namespace PMP = CGAL::Polygon_mesh_processing;

  AABB_tree tree;
  AABB_tree_VPM vpm(mesh_);

  // if no mesh has been given in input, generate a mesh made of a single quad face
  // that contains all the interesting motorcycle interactions (crashes)
  if(using_enclosing_bbox)
    generate_enclosing_face();

  if(is_aabb_tree_needed())
  {
    PMP::build_aabb_tree(mesh_, tree, parameters::vertex_point_map(vpm));
  }

  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t mc_id = 0; mc_id<number_of_motorcycles; ++mc_id)
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "      _" << std::endl;
    std::cout << "    D/_" << std::endl;
    std::cout << "    /(__`=-/" << std::endl;
    std::cout << "  (o)     (o)" << std::endl;
    std::cout << "Initializing motorcycle #" << mc_id << std::endl;
#endif

    Motorcycle& mc = motorcycle(mc_id);
    boost::optional<Vector>& direction = mc.direction();

    // Add the source to the dictionary
    const Point_or_location& input_source = mc.input_source();
    std::pair<DEC_it, bool> source;

    if(input_source.which() == 0) // Point
    {
      const Point& input_source_point = boost::get<Point>(input_source);
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Input source point: " << input_source_point << std::endl;
#endif
      Face_location source_location = locate(input_source_point, tree, vpm);
      source = points.insert(source_location, input_source_point, mesh_);
    }
    else // Face_location
    {
      CGAL_assertion(input_source.which() == 1);
      Face_location input_source_location = boost::get<Face_location>(input_source);
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Input source location fd: " << input_source_location.first
                << "bc: [" << input_source_location.second[0] << " "
                           << input_source_location.second[1] << " "
                           << input_source_location.second[2] << "]" << std::endl;
#endif
      source = points.insert(input_source_location, mesh_);
    }

    const FT time_at_source = mc.current_time();
    source.first->add_motorcycle(mc_id, time_at_source);
    mc.source() = source.first;
    mc.current_position() = source.first;

    // Compute if needed, and add the destination to the dictionary
    boost::optional<Point_or_location>& input_destination = mc.input_destination();
    std::pair<DEC_it, FT> destination = compute_destination(mc, input_destination);
    FT time_at_destination = destination.second;
    mc.destination() = destination.first;

    // Sanity checks: source and destination must be on the same face
    CGAL_postcondition(mc.source()->location().first == mc.destination()->location().first);
    CGAL_postcondition(PMP::is_in_face(mc.source()->location(), mesh_));
    CGAL_postcondition(PMP::is_in_face(mc.destination()->location(), mesh_));

    // Initialize the motorcycle targets queue
    mc.targets().insert(std::make_pair(mc.source(), time_at_source));

    if(mc.source() != mc.destination())
      mc.targets().insert(std::make_pair(mc.destination(), time_at_destination));

    // This is useful to not get an empty track when sour=dest
    // but it creates duplicates @fixme
    mc.track().insert(std::make_pair(mc.source(), mc.current_time()));

    // Compute the direction, if needed
    if(direction == boost::none)
    {
      mc.direction() = Vector(mc.source()->point(), mc.destination()->point());
      std::cout << "Computing direction from destination: " << *(mc.direction()) << std::endl;
    }

    // Sanity check: (destination - source) should be collinear with the direction
    Ray r(mc.source()->point(), *(mc.direction()));
    if(!r.has_on(mc.destination()->point()))
    {
      std::cerr << "Error: Incompatible destination and direction: " << std::endl
                << "- destination: " << mc.destination()->point() << std::endl
                << "- direction: " << *(mc.direction()) << std::endl;
      // the assertion below usually fails due to numerical errors, need an "almost_has_on"
#ifdef CGAL_POLYLINE_TRACING_ENABLE_RIGOROUS_PRECONDITIONS
       CGAL_assertion(false);
#endif
    }
  }
}

template<typename MotorcycleGraphTraits>
bool
Motorcycle_graph<MotorcycleGraphTraits>::
is_aabb_tree_needed() const
{
  // an AABB tree must be built if some sources are given as geometric points

  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t mc_id = 0; mc_id<number_of_motorcycles; ++mc_id)
  {
    const Motorcycle& mc = motorcycle(mc_id);
    if(mc.input_source().which() == 0) // input was given as a 'Point'
      return true;
  }

  return false;
}

template<typename MotorcycleGraphTraits>
bool
Motorcycle_graph<MotorcycleGraphTraits>::
is_motorcycle_position_blocked(const Motorcycle& mc) const
{
  namespace PMP = CGAL::Polygon_mesh_processing;

  if(mc.has_reached_blocked_point())
    return true;

  // to avoid self blocking while crossing mesh edges
  DEC_it position = mc.current_position();
  if(position->earliest_motorcycle()->first < mc.current_time())
    return true;

  return false;
}

template<typename MotorcycleGraphTraits>
typename Motorcycle_graph<MotorcycleGraphTraits>::Face_location
Motorcycle_graph<MotorcycleGraphTraits>::
locate(const Point& p, const AABB_tree& tree, const AABB_tree_VPM vpm) const
{
  namespace PMP = CGAL::Polygon_mesh_processing;

  // An AABB tree is a 3D structure, so we need to convert the point to a Point_3.
  // If the point is already a Point_3, this doesn't do anything.
  // @todo handle weird point types
  P2_or_P3_to_P3 to_p3;
  const typename P2_or_P3_to_P3::Point_3& source_point = to_p3(p);

  Face_location source_location = PMP::locate(source_point, tree, mesh_,
                                              parameters::vertex_point_map(vpm));

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
  // @tmp keep or not ?
  PMP::internal::snap_location_to_border<Triangle_mesh>(source_location);
#endif

  return source_location;
}

template<typename MotorcycleGraphTraits>
template<typename MotorcycleContainerIterator>
void
Motorcycle_graph<MotorcycleGraphTraits>::
trace_graph(MotorcycleContainerIterator mit, MotorcycleContainerIterator beyond)
{
  add_motorcycles(mit, beyond);
  initialize_motorcycles();
  motorcycle_pq.initialize(motorcycles);

#ifdef CGAL_MOTORCYCLE_GRAPH_OUTPUT
  output_motorcycles_sources_and_destinations();
#endif

  // this can only happen at the beginning, simpler to get it out the way immediately
  crash_motorcycles_with_same_source_and_direction();

  // @todo seperate initialization (above) and processing

  while(!motorcycle_pq.empty())
  {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << "---" << std::endl;
    std::cout << "Driving priority queue:" << std::endl << motorcycle_pq << std::endl;
#endif

    // get the earliest available event
    Motorcycle_PQE pqe = motorcycle_pq.top();
    Motorcycle& mc = pqe.motorcycle();

    // move the motorcycle to the target (which becomes the confirmed position)
    drive_to_closest_target(mc);

    if(mc.current_position() == mc.destination())
    {
      // Add the track source -- destination to the track map
      add_track_segment_to_map(mc.current_location().first, mc.id(),
                               mc.source(), mc.time_at_source(),
                               mc.destination(), mc.current_time());

      if(has_motorcycle_reached_final_destination(mc) ||
         has_motorcycle_reached_crashing_point(mc))
      {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
        std::cout << "Reached motorcycle's crashing point:" << std::endl
                  << " - final destination: " << has_motorcycle_reached_final_destination(mc) << std::endl
                  << " - blocked: " << is_motorcycle_position_blocked(mc) << std::endl
                  << " - simultaneous collision: " << mc.has_reached_simultaneous_collision_point() << std::endl;
#endif
        crash_motorcycle(mc);
      }
      // not crashing yet, try to compute the next path
      else
      {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
        std::cout << "Reached destination: " << mc.destination()->point();
        std::cout << " Now computing motorcycle's next path..." << std::endl;
#endif
        // clear any unnecessary targets that might have been built
        mc.clear_targets();

        if(compute_motorcycle_next_path(mc))
        {
          // a new path was found and set up, update the queue and continue
          motorcycle_pq.update(mc);

          // Note that we are not (yet) blocking the point in this case!!
          continue;
        }
        else
        {
          // couldn't find a next destination, crash the motorcycle
          crash_motorcycle(mc);
        }
      }
    }
    // the motorcycle has not reached its destination, but still might be crashing
    else if(has_motorcycle_reached_crashing_point(mc) &&
            // hackish to prevent multiple motorcycles starting from the same source
            // (but with different directions) from blocking each other
            // @fixme: can specify starting time ==> '0' is not acceptable
            // number of elements in the track == 0 ?
            mc.current_time() != 0.)
    {
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Reached crashing point:" << std::endl
                << " - blocked: " << is_motorcycle_position_blocked(mc) << std::endl
                << " - simultaneous collision: " << mc.has_reached_simultaneous_collision_point() << std::endl;
#endif
      // Add the track source -- crash position to the track map
      add_track_segment_to_map(mc.current_location().first, mc.id(),
                               mc.source(), mc.time_at_source(),
                               mc.current_position(), mc.current_time());

      crash_motorcycle(mc);
    }
    // the motorcycle can continue without issue towards its destination
    else
    {
      // check for potential collisions within the face for the next move of 'mc'
      Collision_information res = find_collision(mc);

#ifdef CGAL_MOTORCYCLE_GRAPH_ROBUSTNESS_CODE
      if(res.add_intersection_without_moving_motorcycles) // add an intersection without moving motorcycles
      {
        const FT tolerance = 2 * std::numeric_limits<FT>::epsilon();
        Motorcycle& fmc = motorcycle(res.fmc_id);
        DEC_it collision = res.closest_collision;
        visit_point(mc, fmc, collision, res.time_at_closest_collision,
                    res.foreign_time_at_closest_collision, tolerance);

        // - Re-add the current positions of the motorcycles to re-evaluate potential intersections in the path
        // - Update the priority queue for the two motorcycles
        if(res.closest_collision != mc.current_position())
          mc.add_target(mc.current_position(), mc.current_time());
        motorcycle_pq.update(mc);

        // Same for 'fmc'
        if(!fmc.is_crashed())
        {
          if(fmc.has_target(res.closest_collision).second && res.closest_collision != fmc.current_position())
            fmc.add_target(fmc.current_position(), fmc.current_time());

          motorcycle_pq.update(fmc);
        }

        continue;
      }
#endif

      if(res.found_collision())
        treat_collision(mc, res);
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      else
        std::cout << " No collision was found! " << std::endl;
#endif

      // The target list of 'mc' was modified and the PQ must be updated.
      // The PQ entry of 'fmc' is modified in 'treat_collision()', if needed.
      CGAL_assertion(!mc.is_crashed());
      motorcycle_pq.update(mc);

      // Block the current position
      mc.current_position()->block();
    }
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
treat_collision(Motorcycle& mc, const Collision_information& collision_info)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "+++++++++++ Treat collision [PREPROCESSING] +++++++++++" << std::endl;
#endif

  const std::size_t foreign_motorcycle_id = collision_info.fmc_id;
  Motorcycle& fmc = motorcycle(foreign_motorcycle_id);

  const face_descriptor fd = mc.current_location().first;
  const face_descriptor ffd =
    collision_info.is_foreign_motorcycle_in_different_face ? collision_info.foreign_motorcycle_face
                                                           : fd;

  CGAL_assertion(ffd != boost::graph_traits<Triangle_mesh>::null_face());
  CGAL_assertion((collision_info.is_foreign_motorcycle_in_different_face && fd != ffd) || fd == ffd);

  const FT time_at_collision = collision_info.time_at_closest_collision;
  const FT time_at_foreign_collision = collision_info.foreign_time_at_closest_collision;

  // Insert the collision point in the dictionary, if needed.
  DEC_it collision;
  if(!collision_info.is_closest_collision_already_in_dictionary)
  {
    // Motorcycle info will be added later.
    std::pair<DEC_it, bool> entry = points.insert(collision_info.closest_collision_location, mesh_);
    collision = entry.first;

    if(!entry.second)
    {
      std::cerr << "Warning: collision location actually already existed in the dictionary:"
                << std::endl << *(entry.first) << std::endl;
    }
  }
  else
  {
    collision = collision_info.closest_collision;
  }

  // Get the collision that is in 'fd'
  DEC_it collision_in_fd = collision;
  if(collision_in_fd->location().first != fd)
  {
    bool is_found;
    boost::tie(collision_in_fd, is_found) = points.get_sibling(collision_in_fd, fd);
    CGAL_assertion(is_found);
  }

  // Get the collision that is in 'ffd'
  DEC_it collision_in_ffd = collision;
  if(collision_in_ffd->location().first != ffd)
  {
    const Face_location& foreign_location = collision->sibling(ffd);
    std::pair<DEC_it, bool> foreign_collision_entry = points.find(foreign_location);
    CGAL_assertion(foreign_collision_entry.second); // must be found
    collision_in_ffd = foreign_collision_entry.first;
  }

  // Some sanity tests
  CGAL_postcondition(collision_in_fd->location().first == fd);
  CGAL_postcondition(collision_in_ffd->location().first == ffd);
  CGAL_postcondition_code(if(fd != ffd) {)
  CGAL_postcondition(collision_in_ffd->is_sibling(collision_in_fd->location()));
  CGAL_postcondition(collision_in_fd->is_sibling(collision_in_ffd->location())); // just to be extra sure
  CGAL_postcondition_code(})

  // Treat_collision handles all types of collisions
  treat_collision(mc, collision_in_fd, time_at_collision,
                  fmc, collision_in_ffd, time_at_foreign_collision);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
treat_collision(Motorcycle& mc, DEC_it collision_point, const FT time_at_collision_point,
                Motorcycle& fmc, DEC_it foreign_collision_point, const FT foreign_time_at_collision_point)
{
  // @todo factorize this a bit so we don't have multiple calls to "has_motorcycle"
  // followed by "add_motorcycle": this is all in log(n) complexity (admittedly,
  // log(n) on something that is unlikely to contain more than 2 elements, but still)

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "+++++++++++ Treat collision +++++++++++" << std::endl;
  std::cout << " - motorcycle:" << std::endl << mc << std::endl
            << " - foreign_motorcycle:" << std::endl << fmc << std::endl
            << " - time_at_collision_point: " << time_at_collision_point << std::endl
            << " - foreign_time_at_collision_point: " << foreign_time_at_collision_point << std::endl
            << " - collision_point: " << &*collision_point << std::endl << *(collision_point) << std::endl;
  if(collision_point != foreign_collision_point)
    std::cout << " - foreign collision_point: " << &*foreign_collision_point << std::endl << *(foreign_collision_point) << std::endl;
#endif

  // Some sanity checks
  CGAL_precondition(mc.id() != std::size_t(-1));
  CGAL_precondition(fmc.id() != std::size_t(-1));

  CGAL_precondition(collision_point != DEC_it());
  CGAL_precondition(foreign_collision_point != DEC_it());
  CGAL_precondition(collision_point->point() == foreign_collision_point->point());
  CGAL_precondition(collision_point->location().first == mc.current_location().first);

  // Can't give an upper bound on the (foreign_)time_at_collision due to front collisions
  CGAL_precondition(time_at_collision_point >= mc.current_time());

  if(// the impact is closer than the next target
     time_at_collision_point <= mc.time_at_closest_target() &&
     // the collision is not the next target of 'mc' or the foreign track
     // does not know the collision point yet
     (collision_point != mc.closest_target() ||
      !collision_point->has_motorcycle(fmc.id(), foreign_time_at_collision_point)))
  {
    if(!collision_point->has_motorcycle(mc.id(), time_at_collision_point))
    {
      // Call the halving structure to create a new point
      std::pair<DEC_it, FT> halving_entity =
        compute_halving_point(mc, mc.current_position(), mc.current_time(),
                              collision_point, time_at_collision_point);
      DEC_it halving_point = halving_entity.first;
      const FT time_at_halving_point = halving_entity.second;

      // Degeneracies should have been caught before
      CGAL_postcondition(halving_point != mc.current_position() &&
                         halving_point != collision_point);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Adding collision point: " << &*collision_point
                << " and halving point: " << &*halving_point
                << " to motorcycle #" << mc.id() << std::endl;
#endif

      mc.add_target(collision_point, time_at_collision_point);
      mc.add_target(halving_point, time_at_halving_point);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Adding motorcycle #" << mc.id()
                << " to collision point: " << &*collision_point
                << " and halving point: " << &*halving_point << std::endl;
#endif

      halving_point->add_motorcycle(mc.id(), time_at_halving_point);
      collision_point->add_motorcycle(mc.id(), time_at_collision_point);

      CGAL_postcondition(mc.has_target_at_time(collision_point, time_at_collision_point));
    }
    // If we have snapped the collision point to the current position, re-add it to the targets.
    // Note that we won't find the same intersection again because 'collision_point'
    // (which is 'mc.current_position') now combinatorially knows that there is
    // an intersection with the foreign motorcycle at 'mc.current_time' (and it will be ignored).
    // See "Check #1: known collision at current_position"
    else if(collision_point == mc.current_position())
    {
      mc.add_target(collision_point, time_at_collision_point);
    }

    // Now, do the same for the foreign motorcycle
    if(!foreign_collision_point->has_motorcycle(fmc.id(), foreign_time_at_collision_point) &&
       // ignore points that are farther than the current closest point
       // (otherwise you can get nasty stuff like halving points == existing points, etc.)
       (fmc.is_crashed() || foreign_time_at_collision_point <= fmc.time_at_closest_target()))
    {
      // It is useful to know that the collision point is on the foreign track,
      // even if the collision point is on the confirmed part of the track.
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
      std::cout << "Adding foreign motorcycle #" << fmc.id()
                << " to foreign collision point: " << &*foreign_collision_point << std::endl;
#endif
      foreign_collision_point->add_motorcycle(fmc.id(), foreign_time_at_collision_point);

      if(// the collision point is not on the confirmed track for the foreign mc
         foreign_time_at_collision_point > fmc.current_time()) // @fixme !is_crashed probably here
      {
        // Call the halving structure to create a new point
        std::pair<DEC_it, FT> foreign_halving_entity =
          compute_halving_point(fmc, fmc.current_position(), fmc.current_time(),
                                foreign_collision_point, foreign_time_at_collision_point);
        DEC_it foreign_halving_point = foreign_halving_entity.first;
        const FT foreign_time_at_halving_point = foreign_halving_entity.second;

        // Degeneracies should have been caught before
        CGAL_postcondition(foreign_halving_point != fmc.current_position() &&
                           foreign_halving_point != foreign_collision_point);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
        std::cout << "Adding foreign collision point: " << &*foreign_collision_point
                  << " and halving point: " << &*foreign_halving_point
                  << " to motorcycle #" << fmc.id() << std::endl;
#endif
        fmc.add_target(foreign_collision_point, foreign_time_at_collision_point);
        fmc.add_target(foreign_halving_point, foreign_time_at_halving_point);

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
        std::cout << "Adding foreign motorcycle #" << fmc.id()
                  << " to halving point: " << &*foreign_halving_point << std::endl;
#endif
        foreign_halving_point->add_motorcycle(fmc.id(), foreign_time_at_halving_point);

        // The target list of the foreign motorcycle was modified and the queue must be updated
        motorcycle_pq.update(fmc);

        CGAL_postcondition(fmc.has_target_at_time(foreign_collision_point, foreign_time_at_collision_point));
      }
      else
      {
        // New point for the foreign motorcycle, but it belongs to its confirmed track,
        // and must therefore be blocked
        foreign_collision_point->block();

        // Add it to the track of the foreign motorcycle (useful to check
        // the validity of the final graph)
        fmc.track().insert(std::make_pair(foreign_collision_point, foreign_time_at_collision_point));
      }
    }

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
    std::cout << std::endl << "[[ Post-treatment... ]]" << std::endl;
    std::cout << "Motorcycles involved: " << std::endl << mc << std::endl << fmc << std::endl;

    std::cout << "collision point:" << std::endl << *collision_point << std::endl;
    if(collision_point != foreign_collision_point)
      std::cout << "foreign collision point:" << std::endl << *foreign_collision_point << std::endl;
#endif
  }
}

template<typename MotorcycleGraphTraits>
std::pair<typename Motorcycle_graph<MotorcycleGraphTraits>::DEC_it, bool>
Motorcycle_graph<MotorcycleGraphTraits>::
try_to_snap_location_to_existing_point(const Face_location& location, const Point& p, const FT tolerance) const
{
  // The collision must not already be in the dictionary
  CGAL_expensive_precondition_code(!points.find(location).second);

  const face_descriptor fd = location.first;

  // Brute force for now, need an aabb tree of kd trees (a kd tree per face) @todo
  DEC_it dit = points.entries().begin(), end = points.entries().end();
  for(; dit!=end; ++dit)
  {
    if(dit->location().first != fd)
      continue;

    if(CGAL::squared_distance(dit->point(), p) <= tolerance)
    {
      // The new collision point is very close to an existing point, and we are going
      // to snap the position to that existing point, adding the following information:
      // - mc and fmc visit that point
      // - this point is visited by mc and fmc
      std::cout << "!!! new point: " << p << " is close enough to existing point: " << std::endl << *dit << std::endl;
      return std::make_pair(dit, true);
    }
  }

  return std::make_pair(DEC_it(), false);
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
visit_point(Motorcycle& mc, Motorcycle& fmc, DEC_it collision,
            FT time_at_collision, FT foreign_time_at_collision, const FT tolerance)
{
#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << " ---- Visiting point " << &*collision << std::endl << *collision << std::endl;
  std::cout << "with mc: " << std::endl << mc << std::endl;
  std::cout << "with fmc: " << std::endl << fmc << std::endl;
  std::cout << "times: " << time_at_collision << " || " << foreign_time_at_collision << std::endl;
#endif

  const bool are_times_equal = (CGAL::abs(foreign_time_at_collision - time_at_collision) <= tolerance);

  // First, check if 'mc' is known at the collision point
  FT min_visiting_time = time_at_collision - tolerance;
  FT max_visiting_time = time_at_collision + tolerance;
  FT visiting_time;
  bool is_visited_by_mc = collision->has_motorcycle(mc.id(), min_visiting_time, max_visiting_time, visiting_time);

  FT min_foreign_visiting_time = foreign_time_at_collision - tolerance;
  FT max_foreign_visiting_time = foreign_time_at_collision + tolerance;
  FT foreign_visiting_time;
  bool is_visited_by_fmc = collision->has_motorcycle(fmc.id(), min_foreign_visiting_time,
                                                     max_foreign_visiting_time, foreign_visiting_time);

  // Make sure that even if we snap, the times stay equal
  if(is_visited_by_fmc)
  {
    if(is_visited_by_mc)
    {
      if(are_times_equal) // visited by both, equal times
      {
        // Can't change times otherwise we create inconsistencies
        CGAL_assertion(CGAL::abs(foreign_time_at_collision - time_at_collision) <= tolerance);
      }
    }
    else if(are_times_equal)// only visited by fmc, equal times
      time_at_collision = foreign_visiting_time;
  }
  else if(is_visited_by_mc && are_times_equal) // only visited by mc, equal times
    foreign_time_at_collision = visiting_time;

  // Need to get the representants in the correct faces
  bool is_found;
  DEC_it collision_in_mc_face, collision_in_fmc_face;
  boost::tie(collision_in_mc_face, is_found) = points.get_sibling(collision, mc.current_location().first);
  CGAL_assertion(is_found);
  boost::tie(collision_in_fmc_face, is_found) = points.get_sibling(collision, fmc.current_location().first);
  CGAL_assertion(is_found);

  if(is_visited_by_mc)
  {
    // Consistency: there can't be a target that is ever so slightly before the current point
    CGAL_assertion(visiting_time >= mc.current_time());

    CGAL_assertion_code(if(visiting_time == mc.current_time()))
    CGAL_assertion(collision_in_mc_face == mc.current_position());
    CGAL_assertion_code(if(visiting_time == mc.time_at_closest_target()))
    CGAL_assertion(collision_in_mc_face == mc.closest_target());

    if(!mc.has_target(collision_in_mc_face).second) // nothing to do if it's already a target
      mc.add_target(collision_in_mc_face, visiting_time);
  }
  else // the snapping collision point is not yet visited by 'mc'
  {
    // Consistency: there can't be a target around that time: snapping should create safe zones around points
    CGAL_assertion(!mc.has_target_at_time(min_visiting_time, max_visiting_time).second);
    CGAL_assertion(time_at_collision > mc.current_time());
    CGAL_assertion(time_at_collision < mc.time_at_closest_target());

    mc.add_target(collision_in_mc_face, time_at_collision);
    collision_in_mc_face->add_motorcycle(mc.id(), time_at_collision);
  }

  // Same, for the foreign motorcycle 'fmc'
  CGAL_assertion(is_found);

  if(is_visited_by_fmc)
  {
    if(// can't add targets if the motorcycle is crashed
       !fmc.is_crashed() &&
       // nothing to do if it's already a target
       !fmc.has_target(collision_in_fmc_face).second &&
       // can't be a target if it's younger than the current time
       foreign_visiting_time >= fmc.current_time())
    {
      fmc.add_target(collision_in_fmc_face, foreign_visiting_time);
    }
  }
  else // the snapping collision point is not yet visited by 'fmc'
  {
    CGAL_assertion(!fmc.has_target_at_time(min_foreign_visiting_time, max_foreign_visiting_time).second);
    CGAL_assertion(foreign_time_at_collision <= fmc.time_at_destination());

    if(!fmc.is_crashed() && foreign_time_at_collision >= fmc.current_time())
      fmc.add_target(collision_in_fmc_face, foreign_time_at_collision);

    collision_in_fmc_face->add_motorcycle(fmc.id(), foreign_time_at_collision);

    // Check if the point is on the confirmed part of the foreign motorcycle's track
    if(foreign_time_at_collision <= fmc.current_time())
      collision_in_fmc_face->block();
  }

  CGAL_postcondition(mc.has_target(collision_in_mc_face).second);
  CGAL_postcondition(fmc.is_crashed() || foreign_time_at_collision < fmc.current_time() ||
                     fmc.has_target(collision_in_fmc_face).second);
  CGAL_postcondition(collision->has_motorcycle(mc.id()));
  CGAL_postcondition(collision->has_motorcycle(fmc.id()));

#ifdef CGAL_MOTORCYCLE_GRAPH_VERBOSE
  std::cout << "Post-visit: " << std::endl;
  std::cout << "collision: " << std::endl << *collision << std::endl;
  std::cout << "mc: " << std::endl << mc << std::endl;
  std::cout << "fmc: " << std::endl << fmc << std::endl;
#endif
}

template<typename MotorcycleGraphTraits>
bool
Motorcycle_graph<MotorcycleGraphTraits>::
is_valid() const
{
  // mega brute force validity check
  // @todo do something nice

  std::size_t number_of_motorcycles = motorcycles.size();
  for(std::size_t mc_id = 0; mc_id<number_of_motorcycles; ++mc_id)
  {
    const Motorcycle& mc = motorcycle(mc_id);
    const Track& mc_track = mc.track();
    CGAL_assertion(mc_track.size() > 0);
    if(mc_track.size() <= 2) // ignore degenerate tracks
      continue;

    typename Track::const_iterator tit = mc_track.begin(), end = mc_track.end();
    DEC_it current = tit->first, next;

    while(++tit != end)
    {
      next = tit->first;
      if(current->location().first != next->location().first)
      {
        std::cout << "Should be equal: " << current->point() << " and " << next->point() << std::endl;
        std::cout << "id: " << mc_id << std::endl;
        CGAL_assertion(CGAL::squared_distance(current->point(), next->point()) < std::numeric_limits<FT>::epsilon());
        current = next;
        continue;
      }

      if(current == next)
      {
        // current = next; // unneeded but left for clarity
        continue;
      }

      const Point_2 ts = gt.construct_point_2_object()(current->location().second[0],
                                                       current->location().second[1]);
      const Point_2 tt = gt.construct_point_2_object()(next->location().second[0],
                                                       next->location().second[1]);
      Segment_2 s = gt.construct_segment_2_object()(ts, tt);

      for(std::size_t fmc_id = 0; fmc_id<number_of_motorcycles; ++fmc_id)
      {
        if(fmc_id == mc_id)
          continue;

        const Motorcycle& fmc = motorcycle(fmc_id);
        const Track& fmc_track = fmc.track();
        CGAL_assertion(fmc_track.size() > 0);

        typename Track::const_iterator ftit = fmc_track.begin(), fend = fmc_track.end();
        DEC_it fcurrent = ftit->first, fnext = fcurrent;

        // degenerate fmc track
        if(fmc_track.size() == 1)
        {
          const Point_2 fts = gt.construct_point_2_object()(fcurrent->location().second[0],
                                                            fcurrent->location().second[1]);
          const Point_2 ftt = gt.construct_point_2_object()(fnext->location().second[0],
                                                            fnext->location().second[1]);
          Segment_2 fs = gt.construct_segment_2_object()(fts, ftt);

          if(gt.do_intersect_2_object()(s, fs))
          {
            std::cout << "Intersection ¤~~~~~~~~~~~~~~~~~¤" << std::endl;
            std::cout << "motorcycle #" << mc_id << " (track size: " << mc_track.size();
            std::cout << ") with motorcycle #" << fmc_id << " (track size: " << fmc_track.size() << ")" << std::endl;
            std::cout << "cu/ne: " << std::endl << current->point() << " ## " << next->point() << std::endl;
            std::cout << "fcu/fne: " << std::endl << fcurrent->point() << " ## " << fnext->point() << std::endl;
            std::cout << "DECITs:" << std::endl << &*current << std::endl << &*next << std::endl << &*fcurrent << std::endl << &*fnext << std::endl;
            std::cout << "BCS points: " << std::endl << ts << std::endl << tt << std::endl << fts << std::endl << ftt << std::endl;

            // Xor
            CGAL_assertion((current == fcurrent && next != fcurrent) ||
                           (current != fcurrent && next == fcurrent));
          }
        }

        while(++ftit != fend)
        {
          fnext = ftit->first;

          // different face locations
          if(current->location().first != fcurrent->location().first)
          {
            fcurrent = fnext;
            continue;
          }

          if(fcurrent->location().first != fnext->location().first)
          {
            std::cout << "Should be equal: " << fcurrent->point() << " and " << fnext->point() << std::endl;
            std::cout << "id: " << fmc_id << std::endl;
            CGAL_assertion(CGAL::squared_distance(fcurrent->point(), fnext->point()) < std::numeric_limits<FT>::epsilon());
            fcurrent = fnext;
            continue;
          }

          const Point_2 fts = gt.construct_point_2_object()(fcurrent->location().second[0],
                                                            fcurrent->location().second[1]);
          const Point_2 ftt = gt.construct_point_2_object()(fnext->location().second[0],
                                                            fnext->location().second[1]);
          Segment_2 fs = gt.construct_segment_2_object()(fts, ftt);

          if(gt.do_intersect_2_object()(s, fs))
          {
            std::cout << "Intersection ¤~~~~~~~~~~~~~~~~~¤ " << std::endl;
            std::cout << "motorcycle #" << mc_id << " (track size: " << mc_track.size();
            std::cout << ") with motorcycle #" << fmc_id << " (track size: " << fmc_track.size() << ")" << std::endl;
            std::cout << "DECITs:" << std::endl << *current << std::endl << *next << std::endl
                                                << *fcurrent << std::endl << *fnext << std::endl;

            // Check that the only possible intersection is an extremity
            if(fcurrent == fnext) // degenerate fmc track
            {
              CGAL_assertion((current == fcurrent && next != fcurrent) ||
                             (current != fcurrent && next == fcurrent));
            }
            else
            {
              CGAL_assertion((current == fcurrent && current != fnext && next != fcurrent && next != fnext) ||
                             (current != fcurrent && current == fnext && next != fcurrent && next != fnext) ||
                             (current != fcurrent && current != fnext && next == fcurrent && next != fnext) ||
                             (current != fcurrent && current != fnext && next != fcurrent && next == fnext));
            }

            // Any intersection that is not at the source must crash the motorcycle
            // if the motorcycle reaches this collision point at a later time
            // than another motorcycle. Thus, if there is an intersection at
            // 'next', 'next' must be the last track entry if the time is lower
            // for the other motorcycle.
            typename Track::const_iterator ftitb = ftit;
            // '->second' is the visiting time
            if((next == fcurrent && tit->second >= (--ftitb)->second) ||
               (next == fnext && tit->second >= ftit->second))
            {
              // should be the last item of the track
              if(tit != --(mc.track().end()))
              {
                // check for an end doublon created by snapping
                // @todo clean the tracks at the end of the algorithm...
                typename Track::const_iterator titb = tit;
                ++titb;
                if(*tit != *titb || titb == --(mc.track().end()))
                {
                  std::cout << "Motorcycle: " << std::endl << mc << std::endl;
                  std::cout << "should have been stopped at: " << std::endl << *next << std::endl;
                  std::cout << "by foreign motorcycle : " << std::endl << fmc << std::endl;
                  if(next == fcurrent)
                    std::cout << "times: " << tit->second << " vs " << ftitb->second << std::endl;
                  else
                    std::cout << "times: " << tit->second << " vs " << ftit->second << std::endl;
                  std::cout << "instead, it continued to: " << *(titb->first) << std::endl;
                  CGAL_assertion(false);
                }
              }
            }
          }
          fcurrent = fnext;
        }
      }
      current = next;
    }
  }

  return true;
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
output_all_dictionary_points() const
{
  typename Dictionary::Dictionary_entry_container::const_iterator dit = points.entries().begin();
  typename Dictionary::Dictionary_entry_container::const_iterator end = points.entries().end();

  std::stringstream oss;
  oss << "results_" << gt.dimension() << "/dictionary_points.xyz" << std::ends;
  std::ofstream os(oss.str().c_str());
  os.precision(20);

  for(; dit!=end; ++dit)
  {
    os << dit->point();
    if(gt.dimension() == 2) // The '.xyz' format expects 3D points
      os << " 0";
    os << '\n';
  }
}

template<typename MotorcycleGraphTraits>
void
Motorcycle_graph<MotorcycleGraphTraits>::
output_motorcycles_sources_and_destinations() const
{
  std::stringstream oss_sour, oss_dest;
  oss_sour << "results_" << gt.dimension() << "/motorcycles_sources.xyz" << std::ends;
  oss_dest << "results_" << gt.dimension() << "/motorcycles_destinations.xyz" << std::ends;
  std::ofstream oss(oss_sour.str().c_str());
  std::ofstream osd(oss_dest.str().c_str());
  oss.precision(17);
  osd.precision(17);

  for(std::size_t i=0; i<motorcycles.size(); ++i)
  {
    oss << motorcycle(i).source()->point();
    osd << motorcycle(i).destination()->point();

    if(gt.dimension() == 2) // The '.xyz' format expects 3D points
    {
      oss << " 0";
      osd << " 0";
    }

    oss << '\n';
    osd << '\n';
  }
}

} // namespace Polyline_tracing

} // namespace CGAL

#endif // CGAL_POLYLINE_TRACING_MOTORCYCLE_GRAPH_H
