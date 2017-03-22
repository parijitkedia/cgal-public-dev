// Copyright (c) 2016-2017 INRIA Nancy Grand-Est (France).
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
// $URL$
// $Id$
//
// Author(s)     : Iordan Iordanov <iordan.iordanov@loria.fr>

#include <boost/tuple/tuple.hpp>
#include <boost/random/linear_congruential.hpp>
#include <boost/random/uniform_smallint.hpp>
#include <boost/random/variate_generator.hpp>

#include <CGAL/Periodic_4_hyperbolic_Delaunay_triangulation_2.h>
#include <CGAL/Periodic_4_hyperbolic_Delaunay_triangulation_traits_2.h>
#include <CGAL/Periodic_4_hyperbolic_triangulation_dummy_14.h>
#include <CGAL/Algebraic_kernel_for_circles_2_2.h>
#include <CGAL/Circular_kernel_2.h>
#include <CGAL/CORE_Expr.h>
#include <CGAL/Cartesian.h>
#include <CGAL/determinant.h>


typedef CORE::Expr                                              				NT;				
typedef CGAL::Algebraic_kernel_for_circles_2_2<NT>                              AK;   // Algebraic kernel  
typedef CGAL::Cartesian<NT>                                                     BK;   // Basic kernel
typedef CGAL::Circular_kernel_2<BK, AK>                                         Kernel;
typedef CGAL::Periodic_4_hyperbolic_Delaunay_triangulation_traits_2<Kernel>     Traits;
typedef CGAL::Periodic_4_hyperbolic_Delaunay_triangulation_2<Traits>            Triangulation;
typedef Triangulation::Face_iterator                                            Face_iterator;

int main(void) {

    Triangulation tr;    
    tr.insert_dummy_points();
    
    cout << "Triangulation successfully initialized with dummy points!" << endl << "---------------------------------------------" << endl;
    cout << "Number of vertices:                  " << tr.number_of_vertices() << endl;
    cout << "Number of faces:                     " << tr.number_of_faces() << endl;
    cout << "Number of edges:                     " << tr.number_of_edges() << endl;
    cout << "Expected edges (by Euler relation):  " << tr.number_of_vertices() + tr.number_of_faces() + 2 << endl;

    assert(tr.is_valid(true));

    return 0;
}