/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "compute_shcoeff.h"
#include <mpi.h>
#include "atom.h"
#include "update.h"
#include "force.h"
#include "domain.h"
#include "group.h"
#include "error.h"
#include "memory.h"
#include <cstring>
#include "utils.h"
#include "fmt/format.h"
#include <iostream>
#include <iomanip>
#include "math_const.h"
#include "math_extra.h"
#include <complex>

using namespace LAMMPS_NS;
using namespace MathConst;

#define DELTALINE 256
#define DELTA 4
/* ---------------------------------------------------------------------- */

ComputeShcoeff::ComputeShcoeff(LAMMPS *lmp, int narg, char **arg) :
        Compute(lmp, narg, arg)
{
  std::cout << narg << std::endl;
  if (narg < 4) error->all(FLERR,"Illegal compute shcoeff command");

  MPI_Comm_rank(world,&me);
  maxline = maxcopy = 0;
  line = copy = work = NULL;
  if (me == 0) {
    nfile = 1;
    maxfile = 16;
    infiles = new FILE *[maxfile];
    infiles[0] = infile;
  } else infiles = NULL;
  lnarg = maxarg = 0;
  larg = NULL;
  curfile = curentry = 0;

  array_flag = 1;
  extarray = 0;

  int maxshexpan = 30;
  double *temp_array;
  size_array_rows = narg - 3;
//  size_array_cols = 2 * (maxshexpan + 1) * (maxshexpan + 1);
  size_array_cols = (maxshexpan+1)*(maxshexpan+2);
  size_vector = maxshexpan+1;
  memory->create(array, size_array_rows, size_array_cols, "ComputeShcoeff:shcoeff");
  memory->create(vector, size_vector, "ComputeShcoeff:inertia_scalefactor");
  for (int i = 3; i < narg; i++) {
    std::cout << arg[i] << std::endl;
    read_coeffs(arg[i]);
    curfile++;
  }
  curfile--;
  MPI_Bcast(&(array[0][0]), size_array_rows * size_array_cols, MPI_DOUBLE, 0, world);

//  if (me==0) calcexpansionfactors(100);
  if (me==0) get_quadrature_values(100, maxshexpan);
  if (me==0) compute_volume(100);
  if (me==0) getI(100);

//  std::cout << me << std::endl;
//  if (me != 0) {
//    for (int i = 0; i < size_array_rows; i++) {
//      int count = 1;
//      for (int j = 0; j < size_array_cols; j = j + 2) {
//        std::cout << i << " " << ++count << " " << array[i][j] << " " << array[i][j + 1] << std::endl;
//      }
//    }
//  }

}

/* ---------------------------------------------------------------------- */

ComputeShcoeff::~ComputeShcoeff()
{
  memory->destroy(array);
  memory->destroy(vector);
  memory->sfree(line);
  memory->sfree(copy);
  memory->sfree(work);
  memory->sfree(larg);
  delete [] infiles;

  memory->sfree(angles);
  memory->sfree(weights);
}

/* ---------------------------------------------------------------------- */

void ComputeShcoeff::calcexpansionfactors(int numpoints)
{

  double safety_factor = 1.01;
  int nmax = 20;
  double theta, phi, factor;
  double rmax;
  double x_val;
  double mphi;
  double P_n_m;
  int nloc, loc;
//  complex comp_temp1, comp_temp2;
//  complex *r_n, *r_npo;
//  r_n = new complex[numpoints*numpoints]();
//  r_npo = new complex[numpoints*numpoints]();
  std::vector<double> r_n, r_npo;
  r_n.resize(numpoints*numpoints, 0.0);
  r_npo.resize(numpoints*numpoints, 0.0);

  std::vector<double> ratios, expfactors;
  ratios.resize(numpoints*numpoints, 0.0);
  expfactors.resize(nmax + 1, 0.0);
  auto begin = ratios.begin();
  auto end = ratios.end();
  expfactors[nmax] = 1.0;
  rmax = 0.0;

  int k;
  for (int n = 0; n <= nmax; n++) {
    nloc = n*(n+1);
    k = 0;
    for (int i = 0; i < numpoints; i++) {
      theta = ((double)(i) * MY_PI) / ((double)(numpoints));
      if (i == 0) theta = 0.001 * MY_PI;
      if (i == numpoints - 1) theta = 0.999 * MY_PI;
      x_val = cos(theta);
      for (int j = 0; j < numpoints; j++) {
        phi = (2.0 * MY_PI * (double)(j)) / ((double)((numpoints)));

        if (k==1)theta =0.56548667764616278308;
        if (k==1)phi = 2.7017696820872219021;

        if (k==2)theta =1.4765485471872028533 ;
        if (k==2)phi = 0.75398223686155030343;

        if (k==3)theta =1.6964600329384882382;
        if (k==3)phi = 3.8955748904513431974;

        if (k==4)theta =1.6336281798666925091;
        if (k==4)phi =  3.9584067435231395926;
        x_val = cos(theta);



        loc = nloc;
        P_n_m = plegendre(n, 0, x_val);
        r_n[k] += array[0][(n+1)*(n+2)-2]*P_n_m;
        std::cout << std::setprecision(10);
//        if (k==1) std::cout << n << " " << 0 << " " << P_n_m << std::endl;
//        std::cout << n << " " << 0 << " " << array[0][(n+1)*(n+2)-2] << std::endl;
        for (int m = n; m > 0; m--) {
          mphi = (double) m * phi;
          P_n_m = plegendre(n, m, x_val);
//          std::cout << n << " " << m << " " << array[0][loc] << " " << array[0][loc+1] << std::endl;
//          if (k==1) std::cout << n << " " << m << " "<< theta << " " << phi << " " << P_n_m*std::cos(mphi) << " " << P_n_m*std::sin(mphi) << std::endl;
          r_n[k] += (array[0][loc]*cos(mphi)-array[0][loc+1]*sin(mphi)) * 2.0 * P_n_m;
          loc+=2;
//          C_SET(comp_temp1, array[0][loc++], array[0][loc++]);
//          C_ANGLE(comp_temp2, mphi);
//          r_n[k] += (comp_temp1.re*comp_temp2.re-comp_temp1.im*comp_temp2.im) * 2.0 * P_n_m;
        }
//        std::cout << n << " " << k << " " << i << " " << j << " " << r_n[k] << std::endl;
        if (r_n[k] > rmax) {
          rmax = r_n[k];
//          std::cout << n << " " << k << " " << i << " " << j << " " << rmax << std::endl;
//          std::cout << std::setprecision(20);
//          std::cout << theta << " " << phi << std::endl;
        }
        if (n <= nmax - 1) {
          r_npo[k] = r_n[k];
          n++;
          loc = n*(n+1);
          P_n_m = plegendre(n, 0, x_val);
          r_npo[k] += array[0][(n+1)*(n+2)-2]*P_n_m;
          for (int m = n; m > 0; m--) {
            mphi = (double) m * phi;
            P_n_m = plegendre(n, m, x_val);
            r_npo[k] += (array[0][loc]*cos(mphi)-array[0][loc+1]*sin(mphi)) * 2.0 * P_n_m;
            loc+=2;
//            C_SET(comp_temp1, array[0][loc++], array[0][loc++]);
//            C_ANGLE(comp_temp2, mphi);
//            r_npo[k] += (comp_temp1.re*comp_temp2.re-comp_temp1.im*comp_temp2.im) * 2.0 * P_n_m;
          }
          n--;
          ratios[k] = r_npo[k] / r_n[k];
//          if (n==1) std::cout << ratios[k] << std::endl;
        }
        k++;
      }
    }
    if (n <= nmax - 1) {
//      expfactors[n] = *max_element(begin, end);
      double max_val = 0;
      int kk_val;
      for (int ii = 0; ii<k; ii++){
        if (ratios[ii]>max_val){
          max_val = ratios[ii];
          kk_val = ii;
        }
      }
//      std::cout << std::setprecision(20);
//      std::cout << kk_val << " " << kk_val/numpoints << " " << kk_val%numpoints <<std::endl;
//      std::cout << ((double)(kk_val/numpoints) * MY_PI) / ((double)(numpoints)) <<
//        " " << (2.0 * MY_PI * (double)(kk_val%numpoints)) / ((double)((numpoints))) <<std::endl;
      expfactors[n] = max_val;
      if (expfactors[n] < 1.0) {
        expfactors[n] = 1.0;
      }
      std::cout << expfactors[n] <<std::endl;
    }
  }

//  for (int n = 0; n <= nmax; n++) {
//    std::cout << expfactors[n] << std::endl;
//  }

//  memory->create(vector, nmax + 1, "ComputeShcoeff:scalefactor");
  factor = expfactors[nmax];
  for (int n = nmax - 1; n >= 0; n--) {
    factor *= expfactors[n] * safety_factor;
    expfactors[n] = factor;
    vector[n] = factor;
  }
  vector[nmax] = 1.0;
  rmax *= safety_factor;


  std::cout << "R_max for all harmonics " << rmax <<std::endl;
  std::cout << "0th harmonic expansion factor " << vector[0] <<std::endl;
  std::cout << "0th harmonic sphere radius " << array[0][0]*std::sqrt(1.0/(4.0*MY_PI)) << std::endl;
  std::cout << "expanded 0th harmonic sphere radius " << vector[0]*double (array[0][0])*std::sqrt(1.0/(4.0*MY_PI)) << std::endl;


  for (int n = 0; n <= nmax; n++) {
    std::cout << vector[n] << std::endl;
  }

}

/* ---------------------------------------------------------------------- */

double ComputeShcoeff::plegendre(const int l, const int m, const double x) {

  int i,ll;
  double fact,oldfact,pll,pmm,pmmp1,omx2;
  if (m < 0 || m > l || std::abs(x) > 1.0)
    throw std::invalid_argument( "Bad arguments in routine plgndr" );
  pmm=1.0;
  if (m > 0) {
    omx2=(1.0-x)*(1.0+x);
    fact=1.0;
    for (i=1;i<=m;i++) {
      pmm *= omx2*fact/(fact+1.0);
      fact += 2.0;
    }
  }
  pmm=sqrt((2.0*m+1.0)*pmm/(4.0*MY_PI));
  if (m & 1)
    pmm=-pmm;
  if (l == m)
    return pmm;
  else {
    pmmp1=x*sqrt(2.0*m+3.0)*pmm;
    if (l == (m+1))
      return pmmp1;
    else {
      oldfact=sqrt(2.0*m+3.0);
      for (ll=m+2;ll<=l;ll++) {
        fact=sqrt((4.0*ll*ll-1.0)/(ll*ll-m*m));
        pll=(x*pmmp1-pmm/oldfact)*fact;
        oldfact=fact;
        pmm=pmmp1;
        pmmp1=pll;
      }
      return pll;
    }
  }
}


double ComputeShcoeff::plegendre_nn(const int l, const double x, const double Pnm_nn) {

  double ll, llm1, fact;

  ll = (double) l;
  llm1 = 2.0*(ll-1.0);
  if (std::abs(x) > 1.0)
    throw std::invalid_argument( "Bad arguments in routine plgndr" );
  fact = sqrt((llm1 + 3.0)/(llm1 + 2.0));
  return -sqrt(1.0-(x*x)) * fact * Pnm_nn;
}


double ComputeShcoeff::plegendre_recycle(const int l, const int m, const double x, const double pnm_m1, const double pnm_m2) {

  double fact,oldfact, ll, mm, pmn;

  ll = (double) l;
  mm = (double) m;
  fact = sqrt((4.0*ll*ll-1.0)/(ll*ll-mm*mm));
  oldfact = sqrt((4.0*(ll-1.0)*(ll-1.0)-1.0)/((ll-1.0)*(ll-1.0)-mm*mm));
  pmn=(x*pnm_m1-pnm_m2/oldfact)*fact;
  return pmn;
}
/* ---------------------------------------------------------------------- */

void ComputeShcoeff::read_coeffs(char *filename)
{

//  std::cout << filename << std::endl;

  // error if another nested file still open, should not be possible
  // open new filename and set infile, infiles[0], nfile
  // call to file() will close filename and decrement nfile
  if (me == 0) {
    if (nfile == maxfile)
      error->one(FLERR, "Too many nested levels of input scripts");

    infile = fopen(filename, "r");
    if (infile == NULL)
      error->one(FLERR, fmt::format("Cannot open input script {}: {}",
                                    filename, utils::getsyserror()));
    infiles[nfile++] = infile;

    // process contents of file
    curentry = 0;
    read_coeffs();

    fclose(infile);
    nfile--;
    infile = infiles[nfile - 1];
  }
}

/* ---------------------------------------------------------------------- */

void ComputeShcoeff::read_coeffs()
{
  int m,n;

  while (1) {

    // read a line from input script
    // n = length of line including str terminator, 0 if end of file
    // if line ends in continuation char '&', concatenate next line

    m = 0;
    while (1) {
      if (maxline-m < 2) reallocate(line,maxline,0);

      // end of file reached, so break
      // n == 0 if nothing read, else n = line with str terminator

      if (fgets(&line[m],maxline-m,infile) == NULL) {
        if (m) n = strlen(line) + 1;
        else n = 0;
        break;
      }

      // continue if last char read was not a newline
      // could happen if line is very long

      m = strlen(line);
      if (line[m-1] != '\n') continue;

      // continue reading if final printable char is & char
      // or if odd number of triple quotes
      // else break with n = line with str terminator

      m--;
      while (m >= 0 && isspace(line[m])) m--;
      if (m < 0 || line[m] != '&') {
        if (numtriple(line) % 2) {
          m += 2;
          continue;
        }
        line[m+1] = '\0';
        n = m+2;
        break;
      }
    }


    // if n = 0, end-of-file
    // if original input file, code is done
    // else go back to previous input file

    if (n == 0) break;

    if (n > maxline) reallocate(line,maxline,n);

    // echo the command unless scanning for label
//    if (me == 0) {
//      std::cout << "Line" << std::endl;
//      fprintf(screen,"%s\n",line);
//      fprintf(logfile,"%s\n",line);
//    }

    // parse the line
    // if no command, skip to next line in input script
    parse();
//    std::cout << lnarg << std::endl;
//    for (int i = 0; i < lnarg; i++) {
//      std::cout << larg[i] << ", ";
//    }
//    std::cout << std::endl;

    if (lnarg==1) {
      if (utils::numeric(FLERR, larg[0], true, lmp) > size_array_cols) {
        error->one(FLERR, "Spherical Harmonic file expansion exceeds memory allocation");
      }
    }
    else if (lnarg==4){
      if (utils::inumeric(FLERR, larg[1], true, lmp) >= 0){
        array[curfile][curentry] = utils::numeric(FLERR, larg[2], true, lmp);
        array[curfile][++curentry] = utils::numeric(FLERR, larg[3], true, lmp);
        curentry++;
      }
    }
    else{
      error->one(FLERR, "Too many entries in Spherical Harmonic file line");
    }

  }
}

/* ----------------------------------------------------------------------
   rellocate a string
   if n > 0: set max >= n in increments of DELTALINE
   if n = 0: just increment max by DELTALINE
------------------------------------------------------------------------- */

void ComputeShcoeff::reallocate(char *&str, int &max, int n)
{
  if (n) {
    while (n > max) max += DELTALINE;
  } else max += DELTALINE;

  str = (char *) memory->srealloc(str,max*sizeof(char),"input:str");
}

/* ----------------------------------------------------------------------
   return number of triple quotes in line
------------------------------------------------------------------------- */

int ComputeShcoeff::numtriple(char *line)
{
  int count = 0;
  char *ptr = line;
  while ((ptr = strstr(ptr,"\"\"\""))) {
    ptr += 3;
    count++;
  }
  return count;
}

/* ----------------------------------------------------------------------
   parse copy of command line by inserting string terminators
   strip comment = all chars from # on
   replace all $ via variable substitution except within quotes
   command = first word
   narg = # of args
   arg[] = individual args
   treat text between single/double/triple quotes as one arg via nextword()
------------------------------------------------------------------------- */
void ComputeShcoeff::parse()
{
  // duplicate line into copy string to break into words

  int n = strlen(line) + 1;
  if (n > maxcopy) reallocate(copy,maxcopy,n);
  strcpy(copy,line);

  // strip any # comment by replacing it with 0
  // do not strip from a # inside single/double/triple quotes
  // quoteflag = 1,2,3 when encounter first single/double,triple quote
  // quoteflag = 0 when encounter matching single/double,triple quote

  int quoteflag = 0;
  char *ptr = copy;
  while (*ptr) {
    if (*ptr == '#' && !quoteflag) {
      *ptr = '\0';
      break;
    }
    if (quoteflag == 0) {
      if (strstr(ptr,"\"\"\"") == ptr) {
        quoteflag = 3;
        ptr += 2;
      }
      else if (*ptr == '"') quoteflag = 2;
      else if (*ptr == '\'') quoteflag = 1;
    } else {
      if (quoteflag == 3 && strstr(ptr,"\"\"\"") == ptr) {
        quoteflag = 0;
        ptr += 2;
      }
      else if (quoteflag == 2 && *ptr == '"') quoteflag = 0;
      else if (quoteflag == 1 && *ptr == '\'') quoteflag = 0;
    }
    ptr++;
  }

  char *next;
  lnarg = 0;
  if (lnarg == maxarg) {
    maxarg += DELTA;
    larg = (char **) memory->srealloc(larg,maxarg*sizeof(char *),"input:arg");
  }
  larg[lnarg] = nextword(copy,&next);
  lnarg++;

  // point arg[] at each subsequent arg in copy string
  // nextword() inserts string terminators into copy string to delimit args
  // nextword() treats text between single/double/triple quotes as one arg

  ptr = next;
  while (ptr) {
    if (lnarg == maxarg) {
      maxarg += DELTA;
      larg = (char **) memory->srealloc(larg,maxarg*sizeof(char *),"input:arg");
    }
    larg[lnarg] = nextword(ptr,&next);
    if (!larg[lnarg]) break;
    lnarg++;
    ptr = next;
  }
}

/* ----------------------------------------------------------------------
   find next word in str
   insert 0 at end of word
   ignore leading whitespace
   treat text between single/double/triple quotes as one arg
   matching quote must be followed by whitespace char if not end of string
   strip quotes from returned word
   return ptr to start of word or NULL if no word in string
   also return next = ptr after word
------------------------------------------------------------------------- */

char *ComputeShcoeff::nextword(char *str, char **next)
{
  char *start,*stop;

  // start = first non-whitespace char

  start = &str[strspn(str," \t\n\v\f\r")];
  if (*start == '\0') return NULL;

  // if start is single/double/triple quote:
  //   start = first char beyond quote
  //   stop = first char of matching quote
  //   next = first char beyond matching quote
  //   next must be NULL or whitespace
  // if start is not single/double/triple quote:
  //   stop = first whitespace char after start
  //   next = char after stop, or stop itself if stop is NULL

  if (strstr(start,"\"\"\"") == start) {
    stop = strstr(&start[3],"\"\"\"");
    if (!stop) error->all(FLERR,"Unbalanced quotes in input line");
    start += 3;
    *next = stop+3;
    if (**next && !isspace(**next))
      error->all(FLERR,"Input line quote not followed by white-space");
  } else if (*start == '"' || *start == '\'') {
    stop = strchr(&start[1],*start);
    if (!stop) error->all(FLERR,"Unbalanced quotes in input line");
    start++;
    *next = stop+1;
    if (**next && !isspace(**next))
      error->all(FLERR,"Input line quote not followed by white-space");
  } else {
    stop = &start[strcspn(start," \t\n\v\f\r")];
    if (*stop == '\0') *next = stop;
    else *next = stop+1;
  }

  // set stop to NULL to terminate word

  *stop = '\0';
  return start;
}


// This function computes the kth zero of the BesselJ(0,x)
double ComputeShcoeff::besseljzero(int k)
{
  if(k > 20)
  {
    double z = MY_PI*(k-0.25);
    double r = 1.0/z;
    double r2 = r*r;
    z = z + r*(0.125+r2*(-0.807291666666666666666666666667e-1+r2*(0.246028645833333333333333333333+r2*
            (-1.82443876720610119047619047619+r2*(25.3364147973439050099206349206+r2*(-567.644412135183381139802038240+
            r2*(18690.4765282320653831636345064+r2*(-8.49353580299148769921876983660e5+
            5.09225462402226769498681286758e7*r2))))))));
    return z;
  }
  else
  {
    return JZ[k-1];
  }
}


// This function computes the square of BesselJ(1, BesselZero(0,k))
double ComputeShcoeff::besselj1squared(int k)
{
  if(k > 21)
  {
    double x = 1.0/(k-0.25);
    double x2 = x*x;
    return x * (0.202642367284675542887758926420 + x2*x2*(-0.303380429711290253026202643516e-3 +
    x2*(0.198924364245969295201137972743e-3 + x2*(-0.228969902772111653038747229723e-3+x2*
    (0.433710719130746277915572905025e-3+x2*(-0.123632349727175414724737657367e-2+x2*
    (0.496101423268883102872271417616e-2+x2*(-0.266837393702323757700998557826e-1+
    .185395398206345628711318848386*x2))))))));
  }
  else
  {
    return J1[k-1];
  }
}


// Compute a node-weight pair, with k limited to half the range
ComputeShcoeff::QuadPair ComputeShcoeff::GLPairS(size_t n, size_t k)
{
  // First get the Bessel zero
  double w = 1.0/(n+0.5);
  double nu = besseljzero(k);
  double theta = w*nu;
  double x = theta*theta;

  // Get the asymptotic BesselJ(1,nu) squared
  double B = besselj1squared(k);

  // Get the Chebyshev interpolants for the nodes...
  double SF1T = (((((-1.29052996274280508473467968379e-12 * x + 2.40724685864330121825976175184e-10) * x -
                    3.13148654635992041468855740012e-8) * x + 0.275573168962061235623801563453e-5) * x -
                  0.148809523713909147898955880165e-3) * x + 0.416666666665193394525296923981e-2) * x;
  double SF2T = (((((+2.20639421781871003734786884322e-9 * x - 7.53036771373769326811030753538e-8) * x +
                    0.161969259453836261731700382098e-5) * x - 0.253300326008232025914059965302e-4) * x +
                  0.282116886057560434805998583817e-3) * x - 0.209022248387852902722635654229e-2) * x;
  double SF3T = (((((-2.97058225375526229899781956673e-8 * x + 5.55845330223796209655886325712e-7) * x -
                    0.567797841356833081642185432056e-5) * x + 0.418498100329504574443885193835e-4) * x -
                  0.251395293283965914823026348764e-3) * x + 0.128654198542845137196151147483e-2) * x;

  // ...and for the weights
  double WSF1T = ((((((((-2.20902861044616638398573427475e-14 * x + 2.30365726860377376873232578871e-12) * x -
                        1.75257700735423807659851042318e-10) * x + 1.03756066927916795821098009353e-8) * x -
                      4.63968647553221331251529631098e-7) * x + 0.149644593625028648361395938176e-4) * x -
                    0.326278659594412170300449074873e-3) * x + 0.436507936507598105249726413120e-2) * x -
                  0.305555555555553028279487898503e-1) * x;
  double WSF2T = (((((((+3.63117412152654783455929483029e-12 * x + 7.67643545069893130779501844323e-11) * x -
                       7.12912857233642220650643150625e-9) * x + 2.11483880685947151466370130277e-7) * x -
                     0.381817918680045468483009307090e-5) * x + 0.465969530694968391417927388162e-4) * x -
                   0.407297185611335764191683161117e-3) * x + 0.268959435694729660779984493795e-2) * x;
  double WSF3T = (((((((+2.01826791256703301806643264922e-9 * x - 4.38647122520206649251063212545e-8) * x +
                       5.08898347288671653137451093208e-7) * x - 0.397933316519135275712977531366e-5) * x +
                     0.200559326396458326778521795392e-4) * x - 0.422888059282921161626339411388e-4) * x -
                   0.105646050254076140548678457002e-3) * x - 0.947969308958577323145923317955e-4) * x;

  // Then refine with the paper expansions
  double NuoSin = nu/sin(theta);
  double BNuoSin = B*NuoSin;
  double WInvSinc = w*w*NuoSin;
  double WIS2 = WInvSinc*WInvSinc;

  // Finally compute the node and the weight
  theta = w*(nu + theta * WInvSinc * (SF1T + WIS2*(SF2T + WIS2*SF3T)));
  double Deno = BNuoSin + BNuoSin * WIS2*(WSF1T + WIS2*(WSF2T + WIS2*WSF3T));
  double weight = (2.0*w)/Deno;
  return QuadPair(theta,weight);
}


// Returns tabulated theta and weight values: valid for l <= 100
ComputeShcoeff::QuadPair ComputeShcoeff::GLPairTabulated(size_t l, size_t k)
{
  // Odd Legendre degree
  if(l & 1)
  {
    size_t l2 = (l-1)/2;
    if(k == l2)
      return(QuadPair(MY_PI/2, 2.0/(Cl[l]*Cl[l])));
    else if(k < l2)
      return(QuadPair(OddThetaZeros[l2-1][l2-k-1],OddWeights[l2-1][l2-k-1]));
    else
      return(QuadPair(MY_PI-OddThetaZeros[l2-1][k-l2-1],OddWeights[l2-1][k-l2-1]));
  }
    // Even Legendre degree
  else
  {
    size_t l2 = l/2;
    if(k < l2)
      return(QuadPair(EvenThetaZeros[l2-1][l2-k-1],EvenWeights[l2-1][l2-k-1]));
    else
      return(QuadPair(MY_PI-EvenThetaZeros[l2-1][k-l2],EvenWeights[l2-1][k-l2]));
  }
}


// This function computes the kth GL pair of an n-point rule
ComputeShcoeff::QuadPair ComputeShcoeff::GLPair(size_t n, size_t k)
{
  // Sanity check [also implies l > 0]
  if(n < 101)
    return(GLPairTabulated(n, k-1));
  else
  {
    if((2*k-1) > n)
    {
      QuadPair P = GLPairS(n, n-k+1);
      P.theta = MY_PI - P.theta;
      return P;
    }
    else return GLPairS(n, k);
  }
}


void ComputeShcoeff::get_quadrature_values(int num_quadrature, int maxshexpan) {

  int num_quad2 = num_quadrature*num_quadrature;

  memory->create(angles, 2, num_quad2, "ComputeShcoeff:angles");
  memory->create(weights, num_quadrature, "ComputeShcoeff:weights");
  memory->create(quad_rads, num_quad2, "ComputeShcoeff:quad_rads");

  // Fixed properties
  double abscissa[num_quadrature];
  QuadPair p;

  // Get the quadrature weights, and abscissa. Convert abscissa to theta angles
  for (int i = 0; i < num_quadrature; i++) {
    p = GLPair(num_quadrature, i + 1);
    weights[i] = p.weight;
    abscissa[i] = p.x();
  }

  int count=0;
  for (int i = 0; i < num_quadrature; i++) {
    for (int j = 0; j < num_quadrature; j++) {
      angles[0][count] = 0.5 * MY_PI * (abscissa[i] + 1.0);
      angles[1][count] = MY_PI * (abscissa[j] + 1.0);
      count++;
    }
  }


  double theta, phi;
  int nloc, loc;
  double rad_val;
  double P_n_m, x_val, mphi, Pnm_nn;
  std::vector<double> Pnm_m2, Pnm_m1;
  Pnm_m2.resize(maxshexpan+1, 0.0);
  Pnm_m1.resize(maxshexpan+1, 0.0);

  for (int k = 0; k < num_quad2; k++) {
    theta = angles[0][k];
    phi = angles[1][k];
    x_val = std::cos(theta);
    rad_val = array[0][0]*std::sqrt(1.0/(4.0*MY_PI));
    Pnm_m2.clear();
    Pnm_m1.clear();
    for (int n = 1; n <= maxshexpan; n++) {
      nloc = n*(n+1);
      if (n==1) {
        P_n_m = plegendre(1, 0, x_val);
        Pnm_m2[0] = P_n_m;
        rad_val += array[0][4] * P_n_m;
        P_n_m = plegendre(1, 1, x_val);
        Pnm_m2[1] = P_n_m;
        mphi = 1.0 * phi;
        rad_val += (array[0][2]*cos(mphi)-array[0][3]*sin(mphi)) * 2.0 * P_n_m;
      }
      else if(n==2){
        P_n_m = plegendre(2, 0, x_val);
        Pnm_m1[0] = P_n_m;
        rad_val += array[0][10] * P_n_m;
        for (int m = 2; m >= 1; m--) {
          P_n_m = plegendre(2, m, x_val);
          Pnm_m1[m] = P_n_m;
          mphi = (double) m * phi;
          rad_val += (array[0][nloc]*cos(mphi)-array[0][nloc+1]*sin(mphi)) * 2.0 * P_n_m;
          nloc+=2;
        }
        Pnm_nn = Pnm_m1[2];
      }
      else{
        P_n_m = plegendre_recycle(n, 0, x_val, Pnm_m1[0], Pnm_m2[0]);
        Pnm_m2[0] = Pnm_m1[0];
        Pnm_m1[0] = P_n_m;
        loc = (n+1)*(n+2)-2;
        rad_val += array[0][loc]* P_n_m;
        loc-=2;
        for (int m = 1; m < n-1; m++) {
          P_n_m = plegendre_recycle(n, m, x_val, Pnm_m1[m], Pnm_m2[m]);
          Pnm_m2[m] = Pnm_m1[m];
          Pnm_m1[m] = P_n_m;
          mphi = (double) m * phi;
          rad_val += (array[0][loc]*cos(mphi)-array[0][loc+1]*sin(mphi)) * 2.0 * P_n_m;
          loc-=2;
        }

        P_n_m = x_val * std::sqrt( (2.0*((double)n-1.0))+3.0 ) * Pnm_nn;
        Pnm_m2[n-1] = Pnm_m1[n-1];
        Pnm_m1[n-1] = P_n_m;
        mphi = (double) (n-1) * phi;
        rad_val += (array[0][loc]*cos(mphi)-array[0][loc+1]*sin(mphi)) * 2.0 * P_n_m;
        loc-=2;

        P_n_m = plegendre_nn(n, x_val, Pnm_nn);
        Pnm_nn = P_n_m;
        Pnm_m1[n] = P_n_m;
        mphi = (double) n * phi;
        rad_val += (array[0][loc]*cos(mphi)-array[0][loc+1]*sin(mphi)) * 2.0 * P_n_m;
      }
    }
    quad_rads[k] = rad_val;
  }


}


void ComputeShcoeff::compute_volume(int num_quadrature) {

  double theta, v1;
  double factor = 0.5 * MY_PI * MY_PI;
  int count;
  double volume = v1 = 0.0;

  count = 0;
  for (int i = 0; i < num_quadrature; i++) {
    for (int j = 0; j < num_quadrature; j++) {
      theta = angles[0][count];
      v1 = std::sin(theta) / 3.0;
      v1 *= std::pow(quad_rads[count], 3.0);
      v1 *= (weights[i] * weights[j]);
      volume += v1;
//      std::cout << count << " " << theta << " " << quad_rads[count] << " " << weights[i] << " " << weights[j] << " " << std::endl;
      count++;
    }
  }
  volume *= factor;
  std::cout << "Volume = " << volume << std::endl;
  return;
}


void ComputeShcoeff::getI(int num_quadrature) {
  using std::cout;
  using std::endl;
  using std::sin;
  using std::cos;
  using std::pow;
  using std::sqrt;
  using std::fabs;

  std::vector<double> itensor;

  itensor.clear();

  double i11,i22,i33,i12,i23,i13;
  double theta,phi,st,ct,sp,cp,r,vol,fact;
  double factor = (0.5 * MY_PI * MY_PI);
  int count = 0;

  i11 = i22 = i33 = i12 = i23 = i13 = fact = vol = 0.0;

  for (int i = 0; i < num_quadrature; i++) {
    for (int j = 0; j < num_quadrature; j++) {
      theta = angles[0][count];
      phi = angles[1][count];
      st = sin(theta);
      ct = cos(theta);
      sp = sin(phi);
      cp = cos(phi);
      r = quad_rads[count];
      fact = 0.2 * weights[i] * weights[j] * pow(r,5.0) * st;
      vol += (weights[i] * weights[j] * pow(r,3.0) * st / 3.0);
      i11 += (fact * (1.0 - pow(cp*st,2.0)));
      i22 += (fact * (1.0 - pow(sp*st,2.0)));
      i33 += (fact * (1.0 - pow(ct,2.0)));
      i12 -= (fact * cp * sp * st * st);
      i13 -= (fact * cp * ct * st);
      i23 -= (fact * sp * ct * st);
      count++;
    }
  }

  vol *= factor;
  i11 *= factor;
  i22 *= factor;
  i33 *= factor;
  i12 *= factor;
  i13 *= factor;
  i23 *= factor;
  if (vol > 0.0) {
    i11 /= vol;
    i22 /= vol;
    i33 /= vol;
    i12 /= vol;
    i13 /= vol;
    i23 /= vol;
    itensor.push_back(i11);
    itensor.push_back(i22);
    itensor.push_back(i33);
    itensor.push_back(i12);
    itensor.push_back(i13);
    itensor.push_back(i23);
  } else {
    throw std::invalid_argument( "Divide by vol = 0 in getI" );
  }

  int ierror;
  double inertia[3];        // 3 principal components of inertia
  double tensor[3][3],evectors[3][3];

  tensor[0][0] = itensor[0];
  tensor[1][1] = itensor[1];
  tensor[2][2] = itensor[2];
  tensor[1][2] = tensor[2][1] = itensor[5];
  tensor[0][2] = tensor[2][0] = itensor[4];
  tensor[0][1] = tensor[1][0] = itensor[3];

  ierror = MathExtra::jacobi(tensor,inertia,evectors);
  if (ierror) error->all(FLERR,
                         "Insufficient Jacobi rotations for rigid body");


  // Output eigenvector information
  cout << endl;
  cout << "Eigenvectors for I:" << endl;
  cout << " | " << evectors[0][0] << " " << evectors[1][0] << " " << evectors[2][0] << " |" << endl;
  cout << " | " << evectors[0][1] << " " << evectors[1][1] << " " << evectors[2][1] << " |" << endl;
  cout << " | " << evectors[0][2] << " " << evectors[1][2] << " " << evectors[2][2] << " |" << endl;
  cout << endl;

  // Output eigenvector information
  cout << endl;
  cout << "Principal Inertia:" << endl;
  cout << " | " << inertia[0] << " " << inertia[1] << " " << inertia[2] << " |" << endl;
  cout << endl;

  return;
}



