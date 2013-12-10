#include <CGAL/Cartesian_d.h>
#include <CGAL/point_generators_d.h>
#include <CGAL/Extreme_points_d.h>
#include <CGAL/Extreme_points_traits_d.h>

typedef CGAL::Cartesian_d<double>     Kernel_d;
typedef Kernel_d::Point_d       Point_d;
typedef CGAL::Extreme_points_traits_d<Point_d>  EP_Traits_d;

void print_points(CGAL::Extreme_points_d<EP_Traits_d> ep) {
  std::vector<Point_d> extreme_points;
  ep.extreme_points(std::back_inserter(extreme_points));
  for (std::vector<Point_d>::iterator it=extreme_points.begin();
      it!=extreme_points.end();
      it++) {
    std::cout<<*it<<std::endl;
  }
}

int main() {
  const int D = 5;   // dimension
  const int N = 10;  // number of points for every batch
  const int BATCHES = 1; // number of batches
  
  CGAL::Extreme_points_options_d op;
  // Allows deletion of points after insertion.
  op.set_deletion(true);
  // Ensure termination in all cases at the price of worse performance.
  op.set_anti_cycling(true);
  CGAL::Extreme_points_d<EP_Traits_d> ep(D,op);
  
  // Generator for D-dimensional points with coordinates
  // in the range [-10, 10]
  CGAL::Random_points_in_cube_d<Point_d> gen (D, 10.);
  
  for (int i=0;i<BATCHES;++i) {
    std::cout<<"Inserting the points:"<<std::endl;
  
    // generate N points randomly from the D dimensional iso box
    std::vector<Point_d> points;
    for (int j=0;j<N;++j) {
      Point_d p = *gen++;
      std::cout<<p<<std::endl;
      points.push_back(p);
    }
  
    // add these points to the current point set maintained by ep
    ep.insert(points.begin(), points.end());
  
    // compute the extreme points
    std::cout<<"\nExtreme points of the current set: "<<std::endl;
    print_points(ep);
  
    // we can use classify to see whether
    // some specific point was extreme
    if (ep.classify(points[0], true) == CGAL::ON_BOUNDARY) {
      std::cout<<"The point \""<<points[0]
      <<"\" is an extreme point."<<std::endl;
    } else {
      std::cout<<"The point \""<<points[0]
      <<"\" is not an extreme point."<<std::endl;
    }
  
    // we can also classify some other random point
    Point_d p = *gen++;
    switch (ep.classify(p)) {
      case CGAL::ON_BOUNDED_SIDE:
        std::cout<<"The point \""<<p<<"\" is inside the "
        <<"convex hull of the current point set "
        <<"(but not an extreme point)."<<std::endl;
        break;
      case CGAL::ON_BOUNDARY:
        // the chance that this happens is practically zero..
        std::cout<<"The point \""<<p<<"\" is an extreme point "
        <<"of the current point set."<<std::endl;
        break;
      case CGAL::ON_UNBOUNDED_SIDE:
        std::cout<<"The point \""<<p<<"\" is outside the "
        <<"convex hull of the current point set."
        <<std::endl;
        break;
      default:
        std::cerr<<"Extreme_points_d<Point_d>::classify "
        <<"returned unexpected answer!"<<std::endl;
        assert(0);
    }
    std::cout<<std::endl;

    //delete a point
    ep.remove(*points.begin());
    std::cout<<"\nExtreme points of the current set after deletion: "<<std::endl;
    print_points(ep);
  }
  
  return 0;
}
