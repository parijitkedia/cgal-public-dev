#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>

#include <CGAL/Mesh_3/config.h>
#include <CGAL/Periodic_3_mesh_3/config.h>

#include <CGAL/Implicit_periodic_3_mesh_domain_3.h>
#include <CGAL/Periodic_3_mesh_facet_criteria_3.h>
#include <CGAL/Periodic_3_mesh_cell_criteria_3.h>
#include <CGAL/Periodic_3_mesh_criteria_3.h>
#include <CGAL/Periodic_3_mesh_triangulation_3.h>
#include <CGAL/IO/Medit_IO.h>
#include <CGAL/make_periodic_3_mesh_3.h>

#include <CGAL/Mesh_complex_3_in_triangulation_3.h>
#include <CGAL/Mesh_constant_domain_field_3.h>

namespace P3M3_IO = CGAL::Periodic_3_mesh_3::IO;

// Domain
typedef CGAL::Exact_predicates_inexact_constructions_kernel K;
typedef K::FT FT;
typedef K::Point_3 Point;
typedef FT (Function)(const Point&);
typedef CGAL::Implicit_periodic_3_mesh_domain_3<Function,K> Periodic_mesh_domain;

// Triangulation
typedef CGAL::Periodic_3_mesh_triangulation_3<Periodic_mesh_domain>::type Tr;
typedef CGAL::Mesh_complex_3_in_triangulation_3<Tr>                       C3t3;

// Criteria
typedef CGAL::Mesh_edge_criteria_3<Tr>                     Edge_criteria;
typedef CGAL::Periodic_3_mesh_facet_criteria_3<Tr>         Periodic_facet_criteria;
typedef CGAL::Periodic_3_mesh_cell_criteria_3<Tr>          Periodic_cell_criteria;
typedef CGAL::Periodic_3_mesh_criteria_3<Tr, Edge_criteria,
                                             Periodic_facet_criteria,
                                             Periodic_cell_criteria> Periodic_mesh_criteria;

// To avoid verbose function and named parameters call
using namespace CGAL::parameters;

const FT PI = std::acos(-1.);

// Function
FT sphere_function (const Point& p)
{ return CGAL::squared_distance(p, Point(0.5, 0.5, 0.5)) - 0.2; }

FT schwarz_p(const Point& p) {
  const FT x2 = std::cos( p.x() * 2*PI ),
           y2 = std::cos( p.y() * 2*PI ),
           z2 = std::cos( p.z() * 2*PI );
  return x2 + y2 + z2;
}

typedef CGAL::Mesh_constant_domain_field_3<Periodic_mesh_domain::R,
                                           Periodic_mesh_domain::Index> Field;

int main(int argc, char** argv)
{
  // 'int' because the 'schwarz_p' function is periodic over the domain only if
  // the length of the side of the domain is an integer
  int domain_size = 1;

  if (argc > 1)
    domain_size = atoi(argv[1]);

  Periodic_mesh_domain domain(schwarz_p, CGAL::Iso_cuboid_3<K>(0, 0, 0, domain_size, domain_size, domain_size));

  Periodic_mesh_criteria criteria(domain,
                                  facet_angle = 30,
                                  facet_size = 0.05 * domain_size,
                                  facet_distance = 0.025 * domain_size,
                                  cell_radius_edge_ratio = 2.,
                                  cell_size = 0.05);

  // Mesh generation
  C3t3 c3t3 = CGAL::make_periodic_3_mesh_3<C3t3>(domain, criteria);

  // Output
  std::ofstream medit_file("out_implicit_shape.mesh");
  P3M3_IO::write_complex_to_medit(medit_file, c3t3);

  std::cout << "EXIT SUCCESS" << std::endl;
  return 0;
}

