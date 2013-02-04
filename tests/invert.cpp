#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>

#include <util_quda.h>
#include <test_util.h>
#include <dslash_util.h>
#include <blas_reference.h>
#include <wilson_dslash_reference.h>
#include <domain_wall_dslash_reference.h>
#include "misc.h"

#include "face_quda.h"

#ifdef QMP_COMMS
#include <qmp.h>
#endif

#include <gauge_qio.h>

#define MAX(a,b) ((a)>(b)?(a):(b))

// In a typical application, quda.h is the only QUDA header required.
#include <quda.h>

// Wilson, clover-improved Wilson, twisted mass, and domain wall are supported.
extern QudaDslashType dslash_type;
extern bool tune;
extern int device;
extern int xdim;
extern int ydim;
extern int zdim;
extern int tdim;
extern int gridsize_from_cmdline[];
extern QudaReconstructType link_recon;
extern QudaPrecision prec;
extern QudaReconstructType link_recon_sloppy;
extern QudaPrecision  prec_sloppy;

extern char latfile[];

extern void usage(char** );

void
display_inversion_info()
{
  printfQuda("running the following inversion:\n");
    
  printfQuda("prec    sloppy_prec    link_recon  sloppy_link_recon S_dimension T_dimension\n");
  printfQuda("%s   %s             %s            %s            %d/%d/%d          %d \n",
	     get_prec_str(prec),get_prec_str(prec_sloppy),
	     get_recon_str(link_recon), 
	     get_recon_str(link_recon_sloppy),  xdim, ydim, zdim, tdim);     

  printfQuda("Grid partition info:     X  Y  Z  T\n"); 
  printfQuda("                         %d  %d  %d  %d\n", 
	     commDimPartitioned(0),
	     commDimPartitioned(1),
	     commDimPartitioned(2),
	     commDimPartitioned(3)); 
  
  return ;
  
}

int main(int argc, char **argv)
{
  int i;
  for (i =1;i < argc; i++){
    if(process_command_line_option(argc, argv, &i) == 0){
      continue;
    } 
    printfQuda("ERROR: Invalid option:%s\n", argv[i]);
    usage(argv);
  }

  if (prec_sloppy == QUDA_INVALID_PRECISION){
    prec_sloppy = prec;
  }
  if (link_recon_sloppy == QUDA_RECONSTRUCT_INVALID){
    link_recon_sloppy = link_recon;
  }

  initCommsQuda(argc, argv, gridsize_from_cmdline, 4);

  display_inversion_info();

  // *** QUDA parameters begin here.

  int multi_shift = 0; // whether to test multi-shift or standard solver

  if (dslash_type != QUDA_WILSON_DSLASH &&
      dslash_type != QUDA_CLOVER_WILSON_DSLASH &&
      dslash_type != QUDA_TWISTED_MASS_DSLASH &&
      dslash_type != QUDA_DOMAIN_WALL_DSLASH) {
    printfQuda("dslash_type %d not supported\n", dslash_type);
    exit(0);
  }

  QudaPrecision cpu_prec = QUDA_DOUBLE_PRECISION;
  QudaPrecision cuda_prec = prec;
  QudaPrecision cuda_prec_sloppy = prec_sloppy;
  QudaPrecision cuda_prec_precondition = QUDA_HALF_PRECISION;

  QudaGaugeParam gauge_param = newQudaGaugeParam();
  QudaInvertParam inv_param = newQudaInvertParam();
 
  int myLs = 8; // FIXME
  double kappa5;

  gauge_param.X[0] = xdim;
  gauge_param.X[1] = ydim;
  gauge_param.X[2] = zdim;
  gauge_param.X[3] = tdim;
  inv_param.Ls = (dslash_type == QUDA_DOMAIN_WALL_DSLASH) ? myLs : 1;

  gauge_param.anisotropy = 2.38;
  gauge_param.type = QUDA_WILSON_LINKS;
  gauge_param.gauge_order = QUDA_QDP_GAUGE_ORDER;
  gauge_param.t_boundary = QUDA_ANTI_PERIODIC_T;
  
  gauge_param.cpu_prec = cpu_prec;
  gauge_param.cuda_prec = cuda_prec;
  gauge_param.reconstruct = link_recon;
  gauge_param.cuda_prec_sloppy = cuda_prec_sloppy;
  gauge_param.reconstruct_sloppy = link_recon_sloppy;
  gauge_param.cuda_prec_precondition = cuda_prec_precondition;
  gauge_param.reconstruct_precondition = link_recon_sloppy;
  gauge_param.gauge_fix = QUDA_GAUGE_FIXED_NO;

  inv_param.dslash_type = dslash_type;

  //double mass = -0.4125;
  //inv_param.kappa = 1.0 / (2.0 * (1 + 3/gauge_param.anisotropy + mass));
  inv_param.kappa = 0.160856;

  if (dslash_type == QUDA_TWISTED_MASS_DSLASH) {
    inv_param.mu = 0.004;
    inv_param.twist_flavor = QUDA_TWIST_MINUS;
  } else if (dslash_type == QUDA_DOMAIN_WALL_DSLASH) {
    inv_param.mass = 0.02;
    inv_param.m5 = -1.8;
    kappa5 = 0.5/(5 + inv_param.m5);  
  }

  // offsets used only by multi-shift solver
  inv_param.num_offset = 4;
  double offset[QUDA_MAX_MULTI_SHIFT] = {0.01, 0.02, 0.03, 0.04};
  for (int i=0; i<inv_param.num_offset; i++) inv_param.offset[i] = offset[i];

  inv_param.solution_type = QUDA_MATPC_SOLUTION;
  inv_param.matpc_type = QUDA_MATPC_EVEN_EVEN;
  inv_param.dagger = QUDA_DAG_NO;
  inv_param.mass_normalization = QUDA_KAPPA_NORMALIZATION;

  if (dslash_type == QUDA_DOMAIN_WALL_DSLASH || dslash_type == QUDA_TWISTED_MASS_DSLASH) {
    inv_param.solve_type = QUDA_NORMOP_PC_SOLVE;
    inv_param.inv_type = QUDA_CG_INVERTER;
  } else {
    inv_param.solve_type = QUDA_DIRECT_PC_SOLVE;
    inv_param.inv_type = QUDA_BICGSTAB_INVERTER;
  }

  inv_param.gcrNkrylov = 10;
  inv_param.tol = 1e-13;
  //inv_param.residual_type = QUDA_HEAVY_QUARK_RESIDUAL;
  // these can be set individually
  for (int i=0; i<inv_param.num_offset; i++) inv_param.tol_offset[i] = inv_param.tol;
  inv_param.maxiter = 5000;
  inv_param.reliable_delta = 1e-1; // ignored by multi-shift solver

  // domain decomposition preconditioner parameters
  inv_param.inv_type_precondition = QUDA_INVALID_INVERTER;
  inv_param.schwarz_type = QUDA_ADDITIVE_SCHWARZ;
  inv_param.precondition_cycle = 1;
  inv_param.tol_precondition = 1e-1;
  inv_param.maxiter_precondition = 10;
  inv_param.verbosity_precondition = QUDA_SILENT;
  inv_param.cuda_prec_precondition = cuda_prec_precondition;
  inv_param.omega = 1.0;

  inv_param.cpu_prec = cpu_prec;
  inv_param.cuda_prec = cuda_prec;
  inv_param.cuda_prec_sloppy = cuda_prec_sloppy;
  inv_param.preserve_source = QUDA_PRESERVE_SOURCE_NO;
  inv_param.gamma_basis = QUDA_DEGRAND_ROSSI_GAMMA_BASIS;
  inv_param.dirac_order = QUDA_DIRAC_ORDER;

  inv_param.input_location = QUDA_CPU_FIELD_LOCATION;
  inv_param.output_location = QUDA_CPU_FIELD_LOCATION;

  inv_param.tune = tune ? QUDA_TUNE_YES : QUDA_TUNE_NO;

  gauge_param.ga_pad = 0; // 24*24*24/2;
  inv_param.sp_pad = 0; // 24*24*24/2;
  inv_param.cl_pad = 0; // 24*24*24/2;

  // For multi-GPU, ga_pad must be large enough to store a time-slice
#ifdef MULTI_GPU
  int x_face_size = gauge_param.X[1]*gauge_param.X[2]*gauge_param.X[3]/2;
  int y_face_size = gauge_param.X[0]*gauge_param.X[2]*gauge_param.X[3]/2;
  int z_face_size = gauge_param.X[0]*gauge_param.X[1]*gauge_param.X[3]/2;
  int t_face_size = gauge_param.X[0]*gauge_param.X[1]*gauge_param.X[2]/2;
  int pad_size =MAX(x_face_size, y_face_size);
  pad_size = MAX(pad_size, z_face_size);
  pad_size = MAX(pad_size, t_face_size);
  gauge_param.ga_pad = pad_size;    
#endif

  if (dslash_type == QUDA_CLOVER_WILSON_DSLASH) {
    inv_param.clover_cpu_prec = cpu_prec;
    inv_param.clover_cuda_prec = cuda_prec;
    inv_param.clover_cuda_prec_sloppy = cuda_prec_sloppy;
    inv_param.clover_cuda_prec_precondition = cuda_prec_precondition;
    inv_param.clover_order = QUDA_PACKED_CLOVER_ORDER;
  }

  inv_param.verbosity = QUDA_VERBOSE;

  // *** Everything between here and the call to initQuda() is
  // *** application-specific.

  // set parameters for the reference Dslash, and prepare fields to be loaded
  if (dslash_type == QUDA_DOMAIN_WALL_DSLASH) {
    dw_setDims(gauge_param.X, inv_param.Ls);
  } else {
    setDims(gauge_param.X);
    Ls = 1;
  }
  setSpinorSiteSize(24);

  size_t gSize = (gauge_param.cpu_prec == QUDA_DOUBLE_PRECISION) ? sizeof(double) : sizeof(float);
  size_t sSize = (inv_param.cpu_prec == QUDA_DOUBLE_PRECISION) ? sizeof(double) : sizeof(float);

  void *gauge[4], *clover_inv=0, *clover=0;
  printf("V = %d \n", V);
  for (int dir = 0; dir < 4; dir++) {
    gauge[dir] = malloc(V*gaugeSiteSize*gSize);
  }

  if (strcmp(latfile,"")) {  // load in the command line supplied gauge field
    read_gauge_field(latfile, gauge, gauge_param.cpu_prec, gauge_param.X, argc, argv);
    construct_gauge_field(gauge, 2, gauge_param.cpu_prec, &gauge_param);
  } else { // else generate a random SU(3) field
    construct_gauge_field(gauge, 1, gauge_param.cpu_prec, &gauge_param);
  }

  if (dslash_type == QUDA_CLOVER_WILSON_DSLASH) {
    double norm = 0.0; // clover components are random numbers in the range (-norm, norm)
    double diag = 1.0; // constant added to the diagonal

    size_t cSize = (inv_param.clover_cpu_prec == QUDA_DOUBLE_PRECISION) ? sizeof(double) : sizeof(float);
    clover_inv = malloc(V*cloverSiteSize*cSize);
    construct_clover_field(clover_inv, norm, diag, inv_param.clover_cpu_prec);

    // The uninverted clover term is only needed when solving the unpreconditioned
    // system or when using "asymmetric" even/odd preconditioning.
    int preconditioned = (inv_param.solve_type == QUDA_DIRECT_PC_SOLVE ||
			  inv_param.solve_type == QUDA_NORMOP_PC_SOLVE);
    int asymmetric = preconditioned &&
                         (inv_param.matpc_type == QUDA_MATPC_EVEN_EVEN_ASYMMETRIC ||
                          inv_param.matpc_type == QUDA_MATPC_ODD_ODD_ASYMMETRIC);
    if (!preconditioned) {
      clover = clover_inv;
      clover_inv = NULL;
    } else if (asymmetric) { // fake it by using the same random matrix
      clover = clover_inv;   // for both clover and clover_inv
    } else {
      clover = NULL;
    }
  }

  void *spinorIn = malloc(V*spinorSiteSize*sSize*inv_param.Ls);
  void *spinorCheck = malloc(V*spinorSiteSize*sSize*inv_param.Ls);

  void *spinorOut = NULL, **spinorOutMulti = NULL;
  if (multi_shift) {
    spinorOutMulti = (void**)malloc(inv_param.num_offset*sizeof(void *));
    for (int i=0; i<inv_param.num_offset; i++) {
      spinorOutMulti[i] = malloc(V*spinorSiteSize*sSize*inv_param.Ls);
    }
  } else {
    spinorOut = malloc(V*spinorSiteSize*sSize*inv_param.Ls);
  }

  // create a point source at 0 (in each subvolume...  FIXME)
  if (inv_param.cpu_prec == QUDA_SINGLE_PRECISION) *((float*)spinorIn) = 1.0;
  else *((double*)spinorIn) = 1.0;

  // start the timer
  double time0 = -((double)clock());

  // initialize the QUDA library
  initQuda(device);
printf("L296 reached! \n");
  // load the gauge field
  loadGaugeQuda((void*)gauge, &gauge_param);

  // load the clover term, if desired
  if (dslash_type == QUDA_CLOVER_WILSON_DSLASH) loadCloverQuda(clover, clover_inv, &inv_param);
  printf("L302 reached! \n");
  // perform the inversion
  if (multi_shift) {
    invertMultiShiftQuda(spinorOutMulti, spinorIn, &inv_param);
  } else {
    invertQuda(spinorOut, spinorIn, &inv_param);
  }
printf("L309 reached! \n");
  // stop the timer
  time0 += clock();
  time0 /= CLOCKS_PER_SEC;
    
  printfQuda("Device memory used:\n   Spinor: %f GiB\n    Gauge: %f GiB\n", 
	 inv_param.spinorGiB, gauge_param.gaugeGiB);
  if (dslash_type == QUDA_CLOVER_WILSON_DSLASH) printfQuda("   Clover: %f GiB\n", inv_param.cloverGiB);
  printfQuda("\nDone: %i iter / %g secs = %g Gflops, total time = %g secs\n", 
	 inv_param.iter, inv_param.secs, inv_param.gflops/inv_param.secs, time0);

  if (multi_shift) {

    void *spinorTmp = malloc(V*spinorSiteSize*sSize*inv_param.Ls);

    printfQuda("Host residuum checks: \n");
    for(int i=0; i < inv_param.num_offset; i++) {
      ax(0, spinorCheck, V*spinorSiteSize, inv_param.cpu_prec);
      
      if (dslash_type == QUDA_TWISTED_MASS_DSLASH) {
        tm_matpc(spinorTmp, gauge, spinorOutMulti[i], inv_param.kappa, inv_param.mu, inv_param.twist_flavor, 
                 inv_param.matpc_type, 0, inv_param.cpu_prec, gauge_param);
        tm_matpc(spinorCheck, gauge, spinorTmp, inv_param.kappa, inv_param.mu, inv_param.twist_flavor, 
                 inv_param.matpc_type, 1, inv_param.cpu_prec, gauge_param);
      } else if (dslash_type == QUDA_WILSON_DSLASH || dslash_type == QUDA_CLOVER_WILSON_DSLASH) {
        wil_matpc(spinorTmp, gauge, spinorOutMulti[i], inv_param.kappa, inv_param.matpc_type, 0,
                  inv_param.cpu_prec, gauge_param);
        wil_matpc(spinorCheck, gauge, spinorTmp, inv_param.kappa, inv_param.matpc_type, 1,
                  inv_param.cpu_prec, gauge_param);
      } else {
        printfQuda("Domain wall not supported for multi-shift\n");
        exit(-1);
      }

      axpy(inv_param.offset[i], spinorOutMulti[i], spinorCheck, V*spinorSiteSize, inv_param.cpu_prec);
      mxpy(spinorIn, spinorCheck, V*spinorSiteSize, inv_param.cpu_prec);
      double nrm2 = norm_2(spinorCheck, V*spinorSiteSize, inv_param.cpu_prec);
      double src2 = norm_2(spinorIn, V*spinorSiteSize, inv_param.cpu_prec);
      double l2r = sqrt(nrm2 / src2);

      printfQuda("Shift i=%d residuals: requested %g; relative QUDA = %g, host = %g; heavy-quark QUDA = %g\n",
		 i, inv_param.tol_offset[i], inv_param.true_res_offset[i], l2r, 
		 inv_param.true_res_hq_offset[i]);
    }
    free(spinorTmp);

  } else {
    
    if (inv_param.solution_type == QUDA_MAT_SOLUTION) {

      if (dslash_type == QUDA_TWISTED_MASS_DSLASH) {
        tm_mat(spinorCheck, gauge, spinorOut, inv_param.kappa, inv_param.mu, inv_param.twist_flavor, 
	           0, inv_param.cpu_prec, gauge_param); 
      } else if (dslash_type == QUDA_WILSON_DSLASH || dslash_type == QUDA_CLOVER_WILSON_DSLASH) {
        wil_mat(spinorCheck, gauge, spinorOut, inv_param.kappa, 0, inv_param.cpu_prec, gauge_param);
      } else if (dslash_type == QUDA_DOMAIN_WALL_DSLASH) {
        dw_mat(spinorCheck, gauge, spinorOut, kappa5, inv_param.dagger, inv_param.cpu_prec, gauge_param, inv_param.mass);
      } else {
        printfQuda("Unsupported dslash_type\n");
        exit(-1);
      }
      if (inv_param.mass_normalization == QUDA_MASS_NORMALIZATION) {
        if (dslash_type == QUDA_DOMAIN_WALL_DSLASH) {
          ax(0.5/kappa5, spinorCheck, V*spinorSiteSize*inv_param.Ls, inv_param.cpu_prec);
        } else {
          ax(0.5/inv_param.kappa, spinorCheck, V*spinorSiteSize, inv_param.cpu_prec);
        }
      }

    } else if(inv_param.solution_type == QUDA_MATPC_SOLUTION) {

      if (dslash_type == QUDA_TWISTED_MASS_DSLASH) {
        tm_matpc(spinorCheck, gauge, spinorOut, inv_param.kappa, inv_param.mu, inv_param.twist_flavor, 
                 inv_param.matpc_type, 0, inv_param.cpu_prec, gauge_param);
      } else if (dslash_type == QUDA_WILSON_DSLASH || dslash_type == QUDA_CLOVER_WILSON_DSLASH) {
        wil_matpc(spinorCheck, gauge, spinorOut, inv_param.kappa, inv_param.matpc_type, 0, 
                  inv_param.cpu_prec, gauge_param);
      } else if (dslash_type == QUDA_DOMAIN_WALL_DSLASH) {
	dw_matpc(spinorCheck, gauge, spinorOut, kappa5, inv_param.matpc_type, 0, inv_param.cpu_prec, gauge_param, inv_param.mass);
      } else {
        printfQuda("Unsupported dslash_type\n");
        exit(-1);
      }

      if (inv_param.mass_normalization == QUDA_MASS_NORMALIZATION) {
        if (dslash_type == QUDA_DOMAIN_WALL_DSLASH) {
          ax(0.25/(kappa5*kappa5), spinorCheck, V*spinorSiteSize*inv_param.Ls, inv_param.cpu_prec);
        } else {
          ax(0.25/(inv_param.kappa*inv_param.kappa), spinorCheck, V*spinorSiteSize, inv_param.cpu_prec);
      
	}
      }

    }
    FILE * resFile;
    resFile = fopen("conf_results", "w");
    setOutputFile(resFile);
    for (int X = 0; X < V; ++X){
    fprintf(resFile, "Propagator at x = (%d): ", X);
    printSpinorElement(spinorOut, X, inv_param.cpu_prec);
    };
    setOutputFile(stdout);
    fclose(resFile);

    mxpy(spinorIn, spinorCheck, V*spinorSiteSize*inv_param.Ls, inv_param.cpu_prec);
    double nrm2 = norm_2(spinorCheck, V*spinorSiteSize*inv_param.Ls, inv_param.cpu_prec);
    double src2 = norm_2(spinorIn, V*spinorSiteSize*inv_param.Ls, inv_param.cpu_prec);
    double l2r = sqrt(nrm2 / src2);

    printfQuda("Residuals: requested %g; relative QUDA = %g, host = %g; heavy-quark QUDA = %g\n",
	       inv_param.tol, inv_param.true_res, l2r, inv_param.true_res_hq);

  }

  freeGaugeQuda();
  if (dslash_type == QUDA_CLOVER_WILSON_DSLASH) freeCloverQuda();

  // finalize the QUDA library
  endQuda();

  // finalize the communications layer
  endCommsQuda();

  return 0;
}

