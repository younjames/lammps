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

/* ----------------------------------------------------------------------
   Contributing author: Paul Crozier (SNL)
------------------------------------------------------------------------- */

#include "pair_sh.h"
#include <mpi.h>
#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "update.h"
#include "respa.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"
#include "utils.h"
#include "math_extra.h"
#include "atom_vec_spherharm.h"
#include "math_spherharm.h"

using namespace LAMMPS_NS;
using namespace MathConst;

/* ---------------------------------------------------------------------- */

PairSH::PairSH(LAMMPS *lmp) : Pair(lmp)
{
  // Single steps are for force and energy of a single pairwise interaction between 2 atoms
  // Energy calculation not enabled, as we don't yet have pairwise potential
  single_enable = 0;
  restartinfo = 0; // Not figured out how to do this yet
  writedata = 0; // Ditto
  respa_enable = 0;

  // Flag indicating if lammps types have been matches with SH type.
  matchtypes = 0;
  exponent = -1.0;

  cur_time = 0.0; // for temp file writing
  file_count = 0; // for temp file writing

  num_pole_quad = 30;
  radius_tol = 1e-3; // 0.1%
}

/* ---------------------------------------------------------------------- */

PairSH::~PairSH()
{
  if (allocated) {
    memory->destroy(setflag);
    memory->destroy(cutsq);
    memory->destroy(cut);
    memory->destroy(normal_coeffs);
    memory->destroy(typetosh);

    memory->destroy(weights);
    memory->destroy(abscissa);
  }
}

/* ---------------------------------------------------------------------- */
void PairSH::compute(int eflag, int vflag)
{
  int i,j,ii,jj;
  int inum,jnum,itype,jtype,ishtype,jshtype;
  int *ilist,*jlist,*numneigh,**firstneigh;
  double fpair,radi,radj,r,rsq,iang;
  double irot[3][3],jrot[3][3];
  double x_testpoint[3],delvec[3];
  double iquat_sf_bf[4],iquat_cont[4];

  ev_init(eflag,vflag);

  double **x = atom->x;
  double **f = atom->f;
  int *type = atom->type;
  int *shtype = atom->shtype;
  double **quat = atom->quat;
  int nlocal = atom->nlocal;
  double **torque = atom->torque;
  double *max_rad = atom->maxrad_byshape;

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  int me,kk_count;
  bool first_call,candidates_found;
  double vol_overlap,factor,pn,fn;
  double torsum[3],xcont[3], iforce[3];
  double irot_cont[3][3];

  file_count++;
  MPI_Comm_rank(world,&me);

  // loop over neighbors of my atoms
  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    MathExtra::copy3(x[i], delvec);
    itype = type[i];
    ishtype = shtype[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];
    radi = max_rad[ishtype];

    // Calculate the rotation matrix for the quaternion for atom i
    MathExtra::quat_to_mat(quat[i], irot);
    // Quaternion to get from space frame to body frame for atom "i"
    MathExtra::qconjugate(quat[i], iquat_sf_bf);
    MathExtra::qnormalize(iquat_sf_bf);

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      j &= NEIGHMASK;
      MathExtra::sub3(delvec, x[j], delvec);
      jshtype = shtype[j];
      radj = max_rad[jshtype];
      rsq = MathExtra::lensq3(delvec);
      r = sqrt(rsq);
      jtype = type[j];

      kk_count = -1;
      first_call = true;
      vol_overlap = 0.0;
      MathExtra::zero3(iforce);
      MathExtra::zero3(torsum);

      if (r<radi+radj) {
        if (r > radj) { // Can use spherical cap from particle "i"
//          iang =  std::asin(radj/r) + (0.5 * MY_PI / 180.0); // Adding half a degree to ensure that circumference is populated
          iang = std::asin(radj / r);
        }
          // TODO Add condition for particle "j" were the code will swap to particle "j" becoming the primary if "i" is not feasible
        else { // Can't use either spherical cap
          error->all(FLERR, "Error, centre within radius!");
        }

        // Get the quaternion from north pole of atom "i" to the vector connecting the centre line of atom "i" and "j".
        MathExtra::negate3(delvec);
        get_contact_quat(delvec, iquat_cont);
        // Quaternion of north pole to contact for atom "i"
        MathExtra::quat_to_mat(iquat_cont, irot_cont);
        // Calculate the rotation matrix for the quaternion for atom j
        MathExtra::quat_to_mat(quat[j], jrot);
        cur_time += (update->dt) / 1000; // for temp file writing

        candidates_found = refine_cap_angle(kk_count, ishtype, jshtype, iang, radj, iquat_cont, iquat_sf_bf, x[i], x[j],
                                            jrot);

        if (kk_count > num_pole_quad) kk_count = num_pole_quad; // don't refine if points on first layer

        if (candidates_found) {

          calc_force_torque(kk_count, ishtype, jshtype, iang, radi, radj, iquat_cont, iquat_sf_bf, x[i], x[j], irot,
                            jrot, vol_overlap, iforce, torsum, factor, first_call, ii, jj);

//        if (vol_overlap==0.0) continue;

          fpair = normal_coeffs[itype][jtype][0];
          pn = exponent * fpair * std::pow(vol_overlap, exponent - 1.0);
          MathExtra::scale3(-pn, iforce);    // F_n = -p_n * S_n (S_n = factor*iforce)
          MathExtra::scale3(-pn, torsum);    // M_n

          // Force and torque on particle a
          MathExtra::add3(f[i], iforce, f[i]);
          MathExtra::add3(torque[i], torsum, torque[i]);

          // N.B on a single proc, N3L is always imposed, regardless of Newton On/Off
          if (force->newton_pair || j < nlocal) {

            // Force on particle b
            MathExtra::sub3(f[j], iforce, f[j]);

            // Torque on particle b
            fn = MathExtra::len3(iforce);
            MathExtra::cross3(torsum, iforce, xcont);       // M_n x F_n
            MathExtra::scale3(-1.0 / (fn * fn), xcont);     // (M_n x F_n)/|F_n|^2 [Swap direction due to cross, normally x_c X F_n]
            MathExtra::add3(xcont, x[i], xcont);            // x_c global cords
            MathExtra::sub3(xcont, x[j], x_testpoint);      // Vector from centre of "b" to contact point
            MathExtra::cross3(iforce, x_testpoint, torsum); // M_n' = F_n x (x_c - x_b)
            MathExtra::add3(torque[j], torsum, torque[j]);

          } // newton_pair

//        if (eflag) {
//          evdwl = fpair * std::pow(vol_overlap, exponent);
//        }
//
//        if (evflag) {
//          ev_tally_xyz(i, j, nlocal, force->newton_pair,
//                       evdwl, 0.0, iforce[0], iforce[1],
//                       iforce[2], delvec[0], delvec[1], delvec[2]);
//        }

      avec->dump_ply(i,ishtype,file_count,irot,x[i]);
      avec->dump_ply(j,jshtype,file_count,jrot,x[j]);

        } // candidates found

//      kk_count = -1;
//      double jang =  std::asin(radi/r) + (0.5 * MY_PI / 180.0);
//      MathExtra::negate3(delvec);
//      double jquat_cont[4], jquat_sf_bf[4];
//      get_contact_quat(delvec, jquat_cont);
//      MathExtra::qconjugate(quat[j], jquat_sf_bf);
//      MathExtra::qnormalize(jquat_sf_bf);
//
//      candidates_found = refine_cap_angle(kk_count,jshtype,ishtype,jang,radi,jquat_cont,jquat_sf_bf,x[j],x[i],irot);
//      if (kk_count == 0) kk_count = 1;
//
//      vol_overlap = 0.0;
//      MathExtra::zero3(iforce);
//      MathExtra::zero3(torsum);
//      if (candidates_found) calc_force_torque(kk_count,jshtype,ishtype,jang,radi,jquat_cont,jquat_sf_bf,x[j],x[i],jrot,
//                    irot,vol_overlap,iforce,torsum,factor,first_call,jj,ii);
//      std::cout<<"Vol j : " << std::setprecision(16) << vol_overlap << std::endl;
//      std::cout<<"A j : " << std::setprecision(16) << iforce[0] << " " << iforce[1] << " " << iforce[2] << " " << MathExtra::len3(iforce) << std::endl;
//      fpair = normal_coeffs[itype][jtype][0];
//      pn  = exponent * fpair * std::pow(vol_overlap, exponent-1.0);
//      MathExtra::scale3(-pn, iforce);    // F_n = -p_n * S_n (S_n = factor*iforce)
//      MathExtra::scale3(-pn, torsum);    // M_n
//      std::cout<<"F j : " << std::setprecision(16) << iforce[0] << " " << iforce[1] << " " << iforce[2] << " " << MathExtra::len3(iforce) << std::endl;

      } // bounding spheres
    } // jj
  } // ii
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairSH::allocate()
{
  allocated = 1;
    int n = atom->ntypes;

    memory->create(setflag,n+1,n+1,"pair:setflag");
    for (int i = 1; i <= n; i++)
        for (int j = i; j <= n; j++)
            setflag[i][j] = 0;

    memory->create(cutsq,n+1,n+1,"pair:cutsq");
    memory->create(cut,n+1,n+1,"pair:cut");
    memory->create(normal_coeffs,n+1,n+1,1,"pair:normal_coeffs");
    memory->create(typetosh,n+1,"pair:typetosh");
}

/* ----------------------------------------------------------------------
   global settings
   JY - Not defining a global cut off, as this must come from the
   atom style, where the maximum particle radius is stored
------------------------------------------------------------------------- */

void PairSH::settings(int narg, char **arg) {
  if (narg != 0) error->all(FLERR, "Illegal pair_style command");

  avec = (AtomVecSpherharm *) atom->style_match("spherharm");
  if (!avec) error->all(FLERR,"Pair SH requires atom style shperatom");

}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
   JY - Only type pairs are defined here, no other parameters. The global
   cutoff is taken from the atom style here.
------------------------------------------------------------------------- */

void PairSH::coeff(int narg, char **arg)
{

  if (narg != 4)
    error->all(FLERR,"Incorrect args for pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  double normal_coeffs_one, exponent_in;
  utils::bounds(FLERR,arg[0],1,atom->ntypes,ilo,ihi,error);
  utils::bounds(FLERR,arg[1],1,atom->ntypes,jlo,jhi,error);
  normal_coeffs_one = utils::numeric(FLERR,arg[2],false,lmp);// kn
  exponent_in = utils::numeric(FLERR,arg[3],false,lmp);// m

  if (exponent==-1){
    exponent=exponent_in;
  }
  else if(exponent!=exponent_in){
    error->all(FLERR,"Exponent must be equal for all type interactions, exponent mixing not developed");
  }

  // Linking the Types to the SH Types, needed for finding the cut per Type
  if (!matchtypes) matchtype();

  int count = 0;
  int shi, shj;
  double *max_rad = atom->maxrad_byshape;

  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      shi = typetosh[i];
      shj = typetosh[j];
      cut[i][j] = max_rad[shi]+max_rad[shj];
      setflag[i][j] = 1;
      normal_coeffs[i][j][0] = normal_coeffs_one;
      count++;
    }
  }

  if (count == 0) error->all(FLERR,"Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   JY - Each type can only use one Spherical Harmonic Particle type. This
   method associates a SH particle type with the atom->types. Required for
   finding the cut[i][j] between types which is then used in the neighbour
   searching.
------------------------------------------------------------------------- */
void PairSH::matchtype()
{

  matchtypes = 1;

  int nlocal = atom->nlocal;
  int *shtype = atom->shtype;
  int *type = atom->type;

  for (int i = 0; i <= atom->ntypes; i++) {
    typetosh[i] = -1;
  }

  for (int i = 0; i < nlocal; i++) {
    if (typetosh[type[i]]==-1) {
      typetosh[type[i]] = shtype[i];
    }
    else if(typetosh[type[i]] != shtype[i]){
      error->all(FLERR,"Types must have same Spherical Harmonic particle type");
    }
  }

  // Possibility that atoms on different processors may have associated different
  // SH particle types with atom->types. This will not be caught here and the maximum
  // will be taken.
  MPI_Allreduce(MPI_IN_PLACE,typetosh,atom->ntypes+1,MPI_INT,MPI_MAX,world);
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairSH::init_style()
{
  neighbor->request(this,instance_me);
  get_quadrature_values(num_pole_quad);
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
   JY - Need to set up for different types, although both types must use the
   spherical harmonic atom style. Maximum radius of type pair is used for cut.
   The only mixing would be in the coefficients used in the contact model,
   i.e stiffness, but this will need to be explored later
   These coefficients wouldn't even be mixed if using F_i = K_i*V*n_i (bad model)
------------------------------------------------------------------------- */

double PairSH::init_one(int i, int j)
{
  int shi, shj;
  double *max_rad = atom->maxrad_byshape;

  // No epsilon and no sigma used for the spherical harmonic atom style
  if (setflag[i][j] == 0) {
    shi = typetosh[i];
    shj = typetosh[j];
    cut[i][j] = max_rad[shi]+max_rad[shj];
  }

  // TO FIX - Just use the first coefficient for the pair, no mixing
  normal_coeffs[i][j][0] = normal_coeffs[j][i][0] = normal_coeffs[i][i][0];

  return cut[i][j];
}

/* ----------------------------------------------------------------------
   JY - Calculates the quaternion required to rotate points generated
   on the (north) pole of an atom back to the vector between two atom centres.
   https://stackoverflow.com/questions/1171849/finding-quaternion-representing-the-rotation-from-one-vector-to-another

    TODO - Need to comapre this against generating the quaterion from the
    spherical coordinates (theta,phi) of the contact line from the COG of
    each particle in space frame
 ------------------------------------------------------------------------- */
void PairSH::get_contact_quat(double (&xvecdist)[3], double (&quat)[4]){

  double vert_unit_vec[3], cross_vec[3], c;

  // North pole unit vector, points generated are with reference to this point
  vert_unit_vec[0] = 0;
  vert_unit_vec[1] = 0;
  vert_unit_vec[2] = 1.0;
  c = MathExtra::dot3(vert_unit_vec, xvecdist);
  MathExtra::cross3(vert_unit_vec, xvecdist, cross_vec);
  quat[1] = cross_vec[0];
  quat[2] = cross_vec[1];
  quat[3] = cross_vec[2];
  quat[0] = sqrt(MathExtra::lensq3(vert_unit_vec) * MathExtra::lensq3(xvecdist)) + c;
  MathExtra::qnormalize(quat);
}


void PairSH::get_quadrature_values(int num_quadrature) {

  memory->create(weights, num_quadrature, "PairSH:weights");
  memory->create(abscissa, num_quadrature, "PairSH:abscissa");

  MathSpherharm::QuadPair p;
  // Get the quadrature weights, and abscissa.
  for (int i = 0; i < num_quadrature; i++) {
    p = MathSpherharm::GLPair(num_quadrature, i + 1);
    weights[i] = p.weight;
    abscissa[i] = p.x();
  }

}

int PairSH::refine_cap_angle(int &kk_count, int ishtype, int jshtype, double iang,  double radj,
                             double (&iquat_cont)[4], double (&iquat_sf_bf)[4], const double xi[3],
                             const double xj[3], double (&jrot)[3][3]){

  int kk, ll, n;
  double theta_pole, phi_pole, theta, phi, theta_proj, phi_proj;
  double rad_body, dtemp, finalrad, cosang;
  double ix_sf[3], x_testpoint[3], x_projtestpoint[3];
  double gp[3], gp_bf[3], gp_sf[3];
  double quat[4];
  double rot_np_bf[3][3], rot_np_sf[3][3];

  MathExtra::quat_to_mat(iquat_cont, rot_np_sf);
  MathExtra::quatquat(iquat_sf_bf, iquat_cont, quat);
  MathExtra::qnormalize(quat);
  MathExtra::quat_to_mat(quat, rot_np_bf);

  n = 2*(num_pole_quad-1);
  cosang = std::cos(iang);

  for (kk = num_pole_quad-1; kk >= 0; kk--) { // start from widest angle to allow early stopping
    theta_pole = std::acos((abscissa[kk]*((1.0-cosang)/2.0)) + ((1.0+cosang)/2.0));
    for (ll = 1; ll <= n+1; ll++) {
      phi_pole = MY_2PI * double(ll-1) / (double(n + 1));

      gp[0] = std::sin(theta_pole)*std::cos(phi_pole); // quadrature point at [0,0,1]
      gp[1] = std::sin(theta_pole)*std::sin(phi_pole);
      gp[2] = std::cos(theta_pole);

      MathExtra::matvec(rot_np_bf, gp, gp_bf); // quadrature point at contact in body frame
      phi = std::atan2(gp_bf[1], gp_bf[0]);
      phi = phi > 0.0 ? phi : MY_2PI + phi;
      theta = std::acos(gp_bf[2]);

      rad_body = avec->get_shape_radius(ishtype, theta, phi);

      MathExtra::matvec(rot_np_sf, gp, gp_sf); // quadrature point at contact in space frame
      phi = std::atan2(gp_sf[1], gp_sf[0]);
      phi = phi > 0.0 ? phi : MY_2PI + phi;
      theta = std::acos(gp_sf[2]);

      ix_sf[0] = (rad_body * sin(theta) * cos(phi)) + xi[0];
      ix_sf[1] = (rad_body * sin(theta) * sin(phi)) + xi[1];
      ix_sf[2] = (rad_body * cos(theta)) + xi[2];
      // vector distance from COG of atom j (in space frame) to test point on atom i
      MathExtra::sub3(ix_sf, xj, x_testpoint);
      // scalar distance
      dtemp = MathExtra::len3(x_testpoint);
      if (dtemp > radj) continue;
      // Rotating the projected point into atom j's body frame (rotation matrix transpose = inverse)
      MathExtra::transpose_matvec(jrot, x_testpoint, x_projtestpoint);
      // Get projected phi and theta angle of gauss point in atom i's body frame
      phi_proj = std::atan2(x_projtestpoint[1], x_projtestpoint[0]);
      phi_proj = phi_proj > 0.0 ? phi_proj : MY_2PI + phi_proj; // move atan2 range from 0 to 2pi
      theta_proj = std::acos(x_projtestpoint[2] / dtemp);

      // Check for contact
      if (avec->check_contact(jshtype, phi_proj, theta_proj, dtemp, finalrad)) {
        kk_count = kk+1; // refine the spherical cap angle to this index (+1 as points could exist between indexes)
        return 1;
      }
    }
  }
  return 0;
}

void PairSH::calc_force_torque(int kk_count, int ishtype, int jshtype, double iang, double radi, double radj,
                               double (&iquat_cont)[4], double (&iquat_sf_bf)[4], const double xi[3],
                               const double xj[3], double (&irot)[3][3],  double (&jrot)[3][3],
                               double &vol_overlap, double (&iforce)[3], double (&torsum)[3],
                               double &factor, bool &first_call, int ii, int jj){

  int kk, ll, n;
  double cosang, fac, radtol;
  double theta_pole, phi_pole, theta_proj, phi_proj;
  double theta_bf, phi_bf, theta_sf, phi_sf;
  double rad_body, dtemp, finalrad;
  double ix_sf[3], x_testpoint[3], x_projtestpoint[3];

  double rad_sample, dv;
  double upper_bound, lower_bound;
  double inorm_bf[3], inorm_sf[3], dtor[3], jx_sf[3];
  double gp[3], gp_bf[3], gp_sf[3];
  double quat[4];
  double rot_np_bf[3][3], rot_np_sf[3][3];

  MathExtra::quat_to_mat(iquat_cont, rot_np_sf);
  MathExtra::quatquat(iquat_sf_bf, iquat_cont, quat);
  MathExtra::qnormalize(quat);
  MathExtra::quat_to_mat(quat, rot_np_bf);

  radtol = radius_tol*radi; // fraction of max radius
  n = 2*(num_pole_quad-1);
  cosang = std::cos(iang);
  iang = std::acos((abscissa[kk_count]*((1.0-cosang)/2.0)) + ((1.0+cosang)/2.0)); //refine spherical cap angle
  cosang = std::cos(iang);
  fac = ((1.0-cosang)/2.0)*(MY_2PI/double(n+1));

  for (kk = num_pole_quad-1; kk >= 0; kk--) {
//  for (kk = 0; kk < num_pole_quad; kk++) {
    theta_pole = std::acos((abscissa[kk]*((1.0-cosang)/2.0)) + ((1.0+cosang)/2.0));
    for (ll = 1; ll <= n+1; ll++) {
      phi_pole = MY_2PI * double(ll-1) / (double(n + 1));

      gp[0] = std::sin(theta_pole)*std::cos(phi_pole); // quadrature point at [0,0,1]
      gp[1] = std::sin(theta_pole)*std::sin(phi_pole);
      gp[2] = std::cos(theta_pole);

      MathExtra::matvec(rot_np_sf, gp, gp_sf); // quadrature point at contact in space frame
      phi_sf = std::atan2(gp_sf[1], gp_sf[0]);
      phi_sf = phi_sf > 0.0 ? phi_sf : MY_2PI + phi_sf;
      theta_sf = std::acos(gp_sf[2]);

      MathExtra::matvec(rot_np_bf, gp, gp_bf); // quadrature point at contact in body frame
      phi_bf = std::atan2(gp_bf[1], gp_bf[0]);
      phi_bf = phi_bf > 0.0 ? phi_bf : MY_2PI + phi_bf;
      theta_bf = std::acos(gp_bf[2]);

      // Get the radius at the body frame theta and phi value and normal [not unit]
      rad_body = avec->get_shape_radius_and_normal(ishtype, theta_bf, phi_bf, inorm_bf); // inorm is in body frame

      ix_sf[0] = (rad_body * sin(theta_sf) * cos(phi_sf)) + xi[0]; // Global coordinates of quadrature point
      ix_sf[1] = (rad_body * sin(theta_sf) * sin(phi_sf)) + xi[1];
      ix_sf[2] = (rad_body * cos(theta_sf)) + xi[2];
      // vector distance from COG of atom j (in space frame) to quadrature point on atom i
      MathExtra::sub3(ix_sf, xj, x_testpoint);
      // scalar distance
      dtemp = MathExtra::len3(x_testpoint);
      if (dtemp > radj) continue;
      // Rotating the projected point into atom j's body frame (rotation matrix transpose = inverse)
      MathExtra::transpose_matvec(jrot, x_testpoint, x_projtestpoint);
      // Get projected phi and theta angle of gauss point in atom i's body frame
      phi_proj = std::atan2(x_projtestpoint[1], x_projtestpoint[0]);
      phi_proj = phi_proj > 0.0 ? phi_proj : MY_2PI + phi_proj; // move atan2 range from 0 to 2pi
      theta_proj = std::acos(x_projtestpoint[2] / dtemp);

      // Check for contact
      if (avec->check_contact(jshtype, phi_proj, theta_proj, dtemp, finalrad)) {
        upper_bound = rad_body;
        lower_bound = 0.0;
        rad_sample = (upper_bound + lower_bound) / 2.0;
        while (upper_bound - lower_bound > radtol) {
          jx_sf[0] = (rad_sample * sin(theta_sf) * cos(phi_sf)) + xi[0]; // Global coordinates of point
          jx_sf[1] = (rad_sample * sin(theta_sf) * sin(phi_sf)) + xi[1];
          jx_sf[2] = (rad_sample * cos(theta_sf)) + xi[2];
          // vector distance from COG of atom j (in space frame) to test point on atom i
          MathExtra::sub3(jx_sf, xj, x_testpoint);
          // scalar distance
          dtemp = MathExtra::len3(x_testpoint);
          if (dtemp > radj) {
            lower_bound = rad_sample;  // sampled radius outside of particle j, increase the lower bound
          } else {
            // Rotating the projected point into atom j's body frame (rotation matrix transpose = inverse)
            MathExtra::transpose_matvec(jrot, x_testpoint, x_projtestpoint);
            // Get projected phi and theta angle of gauss point in atom i's body frame
            phi_proj = std::atan2(x_projtestpoint[1], x_projtestpoint[0]);
            phi_proj = phi_proj > 0.0 ? phi_proj : MY_2PI + phi_proj; // move atan2 range from 0 to 2pi
            theta_proj = std::acos(x_projtestpoint[2] / dtemp);
            if (avec->check_contact(jshtype, phi_proj, theta_proj, dtemp, finalrad)) {
              upper_bound = rad_sample; // sampled radius inside of particle j, decrease the upper bound
            } else {
              lower_bound = rad_sample;  // sampled radius outside of particle j, increase the lower bound
            }
          }
          rad_sample = (upper_bound + lower_bound) / 2.0;
        }

        dv = weights[kk] * (std::pow(rad_body, 3) - std::pow(rad_sample, 3));
        vol_overlap += dv;

        MathExtra::scale3(weights[kk]/std::sin(theta_bf), inorm_bf); // w_i * n * Q
        MathExtra::matvec(irot, inorm_bf, inorm_sf);            // w_i * n * Q in space frame
        MathExtra::add3(iforce, inorm_sf, iforce);              // sum(w_i * n * Q)
        MathExtra::sub3(ix_sf, xi, x_testpoint);                // Vector u from centre of "a" to surface point
        MathExtra::cross3(x_testpoint, inorm_sf, dtor);         // u x n_s * Q * w_i
        MathExtra::add3(torsum, dtor, torsum);                  // sum(u x n_s * Q * w_i)

        ///////////
//        double zero_norm[3];
//        MathExtra::zero3(zero_norm);
//        if (file_count % 1 == 0) {
//          if ((first_call) & (ii == 0) & (jj == 0)) {
//            first_call = false;
//            write_surfpoints_to_file(ix_sf, false, 1, 1, inorm_sf);
//            write_surfpoints_to_file(jx_sf, true, 0, 0, zero_norm);
//          } else if (ii == 0 & jj == 0) {
//            write_surfpoints_to_file(ix_sf, true, 1, 1, inorm_sf);
//            write_surfpoints_to_file(jx_sf, true, 0, 0, zero_norm);
//          }
//        }
        ///////////

      } // check_contact
    } // ll (quadrature)
  } // kk (quadrature)
  vol_overlap*=fac/3.0;
  MathExtra::scale3(fac, iforce);
  MathExtra::scale3(fac, torsum);
}


int PairSH::write_surfpoints_to_file(double *x, bool append_file, int cont, int ifnorm, double *norm) const{

  std::ofstream outfile;
  if (append_file){
//    outfile.open("test_dump/surfpoint_"+std::to_string(cur_time)+".csv", std::ios_base::app);
    outfile.open("test_dump/surfpoint_"+std::to_string(file_count)+".csv", std::ios_base::app);
    if (outfile.is_open()) {
      if (ifnorm) {
        outfile << std::setprecision(16) << x[0] << "," << x[1] << "," << x[2] << "," << cont <<
                "," << norm[0] << "," << norm[1] << "," << norm[2] << "\n";
      }
      else{
        outfile << std::setprecision(16) << x[0] << "," << x[1] << "," << x[2] << "," << cont <<
                "," << norm[0] << "," << norm[1] << "," << norm[2] << "\n";
      }
      outfile.close();
    } else std::cout << "Unable to open file";
  }
  else {
//    cur_time += update->dt;
//    outfile.open("test_dump/surfpoint_" + std::to_string(cur_time) + ".csv");
    outfile.open("test_dump/surfpoint_" + std::to_string(file_count) + ".csv");
    if (outfile.is_open()) {
      outfile << "x,y,z,cont,nx,ny,nz" << "\n";
      if (ifnorm) {
        outfile << std::setprecision(16) << x[0] << "," << x[1] << "," << x[2] << "," << cont <<
                "," << norm[0] << "," << norm[1] << "," << norm[2] << "\n";
      }
      else{
        outfile << std::setprecision(16) << x[0] << "," << x[1] << "," << x[2] << "," << cont <<
                "," << norm[0] << "," << norm[1] << "," << norm[2] << "\n";
      }
      outfile.close();
    } else std::cout << "Unable to open file";
  }
  return 0;
};

int PairSH::write_ellipsoid(double *xi, double *xj, double irotmat[3][3], double jrotmat[3][3]) const{

  double sa[3][3];
  double rotmatinv[3][3];
  double tempmat[3][3];
  double icurmat[3][3];
  double jcurmat[3][3];

  MathExtra::zeromat3(sa);
  sa[0][0] = 21.0;
  sa[1][1] = 21.0;
  sa[2][2] = 105.0;

  MathExtra::times3(irotmat, sa, tempmat);
  MathExtra::invert3(irotmat, rotmatinv);
  MathExtra::times3(tempmat, rotmatinv, icurmat);

  MathExtra::times3(jrotmat, sa, tempmat);
  MathExtra::invert3(jrotmat, rotmatinv);
  MathExtra::times3(tempmat, rotmatinv, jcurmat);

  std::ofstream outfile;
//  outfile.open("test_dump/ellipsoidpos_" + std::to_string(cur_time) + ".vtk");
  outfile.open("test_dump/ellipsoidpos_" + std::to_string(file_count) + ".vtk");
  if (outfile.is_open()) {
    outfile << "# vtk DataFile Version 3.0" << "\n";
    outfile << "vtk output" << "\n";
    outfile << "ASCII" << "\n";
    outfile << "DATASET POLYDATA" << "\n";
    outfile << "POINTS 2 float" << "\n";
    outfile << xi[0] << " " << xi[1] << " " << xi[2] << "\n";
    outfile << "\n";
    outfile << xj[0] << " " << xj[1] << " " << xj[2] << "\n";
    outfile << "\n";
    outfile << "POINT_DATA 2" << "\n";
    outfile << "TENSORS tensorF float" << "\n";
    outfile << icurmat[0][0] << " " << icurmat[0][1] << " " << icurmat[0][2] << "\n";
    outfile << icurmat[1][0] << " " << icurmat[1][1] << " " << icurmat[1][2] << "\n";
    outfile << icurmat[2][0] << " " << icurmat[2][1] << " " << icurmat[2][2] << "\n";
    outfile << "\n";
    outfile << jcurmat[0][0] << " " << jcurmat[0][1] << " " << jcurmat[0][2] << "\n";
    outfile << jcurmat[1][0] << " " << jcurmat[1][1] << " " << jcurmat[1][2] << "\n";
    outfile << jcurmat[2][0] << " " << jcurmat[2][1] << " " << jcurmat[2][2] << "\n";
    outfile << "\n";
    outfile.close();
  } else std::cout << "Unable to open file";
  return 0;
};