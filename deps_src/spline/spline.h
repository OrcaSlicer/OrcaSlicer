/*
 * spline.h
 *
 * simple cubic spline interpolation library without external
 * dependencies
 *
 * ---------------------------------------------------------------------
 * Copyright (C) 2011, 2014, 2016, 2021 Tino Kluge (ttk448 at gmail.com)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * ---------------------------------------------------------------------
 *
 */


#ifndef TK_SPLINE_H
#define TK_SPLINE_H

#include <cstdio>
#include <cassert>
#include <cmath>
#include <vector>
#include <algorithm>
#ifdef HAVE_SSTREAM
#include <sstream>
#include <string>
#endif // HAVE_SSTREAM

// not ideal but disable unused-function warnings
// (we get them because we have implementations in the header file,
// and this is because we want to be able to quickly separate them
// into a cpp file if necessary)
#if !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace tk
{

// spline interpolation
class spline
{
public:
    // spline types
    enum spline_type {
        linear = 10,            // linear interpolation
        cspline = 30,           // cubic splines (classical C^2)
        cspline_hermite = 31    // cubic hermite splines (local, only C^1)
    };

    // boundary condition type for the spline end-points
    enum bd_type {
        first_deriv = 1,
        second_deriv = 2,
        not_a_knot = 3
    };

protected:
    std::vector<double> m_x,m_y;            // x,y coordinates of points
    // interpolation parameters
    // f(x) = a_i + b_i*(x-x_i) + c_i*(x-x_i)^2 + d_i*(x-x_i)^3
    // where a_i = y_i, or else it won't go through grid points
    std::vector<double> m_b,m_c,m_d;        // spline coefficients
    double m_c0;                            // for left extrapolation
    spline_type m_type;
    bd_type m_left, m_right;
    double  m_left_value, m_right_value;
    bool m_made_monotonic;
    void set_coeffs_from_b();               // calculate c_i, d_i from b_i
    size_t find_closest(double x) const;    // closest idx so that m_x[idx]<=x

public:
    // default constructor: set boundary condition to be zero curvature
    // at both ends, i.e. natural splines
    spline(): m_type(cspline),
        m_left(second_deriv), m_right(second_deriv),
        m_left_value(0.0), m_right_value(0.0), m_made_monotonic(false)
    {
        ;
    }
    spline(const std::vector<double>& X, const std::vector<double>& Y,
           spline_type type = cspline,
           bool make_monotonic = false,
           bd_type left  = second_deriv, double left_value  = 0.0,
           bd_type right = second_deriv, double right_value = 0.0
          ):
        m_type(type),
        m_left(left), m_right(right),
        m_left_value(left_value), m_right_value(right_value),
        m_made_monotonic(false) // false correct here: make_monotonic() sets it
    {
        this->set_points(X,Y,m_type);
        if(make_monotonic) {
            this->make_monotonic();
        }
    }


    // modify boundary conditions: if called it must be before set_points()
    void set_boundary(bd_type left, double left_value,
                      bd_type right, double right_value);

    // set all data points (cubic_spline=false means linear interpolation)
    void set_points(const std::vector<double>& x,
                    const std::vector<double>& y,
                    spline_type type=cspline);

    // adjust coefficients so that the spline becomes piecewise monotonic
    // where possible
    //   this is done by adjusting slopes at grid points by a non-negative
    //   factor and this will break C^2
    //   this can also break boundary conditions if adjustments need to
    //   be made at the boundary points
    // returns false if no adjustments have been made, true otherwise
    bool make_monotonic();

    // evaluates the spline at point x
    double operator() (double x) const;
    double deriv(int order, double x) const;

    // solves for all x so that: spline(x) = y
    std::vector<double> solve(double y, bool ignore_extrapolation=true) const;

    // returns the input data points
    std::vector<double> get_x() const { return m_x; }
    std::vector<double> get_y() const { return m_y; }
    double get_x_min() const { assert(!m_x.empty()); return m_x.front(); }
    double get_x_max() const { assert(!m_x.empty()); return m_x.back(); }

#ifdef HAVE_SSTREAM
    // spline info string, i.e. spline type, boundary conditions etc.
    std::string info() const;
#endif // HAVE_SSTREAM

};



namespace internal
{

// band matrix solver
class band_matrix
{
private:
    std::vector< std::vector<double> > m_upper;  // upper band
    std::vector< std::vector<double> > m_lower;  // lower band
public:
    band_matrix() {};                             // constructor
    band_matrix(int dim, int n_u, int n_l);       // constructor
    ~band_matrix() {};                            // destructor
    void resize(int dim, int n_u, int n_l);      // init with dim,n_u,n_l
    int dim() const;                             // matrix dimension
    int num_upper() const
    {
        return (int)m_upper.size()-1;
    }
    int num_lower() const
    {
        return (int)m_lower.size()-1;
    }
    // access operator
    double & operator () (int i, int j);            // write
    double   operator () (int i, int j) const;      // read
    // we can store an additional diagonal (in m_lower)
    double& saved_diag(int i);
    double  saved_diag(int i) const;
    void lu_decompose();
    std::vector<double> r_solve(const std::vector<double>& b) const;
    std::vector<double> l_solve(const std::vector<double>& b) const;
    std::vector<double> lu_solve(const std::vector<double>& b,
                                 bool is_lu_decomposed=false);

};

double get_eps();

std::vector<double> solve_cubic(double a, double b, double c, double d,
                                int newton_iter=0);

} // namespace internal




// ---------------------------------------------------------------------
// implementation part, which could be separated into a cpp file
// ---------------------------------------------------------------------

// spline implementation
// -----------------------

void spline::set_boundary(spline::bd_type left, double left_value,
                          spline::bd_type right, double right_value)
{
    assert(m_x.size()==0);          // set_points() must not have happened yet
    m_left=left;
    m_right=right;
    m_left_value=left_value;
    m_right_value=right_value;
}


void spline::set_coeffs_from_b()
{
    assert(m_x.size()==m_y.size());
    assert(m_x.size()==m_b.size());
    assert(m_x.size()>2);
    size_t n=m_b.size();
    if(m_c.size()!=n)
        m_c.resize(n);
    if(m_d.size()!=n)
        m_d.resize(n);

    for(size_t i=0; i<n-1; i++) {
        const double h  = m_x[i+1]-m_x[i];
        // from continuity and differentiability condition
        m_c[i] = ( 3.0*(m_y[i+1]-m_y[i])/h - (2.0*m_b[i]+m_b[i+1]) ) / h;
        // from differentiability condition
        m_d[i] = ( (m_b[i+1]-m_b[i])/(3.0*h) - 2.0/3.0*m_c[i] ) / h;
    }

    // for left extrapolation coefficients
    m_c0 = (m_left==first_deriv) ? 0.0 : m_c[0];
}

void spline::set_points(const std::vector<double>& x,
                        const std::vector<double>& y,
                        spline_type type)
{
    assert(x.size()==y.size());
    assert(x.size()>=3);
    // not-a-knot with 3 points has many solutions
    if(m_left==not_a_knot || m_right==not_a_knot)
        assert(x.size()>=4);
    m_type=type;
    m_made_monotonic=false;
    m_x=x;
    m_y=y;
    int n = (int) x.size();
    // check strict monotonicity of input vector x
    for(int i=0; i<n-1; i++) {
        assert(m_x[i]<m_x[i+1]);
    }


    if(type==linear) {
        // linear interpolation
        m_d.resize(n);
        m_c.resize(n);
        m_b.resize(n);
        for(int i=0; i<n-1; i++) {
            m_d[i]=0.0;
            m_c[i]=0.0;
            m_b[i]=(m_y[i+1]-m_y[i])/(m_x[i+1]-m_x[i]);
        }
        // ignore boundary conditions, set slope equal to the last segment
        m_b[n-1]=m_b[n-2];
        m_c[n-1]=0.0;
        m_d[n-1]=0.0;
    } else if(type==cspline) {
        // classical cubic splines which are C^2 (twice cont differentiable)
        // this requires solving an equation system

        // setting up the matrix and right hand side of the equation system
        // for the parameters b[]
        int n_upper = (m_left  == spline::not_a_knot) ? 2 : 1;
        int n_lower = (m_right == spline::not_a_knot) ? 2 : 1;
        internal::band_matrix A(n,n_upper,n_lower);
        std::vector<double>  rhs(n);
        for(int i=1; i<n-1; i++) {
            A(i,i-1)=1.0/3.0*(x[i]-x[i-1]);
            A(i,i)=2.0/3.0*(x[i+1]-x[i-1]);
            A(i,i+1)=1.0/3.0*(x[i+1]-x[i]);
            rhs[i]=(y[i+1]-y[i])/(x[i+1]-x[i]) - (y[i]-y[i-1])/(x[i]-x[i-1]);
        }
        // boundary conditions
        if(m_left == spline::second_deriv) {
            // 2*c[0] = f''
            A(0,0)=2.0;
            A(0,1)=0.0;
            rhs[0]=m_left_value;
        } else if(m_left == spline::first_deriv) {
            // b[0] = f', needs to be re-expressed in terms of c:
            // (2c[0]+c[1])(x[1]-x[0]) = 3 ((y[1]-y[0])/(x[1]-x[0]) - f')
            A(0,0)=2.0*(x[1]-x[0]);
            A(0,1)=1.0*(x[1]-x[0]);
            rhs[0]=3.0*((y[1]-y[0])/(x[1]-x[0])-m_left_value);
        } else if(m_left == spline::not_a_knot) {
            // f'''(x[1]) exists, i.e. d[0]=d[1], or re-expressed in c:
            // -h1*c[0] + (h0+h1)*c[1] - h0*c[2] = 0
            A(0,0) = -(x[2]-x[1]);
            A(0,1) = x[2]-x[0];
            A(0,2) = -(x[1]-x[0]);
            rhs[0] = 0.0;
        } else {
            assert(false);
        }
        if(m_right == spline::second_deriv) {
            // 2*c[n-1] = f''
            A(n-1,n-1)=2.0;
            A(n-1,n-2)=0.0;
            rhs[n-1]=m_right_value;
        } else if(m_right == spline::first_deriv) {
            // b[n-1] = f', needs to be re-expressed in terms of c:
            // (c[n-2]+2c[n-1])(x[n-1]-x[n-2])
            // = 3 (f' - (y[n-1]-y[n-2])/(x[n-1]-x[n-2]))
            A(n-1,n-1)=2.0*(x[n-1]-x[n-2]);
            A(n-1,n-2)=1.0*(x[n-1]-x[n-2]);
            rhs[n-1]=3.0*(m_right_value-(y[n-1]-y[n-2])/(x[n-1]-x[n-2]));
        } else if(m_right == spline::not_a_knot) {
            // f'''(x[n-2]) exists, i.e. d[n-3]=d[n-2], or re-expressed in c:
            // -h_{n-2}*c[n-3] + (h_{n-3}+h_{n-2})*c[n-2] - h_{n-3}*c[n-1] = 0
            A(n-1,n-3) = -(x[n-1]-x[n-2]);
            A(n-1,n-2) = x[n-1]-x[n-3];
            A(n-1,n-1) = -(x[n-2]-x[n-3]);
            rhs[0] = 0.0;
        } else {
            assert(false);
        }

        // solve the equation system to obtain the parameters c[]
        m_c=A.lu_solve(rhs);

        // calculate parameters b[] and d[] based on c[]
        m_d.resize(n);
        m_b.resize(n);
        for(int i=0; i<n-1; i++) {
            m_d[i]=1.0/3.0*(m_c[i+1]-m_c[i])/(x[i+1]-x[i]);
            m_b[i]=(y[i+1]-y[i])/(x[i+1]-x[i])
                   - 1.0/3.0*(2.0*m_c[i]+m_c[i+1])*(x[i+1]-x[i]);
        }
        // for the right extrapolation coefficients (zero cubic term)
        // f_{n-1}(x) = y_{n-1} + b*(x-x_{n-1}) + c*(x-x_{n-1})^2
        double h=x[n-1]-x[n-2];
        // m_c[n-1] is determined by the boundary condition
        m_d[n-1]=0.0;
        m_b[n-1]=3.0*m_d[n-2]*h*h+2.0*m_c[n-2]*h+m_b[n-2];   // = f'_{n-2}(x_{n-1})
        if(m_right==first_deriv)
            m_c[n-1]=0.0;   // force linear extrapolation

    } else if(type==cspline_hermite) {
        // hermite cubic splines which are C^1 (cont. differentiable)
        // and derivatives are specified on each grid point
        // (here we use 3-point finite differences)
        m_b.resize(n);
        m_c.resize(n);
        m_d.resize(n);
        // set b to match 1st order derivative finite difference
        for(int i=1; i<n-1; i++) {
            const double h  = m_x[i+1]-m_x[i];
            const double hl = m_x[i]-m_x[i-1];
            m_b[i] = -h/(hl*(hl+h))*m_y[i-1] + (h-hl)/(hl*h)*m_y[i]
                     +  hl/(h*(hl+h))*m_y[i+1];
        }
        // boundary conditions determine b[0] and b[n-1]
        if(m_left==first_deriv) {
            m_b[0]=m_left_value;
        } else if(m_left==second_deriv) {
            const double h = m_x[1]-m_x[0];
            m_b[0]=0.5*(-m_b[1]-0.5*m_left_value*h+3.0*(m_y[1]-m_y[0])/h);
        } else if(m_left == not_a_knot) {
            // f''' continuous at x[1]
            const double h0 = m_x[1]-m_x[0];
            const double h1 = m_x[2]-m_x[1];
            m_b[0]= -m_b[1] + 2.0*(m_y[1]-m_y[0])/h0
                    + h0*h0/(h1*h1)*(m_b[1]+m_b[2]-2.0*(m_y[2]-m_y[1])/h1);
        } else {
            assert(false);
        }
        if(m_right==first_deriv) {
            m_b[n-1]=m_right_value;
            m_c[n-1]=0.0;
        } else if(m_right==second_deriv) {
            const double h = m_x[n-1]-m_x[n-2];
            m_b[n-1]=0.5*(-m_b[n-2]+0.5*m_right_value*h+3.0*(m_y[n-1]-m_y[n-2])/h);
            m_c[n-1]=0.5*m_right_value;
        } else if(m_right == not_a_knot) {
            // f''' continuous at x[n-2]
            const double h0 = m_x[n-2]-m_x[n-3];
            const double h1 = m_x[n-1]-m_x[n-2];
            m_b[n-1]= -m_b[n-2] + 2.0*(m_y[n-1]-m_y[n-2])/h1 + h1*h1/(h0*h0)
                      *(m_b[n-3]+m_b[n-2]-2.0*(m_y[n-2]-m_y[n-3])/h0);
            // f'' continuous at x[n-1]: c[n-1] = 3*d[n-2]*h[n-2] + c[n-1]
            m_c[n-1]=(m_b[n-2]+2.0*m_b[n-1])/h1-3.0*(m_y[n-1]-m_y[n-2])/(h1*h1);
        } else {
            assert(false);
        }
        m_d[n-1]=0.0;

        // parameters c and d are determined by continuity and differentiability
        set_coeffs_from_b();

    } else {
        assert(false);
    }

    // for left extrapolation coefficients
    m_c0 = (m_left==first_deriv) ? 0.0 : m_c[0];
}

bool spline::make_monotonic()
{
    assert(m_x.size()==m_y.size());
    assert(m_x.size()==m_b.size());
    assert(m_x.size()>2);
    bool modified = false;
    const int n=(int)m_x.size();
    // make sure: input data monotonic increasing --> b_i>=0
    //            input data monotonic decreasing --> b_i<=0
    for(int i=0; i<n; i++) {
        int im1 = std::max(i-1, 0);
        int ip1 = std::min(i+1, n-1);
        if( ((m_y[im1]<=m_y[i]) && (m_y[i]<=m_y[ip1]) && m_b[i]<0.0) ||
            ((m_y[im1]>=m_y[i]) && (m_y[i]>=m_y[ip1]) && m_b[i]>0.0) ) {
            modified=true;
            m_b[i]=0.0;
        }
    }
    // if input data is monotonic (b[i], b[i+1], avg have all the same sign)
    // ensure a sufficient criteria for monotonicity is satisfied:
    //     sqrt(b[i]^2+b[i+1]^2) <= 3 |avg|, with avg=(y[i+1]-y[i])/h,
    for(int i=0; i<n-1; i++) {
        double h = m_x[i+1]-m_x[i];
        double avg = (m_y[i+1]-m_y[i])/h;
        if( avg==0.0 && (m_b[i]!=0.0 || m_b[i+1]!=0.0) ) {
            modified=true;
            m_b[i]=0.0;
            m_b[i+1]=0.0;
        } else if( (m_b[i]>=0.0 && m_b[i+1]>=0.0 && avg>0.0) ||
                   (m_b[i]<=0.0 && m_b[i+1]<=0.0 && avg<0.0) ) {
            // input data is monotonic
            double r = sqrt(m_b[i]*m_b[i]+m_b[i+1]*m_b[i+1])/std::fabs(avg);
            if(r>3.0) {
                // sufficient criteria for monotonicity: r<=3
                // adjust b[i] and b[i+1]
                modified=true;
                m_b[i]   *= (3.0/r);
                m_b[i+1] *= (3.0/r);
            }
        }
    }

    if(modified==true) {
        set_coeffs_from_b();
        m_made_monotonic=true;
    }

    return modified;
}

// return the closest idx so that m_x[idx] <= x (return 0 if x<m_x[0])
size_t spline::find_closest(double x) const
{
    std::vector<double>::const_iterator it;
    it=std::upper_bound(m_x.begin(),m_x.end(),x);       // *it > x
    size_t idx = std::max( int(it-m_x.begin())-1, 0);   // m_x[idx] <= x
    return idx;
}

double spline::operator() (double x) const
{
    // polynomial evaluation using Horner's scheme
    // TODO: consider more numerically accurate algorithms, e.g.:
    //   - Clenshaw
    //   - Even-Odd method by A.C.R. Newbery
    //   - Compensated Horner Scheme
    size_t n=m_x.size();
    size_t idx=find_closest(x);

    double h=x-m_x[idx];
    double interpol;
    if(x<m_x[0]) {
        // extrapolation to the left
        interpol=(m_c0*h + m_b[0])*h + m_y[0];
    } else if(x>m_x[n-1]) {
        // extrapolation to the right
        interpol=(m_c[n-1]*h + m_b[n-1])*h + m_y[n-1];
    } else {
        // interpolation
        interpol=((m_d[idx]*h + m_c[idx])*h + m_b[idx])*h + m_y[idx];
    }
    return interpol;
}

double spline::deriv(int order, double x) const
{
    assert(order>0);
    size_t n=m_x.size();
    size_t idx = find_closest(x);

    double h=x-m_x[idx];
    double interpol;
    if(x<m_x[0]) {
        // extrapolation to the left
        switch(order) {
        case 1:
            interpol=2.0*m_c0*h + m_b[0];
            break;
        case 2:
            interpol=2.0*m_c0;
            break;
        default:
            interpol=0.0;
            break;
        }
    } else if(x>m_x[n-1]) {
        // extrapolation to the right
        switch(order) {
        case 1:
            interpol=2.0*m_c[n-1]*h + m_b[n-1];
            break;
        case 2:
            interpol=2.0*m_c[n-1];
            break;
        default:
            interpol=0.0;
            break;
        }
    } else {
        // interpolation
        switch(order) {
        case 1:
            interpol=(3.0*m_d[idx]*h + 2.0*m_c[idx])*h + m_b[idx];
            break;
        case 2:
            interpol=6.0*m_d[idx]*h + 2.0*m_c[idx];
            break;
        case 3:
            interpol=6.0*m_d[idx];
            break;
        default:
            interpol=0.0;
            break;
        }
    }
    return interpol;
}

std::vector<double> spline::solve(double y, bool ignore_extrapolation) const
{
    std::vector<double> x;          // roots for the entire spline
    std::vector<double> root;       // roots for each piecewise cubic
    const size_t n=m_x.size();

    // left extrapolation
    if(ignore_extrapolation==false) {
        root = internal::solve_cubic(m_y[0]-y,m_b[0],m_c0,0.0,1);
        for(size_t j=0; j<root.size(); j++) {
            if(root[j]<0.0) {
                x.push_back(m_x[0]+root[j]);
            }
        }
    }

    // brute force check if piecewise cubic has roots in their resp. segment
    // TODO: make more efficient
    for(size_t i=0; i<n-1; i++) {
        root = internal::solve_cubic(m_y[i]-y,m_b[i],m_c[i],m_d[i],1);
        for(size_t j=0; j<root.size(); j++) {
            double h = (i>0) ? (m_x[i]-m_x[i-1]) : 0.0;
            double eps = internal::get_eps()*512.0*std::min(h,1.0);
            if( (-eps<=root[j]) && (root[j]<m_x[i+1]-m_x[i]) ) {
                double new_root = m_x[i]+root[j];
                if(x.size()>0 && x.back()+eps > new_root) {
                    x.back()=new_root;      // avoid spurious duplicate roots
                } else {
                    x.push_back(new_root);
                }
            }
        }
    }

    // right extrapolation
    if(ignore_extrapolation==false) {
        root = internal::solve_cubic(m_y[n-1]-y,m_b[n-1],m_c[n-1],0.0,1);
        for(size_t j=0; j<root.size(); j++) {
            if(0.0<=root[j]) {
                x.push_back(m_x[n-1]+root[j]);
            }
        }
    }

    return x;
};


#ifdef HAVE_SSTREAM
std::string spline::info() const
{
    std::stringstream ss;
    ss << "type " << m_type << ", left boundary deriv " << m_left << " = ";
    ss << m_left_value << ", right boundary deriv " << m_right << " = ";
    ss << m_right_value << std::endl;
    if(m_made_monotonic) {
        ss << "(spline has been adjusted for piece-wise monotonicity)";
    }
    return ss.str();
}
#endif // HAVE_SSTREAM


namespace internal
{

// band_matrix implementation
// -------------------------

band_matrix::band_matrix(int dim, int n_u, int n_l)
{
    resize(dim, n_u, n_l);
}
void band_matrix::resize(int dim, int n_u, int n_l)
{
    assert(dim>0);
    assert(n_u>=0);
    assert(n_l>=0);
    m_upper.resize(n_u+1);
    m_lower.resize(n_l+1);
    for(size_t i=0; i<m_upper.size(); i++) {
        m_upper[i].resize(dim);
    }
    for(size_t i=0; i<m_lower.size(); i++) {
        m_lower[i].resize(dim);
    }
}
int band_matrix::dim() const
{
    if(m_upper.size()>0) {
        return (int)m_upper[0].size();
    } else {
        return 0;
    }
}


// defines the new operator (), so that we can access the elements
// by A(i,j), index going from i=0,...,dim()-1
double & band_matrix::operator () (int i, int j)
{
    int k=j-i;       // what band is the entry
    assert( (i>=0) && (i<dim()) && (j>=0) && (j<dim()) );
    assert( (-num_lower()<=k) && (k<=num_upper()) );
    // k=0 -> diagonal, k<0 lower left part, k>0 upper right part
    if(k>=0)    return m_upper[k][i];
    else        return m_lower[-k][i];
}
double band_matrix::operator () (int i, int j) const
{
    int k=j-i;       // what band is the entry
    assert( (i>=0) && (i<dim()) && (j>=0) && (j<dim()) );
    assert( (-num_lower()<=k) && (k<=num_upper()) );
    // k=0 -> diagonal, k<0 lower left part, k>0 upper right part
    if(k>=0)    return m_upper[k][i];
    else        return m_lower[-k][i];
}
// second diag (used in LU decomposition), saved in m_lower
double band_matrix::saved_diag(int i) const
{
    assert( (i>=0) && (i<dim()) );
    return m_lower[0][i];
}
double & band_matrix::saved_diag(int i)
{
    assert( (i>=0) && (i<dim()) );
    return m_lower[0][i];
}

// LR-Decomposition of a band matrix
void band_matrix::lu_decompose()
{
    int  i_max,j_max;
    int  j_min;
    double x;

    // preconditioning
    // normalize column i so that a_ii=1
    for(int i=0; i<this->dim(); i++) {
        assert(this->operator()(i,i)!=0.0);
        this->saved_diag(i)=1.0/this->operator()(i,i);
        j_min=std::max(0,i-this->num_lower());
        j_max=std::min(this->dim()-1,i+this->num_upper());
        for(int j=j_min; j<=j_max; j++) {
            this->operator()(i,j) *= this->saved_diag(i);
        }
        this->operator()(i,i)=1.0;          // prevents rounding errors
    }

    // Gauss LR-Decomposition
    for(int k=0; k<this->dim(); k++) {
        i_max=std::min(this->dim()-1,k+this->num_lower());  // num_lower not a mistake!
        for(int i=k+1; i<=i_max; i++) {
            assert(this->operator()(k,k)!=0.0);
            x=-this->operator()(i,k)/this->operator()(k,k);
            this->operator()(i,k)=-x;                         // assembly part of L
            j_max=std::min(this->dim()-1,k+this->num_upper());
            for(int j=k+1; j<=j_max; j++) {
                // assembly part of R
                this->operator()(i,j)=this->operator()(i,j)+x*this->operator()(k,j);
            }
        }
    }
}
// solves Ly=b
std::vector<double> band_matrix::l_solve(const std::vector<double>& b) const
{
    assert( this->dim()==(int)b.size() );
    std::vector<double> x(this->dim());
    int j_start;
    double sum;
    for(int i=0; i<this->dim(); i++) {
        sum=0;
        j_start=std::max(0,i-this->num_lower());
        for(int j=j_start; j<i; j++) sum += this->operator()(i,j)*x[j];
        x[i]=(b[i]*this->saved_diag(i)) - sum;
    }
    return x;
}
// solves Rx=y
std::vector<double> band_matrix::r_solve(const std::vector<double>& b) const
{
    assert( this->dim()==(int)b.size() );
    std::vector<double> x(this->dim());
    int j_stop;
    double sum;
    for(int i=this->dim()-1; i>=0; i--) {
        sum=0;
        j_stop=std::min(this->dim()-1,i+this->num_upper());
        for(int j=i+1; j<=j_stop; j++) sum += this->operator()(i,j)*x[j];
        x[i]=( b[i] - sum ) / this->operator()(i,i);
    }
    return x;
}

std::vector<double> band_matrix::lu_solve(const std::vector<double>& b,
        bool is_lu_decomposed)
{
    assert( this->dim()==(int)b.size() );
    std::vector<double>  x,y;
    if(is_lu_decomposed==false) {
        this->lu_decompose();
    }
    y=this->l_solve(b);
    x=this->r_solve(y);
    return x;
}

// machine precision of a double, i.e. the successor of 1 is 1+eps
double get_eps()
{
    //return std::numeric_limits<double>::epsilon();    // __DBL_EPSILON__
    return 2.2204460492503131e-16;                      // 2^-52
}

// solutions for a + b*x = 0
std::vector<double> solve_linear(double a, double b)
{
    std::vector<double> x;      // roots
    if(b==0.0) {
        if(a==0.0) {
            // 0*x = 0
            x.resize(1);
            x[0] = 0.0;   // any x solves it but we need to pick one
            return x;
        } else {
            // 0*x + ... = 0, no solution
            return x;
        }
    } else {
        x.resize(1);
        x[0] = -a/b;
        return x;
    }
}

// solutions for a + b*x + c*x^2 = 0
std::vector<double> solve_quadratic(double a, double b, double c,
                                    int newton_iter=0)
{
    if(c==0.0) {
        return solve_linear(a,b);
    }
    // rescale so that we solve x^2 + 2p x + q = (x+p)^2 + q - p^2 = 0
    double p=0.5*b/c;
    double q=a/c;
    double discr = p*p-q;
    const double eps=0.5*internal::get_eps();
    double discr_err = (6.0*(p*p)+3.0*fabs(q)+fabs(discr))*eps;

    std::vector<double> x;      // roots
    if(fabs(discr)<=discr_err) {
        // discriminant is zero --> one root
        x.resize(1);
        x[0] = -p;
    } else if(discr<0) {
        // no root
    } else {
        // two roots
        x.resize(2);
        x[0] = -p - sqrt(discr);
        x[1] = -p + sqrt(discr);
    }

    // improve solution via newton steps
    for(size_t i=0; i<x.size(); i++) {
        for(int k=0; k<newton_iter; k++) {
            double f  = (c*x[i] + b)*x[i] + a;
            double f1 = 2.0*c*x[i] + b;
            // only adjust if slope is large enough
            if(fabs(f1)>1e-8) {
                x[i] -= f/f1;
            }
        }
    }

    return x;
}

// solutions for the cubic equation: a + b*x +c*x^2 + d*x^3 = 0
// this is a naive implementation of the analytic solution without
// optimisation for speed or numerical accuracy
// newton_iter: number of newton iterations to improve analytical solution
// see also
//   gsl: gsl_poly_solve_cubic() in solve_cubic.c
//   octave: roots.m - via eigenvalues of the Frobenius companion matrix
std::vector<double> solve_cubic(double a, double b, double c, double d,
                                int newton_iter)
{
    if(d==0.0) {
        return solve_quadratic(a,b,c,newton_iter);
    }

    // convert to normalised form: a + bx + cx^2 + x^3 = 0
    if(d!=1.0) {
        a/=d;
        b/=d;
        c/=d;
    }

    // convert to depressed cubic: z^3 - 3pz - 2q = 0
    // via substitution: z = x + c/3
    std::vector<double> z;              // roots of the depressed cubic
    double p = -(1.0/3.0)*b + (1.0/9.0)*(c*c);
    double r = 2.0*(c*c)-9.0*b;
    double q = -0.5*a - (1.0/54.0)*(c*r);
    double discr=p*p*p-q*q;             // discriminant
    // calculating numerical round-off errors with assumptions:
    //  - each operation is precise but each intermediate result x
    //    when stored has max error of x*eps
    //  - only multiplication with a power of 2 introduces no new error
    //  - a,b,c,d and some fractions (e.g. 1/3) have rounding errors eps
    //  - p_err << |p|, q_err << |q|, ... (this is violated in rare cases)
    // would be more elegant to use boost::numeric::interval<double>
    const double eps = internal::get_eps();
    double p_err = eps*((3.0/3.0)*fabs(b)+(4.0/9.0)*(c*c)+fabs(p));
    double r_err = eps*(6.0*(c*c)+18.0*fabs(b)+fabs(r));
    double q_err = 0.5*fabs(a)*eps + (1.0/54.0)*fabs(c)*(r_err+fabs(r)*3.0*eps)
                   + fabs(q)*eps;
    double discr_err = (p*p) * (3.0*p_err + fabs(p)*2.0*eps)
                       + fabs(q) * (2.0*q_err + fabs(q)*eps) + fabs(discr)*eps;

    // depending on the discriminant we get different solutions
    if(fabs(discr)<=discr_err) {
        // discriminant zero: one or two real roots
        if(fabs(p)<=p_err) {
            // p and q are zero: single root
            z.resize(1);
            z[0] = 0.0;             // triple root
        } else {
            z.resize(2);
            z[0] = 2.0*q/p;         // single root
            z[1] = -0.5*z[0];       // double root
        }
    } else if(discr>0) {
        // three real roots: via trigonometric solution
        z.resize(3);
        double ac = (1.0/3.0) * acos( q/(p*sqrt(p)) );
        double sq = 2.0*sqrt(p);
        z[0] = sq * cos(ac);
        z[1] = sq * cos(ac-2.0*M_PI/3.0);
        z[2] = sq * cos(ac-4.0*M_PI/3.0);
    } else if (discr<0.0) {
        // single real root: via Cardano's fromula
        z.resize(1);
        double sgnq = (q >= 0 ? 1 : -1);
        double basis = fabs(q) + sqrt(-discr);
        double C = sgnq * pow(basis, 1.0/3.0); // c++11 has std::cbrt()
        z[0] = C + p/C;
    }
    for(size_t i=0; i<z.size(); i++) {
        // convert depressed cubic roots to original cubic: x = z - c/3
        z[i] -= (1.0/3.0)*c;
        // improve solution via newton steps
        for(int k=0; k<newton_iter; k++) {
            double f  = ((z[i] + c)*z[i] + b)*z[i] + a;
            double f1 = (3.0*z[i] + 2.0*c)*z[i] + b;
            // only adjust if slope is large enough
            if(fabs(f1)>1e-8) {
                z[i] -= f/f1;
            }
        }
    }
    // ensure if a=0 we get exactly x=0 as root
    // TODO: remove this fudge
    if(a==0.0) {
        assert(z.size()>0);     // cubic should always have at least one root
        double xmin=fabs(z[0]);
        size_t imin=0;
        for(size_t i=1; i<z.size(); i++) {
            if(xmin>fabs(z[i])) {
                xmin=fabs(z[i]);
                imin=i;
            }
        }
        z[imin]=0.0;        // replace the smallest absolute value with 0
    }
    std::sort(z.begin(), z.end());
    return z;
}


} // namespace internal


} // namespace tk


#if !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif

#endif /* TK_SPLINE_H */
