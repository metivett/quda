#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>

#include <quda.h>
#include <quda_internal.h>
#include <comm_quda.h>
#include <tune_quda.h>
#include <blas_quda.h>
#include <gauge_field.h>
#include <dirac_quda.h>
#include <dslash_quda.h>
#include <invert_quda.h>
#include <color_spinor_field.h>
#include <clover_field.h>
#include <llfat_quda.h>
#include <fat_force_quda.h>
#include <hisq_links_quda.h>

#ifdef NUMA_AFFINITY
#include <numa_affinity.h>
#endif

#include <cuda.h>

#ifdef MULTI_GPU
extern void exchange_cpu_sitelink_ex(int* X, int *R, void** sitelink, QudaGaugeFieldOrder cpu_order,
				     QudaPrecision gPrecision, int optflag);
#endif // MULTI_GPU

#ifdef GPU_GAUGE_FORCE
#include <gauge_force_quda.h>
#endif

#define MAX(a,b) ((a)>(b)? (a):(b))
#define TDIFF(a,b) (b.tv_sec - a.tv_sec + 0.000001*(b.tv_usec - a.tv_usec))

#define spinorSiteSize 24 // real numbers per spinor

#define MAX_GPU_NUM_PER_NODE 16

// define newQudaGaugeParam() and newQudaInvertParam()
#define INIT_PARAM
#include "check_params.h"
#undef INIT_PARAM

// define (static) checkGaugeParam() and checkInvertParam()
#define CHECK_PARAM
#include "check_params.h"
#undef CHECK_PARAM

// define printQudaGaugeParam() and printQudaInvertParam()
#define PRINT_PARAM
#include "check_params.h"
#undef PRINT_PARAM

#include "face_quda.h"

int numa_affinity_enabled = 1;

using namespace quda;

cudaGaugeField *gaugePrecise = NULL;
cudaGaugeField *gaugeSloppy = NULL;
cudaGaugeField *gaugePrecondition = NULL;

// It's important that these alias the above so that constants are set correctly in Dirac::Dirac()
cudaGaugeField *&gaugeFatPrecise = gaugePrecise;
cudaGaugeField *&gaugeFatSloppy = gaugeSloppy;
cudaGaugeField *&gaugeFatPrecondition = gaugePrecondition;

cudaGaugeField *gaugeLongPrecise = NULL;
cudaGaugeField *gaugeLongSloppy = NULL;
cudaGaugeField *gaugeLongPrecondition = NULL;

cudaCloverField *cloverPrecise = NULL;
cudaCloverField *cloverSloppy = NULL;
cudaCloverField *cloverPrecondition = NULL;

/*Dirac *diracPrecise = NULL;
Dirac *diracSloppy = NULL;
Dirac *diracPrecondition = NULL;
*/


cudaDeviceProp deviceProp;
cudaStream_t *streams;

static bool initialized = false;

//!< Profiler for initQuda
TimeProfile profileInit("initQuda");

//!< Profile for loadGaugeQuda / saveGaugeQuda
TimeProfile profileGauge("loadGaugeQuda");

//!< Profile for loadCloverQuda
TimeProfile profileClover("loadCloverQuda");

//!< Profiler for invertQuda
TimeProfile profileInvert("invertQuda");

//!< Profiler for invertMultiShiftQuda
TimeProfile profileMulti("invertMultiShiftQuda");

//!< Profiler for invertMultiShiftMixedQuda
TimeProfile profileMultiMixed("invertMultiShiftMixedQuda");

//!< Profiler for endQuda
TimeProfile profileEnd("endQuda");

int getGpuCount()
{
  int count;
  cudaGetDeviceCount(&count);
  if (count <= 0){
    errorQuda("No devices supporting CUDA");
  }
  if(count > MAX_GPU_NUM_PER_NODE){
    errorQuda("GPU count (%d) is larger than limit\n", count);
  }
  return count;
}


void setVerbosityQuda(QudaVerbosity verbosity, const char prefix[], FILE *outfile)
{
  setVerbosity(verbosity);
  setOutputPrefix(prefix);
  setOutputFile(outfile);
}


void initQuda(int dev)
{
  profileInit[QUDA_PROFILE_TOTAL].Start();

  //static bool initialized = false;
  if (initialized) return;
  initialized = true;

#if defined(GPU_DIRECT) && defined(MULTI_GPU) && (CUDA_VERSION == 4000)
  //check if CUDA_NIC_INTEROP is set to 1 in the enviroment
  // not needed for CUDA >= 4.1
  char* cni_str = getenv("CUDA_NIC_INTEROP");
  if(cni_str == NULL){
    errorQuda("Environment variable CUDA_NIC_INTEROP is not set\n");
  }
  int cni_int = atoi(cni_str);
  if (cni_int != 1){
    errorQuda("Environment variable CUDA_NIC_INTEROP is not set to 1\n");    
  }
#endif

  int deviceCount;
  cudaGetDeviceCount(&deviceCount);
  if (deviceCount == 0) {
    errorQuda("No devices supporting CUDA");
  }

  for(int i=0; i<deviceCount; i++) {
    cudaDeviceProp deviceProp;
    cudaGetDeviceProperties(&deviceProp, i);
    checkCudaErrorNoSync(); // "NoSync" for correctness in HOST_DEBUG mode
    if (getVerbosity() >= QUDA_SUMMARIZE) {
      printfQuda("Found device %d: %s\n", i, deviceProp.name);
    }
  }

#ifdef MULTI_GPU
  comm_init();
  if (dev < 0) dev = comm_gpuid();
#else
  if (dev < 0 || dev >= 16) errorQuda("Invalid device number %d", dev);
#endif

  cudaGetDeviceProperties(&deviceProp, dev);
  checkCudaErrorNoSync(); // "NoSync" for correctness in HOST_DEBUG mode
  if (deviceProp.major < 1) {
    errorQuda("Device %d does not support CUDA", dev);
  }
  
  if (getVerbosity() >= QUDA_SUMMARIZE) {
    printfQuda("Using device %d: %s\n", dev, deviceProp.name);
  }
  cudaSetDevice(dev);
  checkCudaErrorNoSync(); // "NoSync" for correctness in HOST_DEBUG mode

#ifdef NUMA_AFFINITY
  if(numa_affinity_enabled){
    setNumaAffinity(dev);
  }
#endif
  // if the device supports host-mapped memory, then enable this
  if(deviceProp.canMapHostMemory) cudaSetDeviceFlags(cudaDeviceMapHost);
  checkCudaError();

  cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
  //cudaDeviceSetSharedMemConfig(cudaSharedMemBankSizeEightByte);
  cudaGetDeviceProperties(&deviceProp, dev);

  streams = new cudaStream_t[Nstream];
  for (int i=0; i<Nstream; i++) {
    cudaStreamCreate(&streams[i]);
  }
  checkCudaError();
  createDslashEvents();

  initBlas();

  loadTuneCache(getVerbosity());

  profileInit[QUDA_PROFILE_TOTAL].Stop();
}


void loadGaugeQuda(void *h_gauge, QudaGaugeParam *param)
{
  profileGauge[QUDA_PROFILE_TOTAL].Start();

  if (!initialized) errorQuda("QUDA not initialized");
  if (getVerbosity() == QUDA_DEBUG_VERBOSE) printQudaGaugeParam(param);

  checkGaugeParam(param);

  // Set the specific cpu parameters and create the cpu gauge field
  GaugeFieldParam gauge_param(h_gauge, *param);

  cpuGaugeField cpu(gauge_param);

  profileGauge[QUDA_PROFILE_INIT].Start();  
  // switch the parameters for creating the mirror precise cuda gauge field
  gauge_param.create = QUDA_NULL_FIELD_CREATE;
  gauge_param.precision = param->cuda_prec;
  gauge_param.reconstruct = param->reconstruct;
  gauge_param.pad = param->ga_pad;
  gauge_param.order = (gauge_param.precision == QUDA_DOUBLE_PRECISION || 
		       gauge_param.reconstruct == QUDA_RECONSTRUCT_NO ) ?
    QUDA_FLOAT2_GAUGE_ORDER : QUDA_FLOAT4_GAUGE_ORDER;
  cudaGaugeField *precise = new cudaGaugeField(gauge_param);
  profileGauge[QUDA_PROFILE_INIT].Stop();  

  profileGauge[QUDA_PROFILE_H2D].Start();  
  precise->loadCPUField(cpu, QUDA_CPU_FIELD_LOCATION);

  param->gaugeGiB += precise->GBytes();

  // switch the parameters for creating the mirror sloppy cuda gauge field
  gauge_param.precision = param->cuda_prec_sloppy;
  gauge_param.reconstruct = param->reconstruct_sloppy;
  gauge_param.order = (gauge_param.precision == QUDA_DOUBLE_PRECISION || 
		       gauge_param.reconstruct == QUDA_RECONSTRUCT_NO ) ?
    QUDA_FLOAT2_GAUGE_ORDER : QUDA_FLOAT4_GAUGE_ORDER;
  cudaGaugeField *sloppy = NULL;
  if (param->cuda_prec != param->cuda_prec_sloppy) {
    sloppy = new cudaGaugeField(gauge_param);
    sloppy->loadCPUField(cpu, QUDA_CPU_FIELD_LOCATION);
    param->gaugeGiB += sloppy->GBytes();
  } else {
    sloppy = precise;
  }

  // switch the parameters for creating the mirror preconditioner cuda gauge field
  gauge_param.precision = param->cuda_prec_precondition;
  gauge_param.reconstruct = param->reconstruct_precondition;
  gauge_param.order = (gauge_param.precision == QUDA_DOUBLE_PRECISION || 
		       gauge_param.reconstruct == QUDA_RECONSTRUCT_NO ) ?
    QUDA_FLOAT2_GAUGE_ORDER : QUDA_FLOAT4_GAUGE_ORDER;
  cudaGaugeField *precondition = NULL;
  if (param->cuda_prec_sloppy != param->cuda_prec_precondition) {
    precondition = new cudaGaugeField(gauge_param);
    precondition->loadCPUField(cpu, QUDA_CPU_FIELD_LOCATION);
    param->gaugeGiB += precondition->GBytes();
  } else {
    precondition = sloppy;
  }
  profileGauge[QUDA_PROFILE_H2D].Stop();  
  
  switch (param->type) {
  case QUDA_WILSON_LINKS:
    //if (gaugePrecise) errorQuda("Precise gauge field already allocated");
    gaugePrecise = precise;
    //if (gaugeSloppy) errorQuda("Sloppy gauge field already allocated");
    gaugeSloppy = sloppy;
    //if (gaugePrecondition) errorQuda("Precondition gauge field already allocated");
    gaugePrecondition = precondition;
    break;
  case QUDA_ASQTAD_FAT_LINKS:
    if (gaugeFatPrecise) errorQuda("Precise gauge fat field already allocated");
    gaugeFatPrecise = precise;
    if (gaugeFatSloppy) errorQuda("Sloppy gauge fat field already allocated");
    gaugeFatSloppy = sloppy;
    if (gaugeFatPrecondition) errorQuda("Precondition gauge fat field already allocated");
    gaugeFatPrecondition = precondition;
    break;
  case QUDA_ASQTAD_LONG_LINKS:
    if (gaugeLongPrecise) errorQuda("Precise gauge long field already allocated");
    gaugeLongPrecise = precise;
    if (gaugeLongSloppy) errorQuda("Sloppy gauge long field already allocated");
    gaugeLongSloppy = sloppy;
    if (gaugeLongPrecondition) errorQuda("Precondition gauge long field already allocated");
    gaugeLongPrecondition = precondition;
    break;
  default:
    errorQuda("Invalid gauge type");   
  }

  profileGauge[QUDA_PROFILE_TOTAL].Stop();
}

void saveGaugeQuda(void *h_gauge, QudaGaugeParam *param)
{
  profileGauge[QUDA_PROFILE_TOTAL].Start();

  if (!initialized) errorQuda("QUDA not initialized");
  checkGaugeParam(param);

  // Set the specific cpu parameters and create the cpu gauge field
  GaugeFieldParam gauge_param(h_gauge, *param);
  cpuGaugeField cpuGauge(gauge_param);
  cudaGaugeField *cudaGauge = NULL;
  switch (param->type) {
  case QUDA_WILSON_LINKS:
    cudaGauge = gaugePrecise;
    break;
  case QUDA_ASQTAD_FAT_LINKS:
    cudaGauge = gaugeFatPrecise;
    break;
  case QUDA_ASQTAD_LONG_LINKS:
    cudaGauge = gaugeLongPrecise;
    break;
  default:
    errorQuda("Invalid gauge type");   
  }

  profileGauge[QUDA_PROFILE_D2H].Start();  
  cudaGauge->saveCPUField(cpuGauge, QUDA_CPU_FIELD_LOCATION);
  profileGauge[QUDA_PROFILE_D2H].Stop();  

  profileGauge[QUDA_PROFILE_TOTAL].Stop();
}


void loadCloverQuda(void *h_clover, void *h_clovinv, QudaInvertParam *inv_param)
{
  profileClover[QUDA_PROFILE_TOTAL].Start();

  pushVerbosity(inv_param->verbosity);
  if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printQudaInvertParam(inv_param);

  if (!initialized) errorQuda("QUDA not initialized");

  if (!h_clover && !h_clovinv) {
    errorQuda("loadCloverQuda() called with neither clover term nor inverse");
  }
  if (inv_param->clover_cpu_prec == QUDA_HALF_PRECISION) {
    errorQuda("Half precision not supported on CPU");
  }
  if (gaugePrecise == NULL) {
    errorQuda("Gauge field must be loaded before clover");
  }
  if (inv_param->dslash_type != QUDA_CLOVER_WILSON_DSLASH) {
    errorQuda("Wrong dslash_type in loadCloverQuda()");
  }

  // determines whether operator is preconditioned when calling invertQuda()
  bool pc_solve = (inv_param->solve_type == QUDA_DIRECT_PC_SOLVE ||
		   inv_param->solve_type == QUDA_NORMOP_PC_SOLVE);

  // determines whether operator is preconditioned when calling MatQuda() or MatDagMatQuda()
  bool pc_solution = (inv_param->solution_type == QUDA_MATPC_SOLUTION ||
		      inv_param->solution_type == QUDA_MATPCDAG_MATPC_SOLUTION);

  bool asymmetric = (inv_param->matpc_type == QUDA_MATPC_EVEN_EVEN_ASYMMETRIC ||
		     inv_param->matpc_type == QUDA_MATPC_ODD_ODD_ASYMMETRIC);

  // We issue a warning only when it seems likely that the user is screwing up:

  // inverted clover term is required when applying preconditioned operator
  if (!h_clovinv && pc_solve && pc_solution) {
    warningQuda("Inverted clover term not loaded");
  }

  // uninverted clover term is required when applying unpreconditioned operator,
  // but note that dslashQuda() is always preconditioned
  if (!h_clover && !pc_solve && !pc_solution) {
    //warningQuda("Uninverted clover term not loaded");
  }

  // uninverted clover term is also required for "asymmetric" preconditioning
  if (!h_clover && pc_solve && pc_solution && asymmetric) {
    warningQuda("Uninverted clover term not loaded");
  }

  CloverFieldParam clover_param;
  clover_param.nDim = 4;
  for (int i=0; i<4; i++) clover_param.x[i] = gaugePrecise->X()[i];
  clover_param.precision = inv_param->clover_cuda_prec;
  clover_param.pad = inv_param->cl_pad;

  profileClover[QUDA_PROFILE_H2D].Start();

  cloverPrecise = new cudaCloverField(h_clover, h_clovinv, inv_param->clover_cpu_prec, 
				      inv_param->clover_order, clover_param);
  inv_param->cloverGiB = cloverPrecise->GBytes();

  // create the mirror sloppy clover field
  if (inv_param->clover_cuda_prec != inv_param->clover_cuda_prec_sloppy) {
    clover_param.precision = inv_param->clover_cuda_prec_sloppy;
    cloverSloppy = new cudaCloverField(h_clover, h_clovinv, inv_param->clover_cpu_prec, 
				       inv_param->clover_order, clover_param); 
    inv_param->cloverGiB += cloverSloppy->GBytes();
  } else {
    cloverSloppy = cloverPrecise;
  }

  // create the mirror preconditioner clover field
  if (inv_param->clover_cuda_prec_sloppy != inv_param->clover_cuda_prec_precondition &&
      inv_param->clover_cuda_prec_precondition != QUDA_INVALID_PRECISION) {
    clover_param.precision = inv_param->clover_cuda_prec_precondition;
    cloverPrecondition = new cudaCloverField(h_clover, h_clovinv, inv_param->clover_cpu_prec, 
					     inv_param->clover_order, clover_param); 
    inv_param->cloverGiB += cloverPrecondition->GBytes();
  } else {
    cloverPrecondition = cloverSloppy;
  }
  profileClover[QUDA_PROFILE_H2D].Stop();

  popVerbosity();

  profileClover[QUDA_PROFILE_TOTAL].Stop();
}

void freeGaugeQuda(void) 
{  
  if (!initialized) errorQuda("QUDA not initialized");
  if (gaugeSloppy != gaugePrecondition && gaugePrecondition) delete gaugePrecondition;
  if (gaugePrecise != gaugeSloppy && gaugeSloppy) delete gaugeSloppy;
  if (gaugePrecise) delete gaugePrecise;

  gaugePrecondition = NULL;
  gaugeSloppy = NULL;
  gaugePrecise = NULL;

  if (gaugeLongSloppy != gaugeLongPrecondition && gaugeLongPrecondition) delete gaugeLongPrecondition;
  if (gaugeLongPrecise != gaugeLongSloppy && gaugeLongSloppy) delete gaugeLongSloppy;
  if (gaugeLongPrecise) delete gaugeLongPrecise;

  gaugeLongPrecondition = NULL;
  gaugeLongSloppy = NULL;
  gaugeLongPrecise = NULL;

  if (gaugeFatSloppy != gaugeFatPrecondition && gaugeFatPrecondition) delete gaugeFatPrecondition;
  if (gaugeFatPrecise != gaugeFatSloppy && gaugeFatSloppy) delete gaugeFatSloppy;
  if (gaugeFatPrecise) delete gaugeFatPrecise;
  
  gaugeFatPrecondition = NULL;
  gaugeFatSloppy = NULL;
  gaugeFatPrecise = NULL;
}


void freeCloverQuda(void)
{
  if (!initialized) errorQuda("QUDA not initialized");
  if (cloverPrecondition != cloverSloppy && cloverPrecondition) delete cloverPrecondition;
  if (cloverSloppy != cloverPrecise && cloverSloppy) delete cloverSloppy;
  if (cloverPrecise) delete cloverPrecise;

  cloverPrecondition = NULL;
  cloverSloppy = NULL;
  cloverPrecise = NULL;
}


void endQuda(void)
{
  profileEnd[QUDA_PROFILE_TOTAL].Start();

  if (!initialized) return;

  LatticeField::freeBuffer();
  cudaColorSpinorField::freeBuffer();
  cudaColorSpinorField::freeGhostBuffer();
  cpuColorSpinorField::freeGhostBuffer();
  FaceBuffer::flushPinnedCache();
  freeGaugeQuda();
  freeCloverQuda();

  endBlas();

  if (streams) {
    for (int i=0; i<Nstream; i++) cudaStreamDestroy(streams[i]);
    delete []streams;
    streams = NULL;
  }
  destroyDslashEvents();

  saveTuneCache(getVerbosity());

  // end this CUDA context
  cudaDeviceReset();

  initialized = false;

  profileEnd[QUDA_PROFILE_TOTAL].Stop();

  // print out the profile information of the lifetime of the library
  if (getVerbosity() >= QUDA_SUMMARIZE) {
    profileInit.Print();
    profileGauge.Print();
    profileClover.Print();
    profileInvert.Print();
    profileMulti.Print();
    profileMultiMixed.Print();
    profileEnd.Print();

    printfQuda("\n");
    printPeakMemUsage();
    printfQuda("\n");
  }

  assertAllMemFree();
}


namespace quda {

  void setDiracParam(DiracParam &diracParam, QudaInvertParam *inv_param, const bool pc)
  {
    double kappa = inv_param->kappa;
    if (inv_param->dirac_order == QUDA_CPS_WILSON_DIRAC_ORDER) {
      kappa *= gaugePrecise->Anisotropy();
    }

    switch (inv_param->dslash_type) {
    case QUDA_WILSON_DSLASH:
      diracParam.type = pc ? QUDA_WILSONPC_DIRAC : QUDA_WILSON_DIRAC;
      break;
    case QUDA_CLOVER_WILSON_DSLASH:
      diracParam.type = pc ? QUDA_CLOVERPC_DIRAC : QUDA_CLOVER_DIRAC;
      break;
    case QUDA_DOMAIN_WALL_DSLASH:
      diracParam.type = pc ? QUDA_DOMAIN_WALLPC_DIRAC : QUDA_DOMAIN_WALL_DIRAC;
      //BEGIN NEW :
      diracParam.Ls = inv_param->Ls;
      //END NEW    
      break;
    case QUDA_ASQTAD_DSLASH:
      diracParam.type = pc ? QUDA_ASQTADPC_DIRAC : QUDA_ASQTAD_DIRAC;
      break;
    case QUDA_TWISTED_MASS_DSLASH:
      diracParam.type = pc ? QUDA_TWISTED_MASSPC_DIRAC : QUDA_TWISTED_MASS_DIRAC;
      break;
    default:
      errorQuda("Unsupported dslash_type %d", inv_param->dslash_type);
    }

    diracParam.matpcType = inv_param->matpc_type;
    diracParam.dagger = inv_param->dagger;
    diracParam.gauge = gaugePrecise;
    diracParam.fatGauge = gaugeFatPrecise;
    diracParam.longGauge = gaugeLongPrecise;    
    diracParam.clover = cloverPrecise;
    diracParam.kappa = kappa;
    diracParam.mass = inv_param->mass;
    diracParam.m5 = inv_param->m5;
    diracParam.mu = inv_param->mu;
    diracParam.verbose = getVerbosity();

    for (int i=0; i<4; i++) {
      diracParam.commDim[i] = 1;   // comms are always on
    }
  }


  void setDiracSloppyParam(DiracParam &diracParam, QudaInvertParam *inv_param, const bool pc)
  {
    setDiracParam(diracParam, inv_param, pc);

    diracParam.gauge = gaugeSloppy;
    diracParam.fatGauge = gaugeFatSloppy;
    diracParam.longGauge = gaugeLongSloppy;    
    diracParam.clover = cloverSloppy;

    for (int i=0; i<4; i++) {
      diracParam.commDim[i] = 1;   // comms are always on
    }

  }

  // The preconditioner currently mimicks the sloppy operator with no comms
  void setDiracPreParam(DiracParam &diracParam, QudaInvertParam *inv_param, const bool pc)
  {
    setDiracParam(diracParam, inv_param, pc);

    diracParam.gauge = gaugePrecondition;
    diracParam.fatGauge = gaugeFatPrecondition;
    diracParam.longGauge = gaugeLongPrecondition;    
    diracParam.clover = cloverPrecondition;

    for (int i=0; i<4; i++) {
      diracParam.commDim[i] = 0; // comms are always off
    }

  }

  void createDirac(Dirac *&d, Dirac *&dSloppy, Dirac *&dPre, QudaInvertParam &param, const bool pc_solve)
  {
    DiracParam diracParam;
    DiracParam diracSloppyParam;
    DiracParam diracPreParam;
    
    setDiracParam(diracParam, &param, pc_solve);
    setDiracSloppyParam(diracSloppyParam, &param, pc_solve);
    setDiracPreParam(diracPreParam, &param, pc_solve);
    
    d = Dirac::create(diracParam); // create the Dirac operator   
    dSloppy = Dirac::create(diracSloppyParam);
    dPre = Dirac::create(diracPreParam);
  }

  void massRescale(QudaDslashType dslash_type, double &kappa, QudaSolutionType solution_type, 
		   QudaMassNormalization mass_normalization, cudaColorSpinorField &b)
  {   
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) {
      printfQuda("Mass rescale: Kappa is: %g\n", kappa);
      printfQuda("Mass rescale: mass normalization: %d\n", mass_normalization);
      double nin = norm2(b);
      printfQuda("Mass rescale: norm of source in = %g\n", nin);
    }
 
    if (dslash_type == QUDA_ASQTAD_DSLASH) {
      if (mass_normalization != QUDA_MASS_NORMALIZATION) {
	errorQuda("Staggered code only supports QUDA_MASS_NORMALIZATION");
      }
      return;
    }

    // multiply the source to compensate for normalization of the Dirac operator, if necessary
    switch (solution_type) {
    case QUDA_MAT_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION ||
	  mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	axCuda(2.0*kappa, b);
      }
      break;
    case QUDA_MATDAG_MAT_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION ||
	  mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	axCuda(4.0*kappa*kappa, b);
      }
      break;
    case QUDA_MATPC_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION) {
	axCuda(4.0*kappa*kappa, b);
      } else if (mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	axCuda(2.0*kappa, b);
      }
      break;
    case QUDA_MATPCDAG_MATPC_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION) {
	axCuda(16.0*pow(kappa,4), b);
      } else if (mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	axCuda(4.0*kappa*kappa, b);
      }
      break;
    default:
      errorQuda("Solution type %d not supported", solution_type);
    }

    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printfQuda("Mass rescale done\n");   
    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) {
      printfQuda("Mass rescale: Kappa is: %g\n", kappa);
      printfQuda("Mass rescale: mass normalization: %d\n", mass_normalization);
      double nin = norm2(b);
      printfQuda("Mass rescale: norm of source out = %g\n", nin);
    }

  }

  void massRescaleCoeff(QudaDslashType dslash_type, double &kappa, QudaSolutionType solution_type, 
			QudaMassNormalization mass_normalization, double &coeff)
  {    
    if (dslash_type == QUDA_ASQTAD_DSLASH) {
      if (mass_normalization != QUDA_MASS_NORMALIZATION) {
	errorQuda("Staggered code only supports QUDA_MASS_NORMALIZATION");
      }
      return;
    }

    // multiply the source to compensate for normalization of the Dirac operator, if necessary
    switch (solution_type) {
    case QUDA_MAT_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION ||
	  mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	coeff *= 2.0*kappa;
      }
      break;
    case QUDA_MATDAG_MAT_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION ||
	  mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	coeff *= 4.0*kappa*kappa;
      }
      break;
    case QUDA_MATPC_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION) {
	coeff *= 4.0*kappa*kappa;
      } else if (mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	coeff *= 2.0*kappa;
      }
      break;
    case QUDA_MATPCDAG_MATPC_SOLUTION:
      if (mass_normalization == QUDA_MASS_NORMALIZATION) {
	coeff*=16.0*pow(kappa,4);
      } else if (mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
	coeff*=4.0*kappa*kappa;
      }
      break;
    default:
      errorQuda("Solution type %d not supported", solution_type);
    }

    if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printfQuda("Mass rescale done\n");   
  }
}

/*void QUDA_DiracField(QUDA_DiracParam *param) {
  
  }*/

void dslashQuda(void *h_out, void *h_in, QudaInvertParam *inv_param, QudaParity parity)
{

  if (inv_param->dslash_type == QUDA_DOMAIN_WALL_DSLASH) setKernelPackT(true);
  if (gaugePrecise == NULL) errorQuda("Gauge field not allocated");
  if (cloverPrecise == NULL && inv_param->dslash_type == QUDA_CLOVER_WILSON_DSLASH) 
    errorQuda("Clover field not allocated");

  pushVerbosity(inv_param->verbosity);
  if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printQudaInvertParam(inv_param);

  ColorSpinorParam cpuParam(h_in, inv_param->input_location, *inv_param, gaugePrecise->X(), 1);

  ColorSpinorField *in_h = (inv_param->input_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : 
    static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));

  ColorSpinorParam cudaParam(cpuParam, *inv_param);
  cudaColorSpinorField in(*in_h, cudaParam);

  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*in_h);
    double gpu = norm2(in);
    printfQuda("In CPU %e CUDA %e\n", cpu, gpu);
  }

  cudaParam.create = QUDA_NULL_FIELD_CREATE;
  cudaColorSpinorField out(in, cudaParam);

  if (inv_param->dirac_order == QUDA_CPS_WILSON_DIRAC_ORDER) {
    if (parity == QUDA_EVEN_PARITY) {
      parity = QUDA_ODD_PARITY;
    } else {
      parity = QUDA_EVEN_PARITY;
    }
    axCuda(gaugePrecise->Anisotropy(), in);
  }
  bool pc = true;

  DiracParam diracParam;
  setDiracParam(diracParam, inv_param, pc);

  Dirac *dirac = Dirac::create(diracParam); // create the Dirac operator
  dirac->Dslash(out, in, parity); // apply the operator
  delete dirac; // clean up

  cpuParam.v = h_out;

  ColorSpinorField *out_h = (inv_param->output_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));
  *out_h = out;
  
  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*out_h);
    double gpu = norm2(out);
    printfQuda("Out CPU %e CUDA %e\n", cpu, gpu);
  }

  delete out_h;
  delete in_h;

  popVerbosity();
}


void MatQuda(void *h_out, void *h_in, QudaInvertParam *inv_param)
{
  pushVerbosity(inv_param->verbosity);

  if (inv_param->dslash_type == QUDA_DOMAIN_WALL_DSLASH) setKernelPackT(true);
  if (gaugePrecise == NULL) errorQuda("Gauge field not allocated");
  if (cloverPrecise == NULL && inv_param->dslash_type == QUDA_CLOVER_WILSON_DSLASH) 
    errorQuda("Clover field not allocated");
  if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printQudaInvertParam(inv_param);

  bool pc = (inv_param->solution_type == QUDA_MATPC_SOLUTION ||
	     inv_param->solution_type == QUDA_MATPCDAG_MATPC_SOLUTION);

  ColorSpinorParam cpuParam(h_in, inv_param->input_location, *inv_param, gaugePrecise->X(), pc);
  ColorSpinorField *in_h = (inv_param->input_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));

  ColorSpinorParam cudaParam(cpuParam, *inv_param);
  cudaColorSpinorField in(*in_h, cudaParam);

  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*in_h);
    double gpu = norm2(in);
    printfQuda("In CPU %e CUDA %e\n", cpu, gpu);
  }

  cudaParam.create = QUDA_NULL_FIELD_CREATE;
  cudaColorSpinorField out(in, cudaParam);

  DiracParam diracParam;
  setDiracParam(diracParam, inv_param, pc);

  Dirac *dirac = Dirac::create(diracParam); // create the Dirac operator
  dirac->M(out, in); // apply the operator
  delete dirac; // clean up

  double kappa = inv_param->kappa;
  if (pc) {
    if (inv_param->mass_normalization == QUDA_MASS_NORMALIZATION) {
      axCuda(0.25/(kappa*kappa), out);
    } else if (inv_param->mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
      axCuda(0.5/kappa, out);
    }
  } else {
    if (inv_param->mass_normalization == QUDA_MASS_NORMALIZATION ||
	inv_param->mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
      axCuda(0.5/kappa, out);
    }
  }

  cpuParam.v = h_out;

  ColorSpinorField *out_h = (inv_param->output_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));
  *out_h = out;

  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*out_h);
    double gpu = norm2(out);
    printfQuda("Out CPU %e CUDA %e\n", cpu, gpu);
  }

  delete out_h;
  delete in_h;

  popVerbosity();
}


void MatDagMatQuda(void *h_out, void *h_in, QudaInvertParam *inv_param)
{
  pushVerbosity(inv_param->verbosity);

  if (inv_param->dslash_type == QUDA_DOMAIN_WALL_DSLASH) setKernelPackT(true);
  if (!initialized) errorQuda("QUDA not initialized");
  if (gaugePrecise == NULL) errorQuda("Gauge field not allocated");
  if (cloverPrecise == NULL && inv_param->dslash_type == QUDA_CLOVER_WILSON_DSLASH) 
    errorQuda("Clover field not allocated");
  if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printQudaInvertParam(inv_param);

  bool pc = (inv_param->solution_type == QUDA_MATPC_SOLUTION ||
	     inv_param->solution_type == QUDA_MATPCDAG_MATPC_SOLUTION);

  ColorSpinorParam cpuParam(h_in, inv_param->input_location, *inv_param, gaugePrecise->X(), pc);
  ColorSpinorField *in_h = (inv_param->input_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));  

  ColorSpinorParam cudaParam(cpuParam, *inv_param);
  cudaColorSpinorField in(*in_h, cudaParam);
  
  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*in_h);
    double gpu = norm2(in);
    printfQuda("In CPU %e CUDA %e\n", cpu, gpu);
  }

  cudaParam.create = QUDA_NULL_FIELD_CREATE;
  cudaColorSpinorField out(in, cudaParam);

  //  double kappa = inv_param->kappa;
  //  if (inv_param->dirac_order == QUDA_CPS_WILSON_DIRAC_ORDER) kappa *= gaugePrecise->anisotropy;

  DiracParam diracParam;
  setDiracParam(diracParam, inv_param, pc);

  Dirac *dirac = Dirac::create(diracParam); // create the Dirac operator
  dirac->MdagM(out, in); // apply the operator
  delete dirac; // clean up

  double kappa = inv_param->kappa;
  if (pc) {
    if (inv_param->mass_normalization == QUDA_MASS_NORMALIZATION) {
      axCuda(1.0/pow(2.0*kappa,4), out);
    } else if (inv_param->mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
      axCuda(0.25/(kappa*kappa), out);
    }
  } else {
    if (inv_param->mass_normalization == QUDA_MASS_NORMALIZATION ||
	inv_param->mass_normalization == QUDA_ASYMMETRIC_MASS_NORMALIZATION) {
      axCuda(0.25/(kappa*kappa), out);
    }
  }

  cpuParam.v = h_out;

  ColorSpinorField *out_h = (inv_param->output_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));
  *out_h = out;

  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*out_h);
    double gpu = norm2(out);
    printfQuda("Out CPU %e CUDA %e\n", cpu, gpu);
  }

  delete out_h;
  delete in_h;

  popVerbosity();
}

quda::cudaGaugeField* checkGauge(QudaInvertParam *param) {
  quda::cudaGaugeField *cudaGauge = NULL;
  if (param->dslash_type != QUDA_ASQTAD_DSLASH) {
    if (gaugePrecise == NULL) errorQuda("Precise gauge field doesn't exist");
    if (gaugeSloppy == NULL) errorQuda("Sloppy gauge field doesn't exist");
    if (gaugePrecondition == NULL) errorQuda("Precondition gauge field doesn't exist");
    cudaGauge = gaugePrecise;
  } else {
    if (gaugeFatPrecise == NULL) errorQuda("Precise gauge fat field doesn't exist");
    if (gaugeFatSloppy == NULL) errorQuda("Sloppy gauge fat field doesn't exist");
    if (gaugeFatPrecondition == NULL) errorQuda("Precondition gauge fat field doesn't exist");

    if (gaugeLongPrecise == NULL) errorQuda("Precise gauge long field doesn't exist");
    if (gaugeLongSloppy == NULL) errorQuda("Sloppy gauge long field doesn't exist");
    if (gaugeLongPrecondition == NULL) errorQuda("Precondition gauge long field doesn't exist");
    cudaGauge = gaugeFatPrecise;
  }
  return cudaGauge;
}


void cloverQuda(void *h_out, void *h_in, QudaInvertParam *inv_param, QudaParity parity, int inverse)
{
  pushVerbosity(inv_param->verbosity);

  if (!initialized) errorQuda("QUDA not initialized");
  if (gaugePrecise == NULL) errorQuda("Gauge field not allocated");
  if (cloverPrecise == NULL) errorQuda("Clover field not allocated");

  if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printQudaInvertParam(inv_param);

  if (inv_param->dslash_type != QUDA_CLOVER_WILSON_DSLASH)
    errorQuda("Cannot apply the clover term for a non Wilson-clover dslash");

  ColorSpinorParam cpuParam(h_in, inv_param->input_location, *inv_param, gaugePrecise->X(), 1);

  ColorSpinorField *in_h = (inv_param->input_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : 
    static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));

  ColorSpinorParam cudaParam(cpuParam, *inv_param);
  cudaColorSpinorField in(*in_h, cudaParam);

  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*in_h);
    double gpu = norm2(in);
    printfQuda("In CPU %e CUDA %e\n", cpu, gpu);
  }

  cudaParam.create = QUDA_NULL_FIELD_CREATE;
  cudaColorSpinorField out(in, cudaParam);

  if (inv_param->dirac_order == QUDA_CPS_WILSON_DIRAC_ORDER) {
    if (parity == QUDA_EVEN_PARITY) {
      parity = QUDA_ODD_PARITY;
    } else {
      parity = QUDA_EVEN_PARITY;
    }
    axCuda(gaugePrecise->Anisotropy(), in);
  }
  bool pc = true;

  DiracParam diracParam;
  setDiracParam(diracParam, inv_param, pc);

  DiracCloverPC dirac(diracParam); // create the Dirac operator
  if (!inverse) dirac.Clover(out, in, parity); // apply the clover operator
  else dirac.CloverInv(out, in, parity);

  cpuParam.v = h_out;

  ColorSpinorField *out_h = (inv_param->output_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));
  *out_h = out;
  
  if (getVerbosity() >= QUDA_VERBOSE) {
    double cpu = norm2(*out_h);
    double gpu = norm2(out);
    printfQuda("Out CPU %e CUDA %e\n", cpu, gpu);
  }

  /*for (int i=0; i<in_h->Volume(); i++) {
    ((cpuColorSpinorField*)out_h)->PrintVector(i);
    }*/

  delete out_h;
  delete in_h;

  popVerbosity();
}


void invertQuda(void *hp_x, void *hp_b, QudaInvertParam *param)
{
  profileInvert[QUDA_PROFILE_TOTAL].Start();

  if (!initialized) errorQuda("QUDA not initialized");
  if (param->dslash_type == QUDA_DOMAIN_WALL_DSLASH) setKernelPackT(true);

  pushVerbosity(param->verbosity);
  if (getVerbosity() >= QUDA_DEBUG_VERBOSE) printQudaInvertParam(param);

  // check the gauge fields have been created
  cudaGaugeField *cudaGauge = checkGauge(param);

  checkInvertParam(param);

  // It was probably a bad design decision to encode whether the system is even/odd preconditioned (PC) in
  // solve_type and solution_type, rather than in separate members of QudaInvertParam.  We're stuck with it
  // for now, though, so here we factorize everything for convenience.

  bool pc_solution = (param->solution_type == QUDA_MATPC_SOLUTION) || (param->solution_type == QUDA_MATPCDAG_MATPC_SOLUTION);
  bool pc_solve = (param->solve_type == QUDA_DIRECT_PC_SOLVE) || (param->solve_type == QUDA_NORMOP_PC_SOLVE);
  bool mat_solution = (param->solution_type == QUDA_MAT_SOLUTION) || (param->solution_type ==  QUDA_MATPC_SOLUTION);
  bool direct_solve = (param->solve_type == QUDA_DIRECT_SOLVE) || (param->solve_type == QUDA_DIRECT_PC_SOLVE);

  param->spinorGiB = cudaGauge->VolumeCB() * spinorSiteSize;
  if (!pc_solve) param->spinorGiB *= 2;
  param->spinorGiB *= (param->cuda_prec == QUDA_DOUBLE_PRECISION ? sizeof(double) : sizeof(float));
  if (param->preserve_source == QUDA_PRESERVE_SOURCE_NO) {
    param->spinorGiB *= (param->inv_type == QUDA_CG_INVERTER ? 5 : 7)/(double)(1<<30);
  } else {
    param->spinorGiB *= (param->inv_type == QUDA_CG_INVERTER ? 8 : 9)/(double)(1<<30);
  }

  param->secs = 0;
  param->gflops = 0;
  param->iter = 0;

  Dirac *d = NULL;
  Dirac *dSloppy = NULL;
  Dirac *dPre = NULL;

  // create the dirac operator
  createDirac(d, dSloppy, dPre, *param, pc_solve);

  Dirac &dirac = *d;
  Dirac &diracSloppy = *dSloppy;
  Dirac &diracPre = *dPre;

  profileInvert[QUDA_PROFILE_H2D].Start();

  cudaColorSpinorField *b = NULL;
  cudaColorSpinorField *x = NULL;
  cudaColorSpinorField *in = NULL;
  cudaColorSpinorField *out = NULL;

  const int *X = cudaGauge->X();

  // Print dimensions of the gauge
  for (int d=0; d < 4; ++d)
	  printfQuda("Gauge dims : X[%i]=%i \n", d, X[d]);

  // wrap CPU host side pointers
  ColorSpinorParam cpuParam(hp_b, param->input_location, *param, X, pc_solution);
  ColorSpinorField *h_b = (param->input_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));

  cpuParam.v = hp_x;
  ColorSpinorField *h_x = (param->input_location == QUDA_CPU_FIELD_LOCATION) ?
    static_cast<ColorSpinorField*>(new cpuColorSpinorField(cpuParam)) : static_cast<ColorSpinorField*>(new cudaColorSpinorField(cpuParam));

  // download source
  ColorSpinorParam cudaParam(cpuParam, *param);
  cudaParam.create = QUDA_COPY_FIELD_CREATE;

  // Print dimensions of the cudaColorSpinorField
    for (int d=0; d < 4; ++d)
  	  printfQuda("cudaColorSpinorField dims : X[%i]=%i \n", d, cudaParam.x[d]);

  b = new cudaColorSpinorField(*h_b, cudaParam); 

  if (param->use_init_guess == QUDA_USE_INIT_GUESS_YES) { // download initial guess
    // initial guess only supported for single-pass solvers
    if ((param->solution_type == QUDA_MATDAG_MAT_SOLUTION || param->solution_type == QUDA_MATPCDAG_MATPC_SOLUTION) &&
	(param->solve_type == QUDA_DIRECT_SOLVE || param->solve_type == QUDA_DIRECT_PC_SOLVE)) {
      errorQuda("Initial guess not supported for two-pass solver");
    }

    x = new cudaColorSpinorField(*h_x, cudaParam); // solution  
  } else { // zero initial guess
    cudaParam.create = QUDA_ZERO_FIELD_CREATE;
    x = new cudaColorSpinorField(cudaParam); // solution
  }

  if (param->residual_type == QUDA_HEAVY_QUARK_RESIDUAL && 
      (param->inv_type != QUDA_CG_INVERTER && param->inv_type != QUDA_BICGSTAB_INVERTER) ) {
    errorQuda("Heavy quark residual only supported for CG and BiCGStab");
  }
    
  profileInvert[QUDA_PROFILE_H2D].Stop();

  if (getVerbosity() >= QUDA_VERBOSE) {
    double nh_b = norm2(*h_b);
    double nb = norm2(*b);
    double nh_x = norm2(*h_x);
    double nx = norm2(*x);
    printfQuda("Source: CPU = %g, CUDA copy = %g\n", nh_b, nb);
    printfQuda("Solution: CPU = %g, CUDA copy = %g\n", nh_x, nx);
  }

  setDslashTuning(param->tune, getVerbosity());
  setBlasTuning(param->tune, getVerbosity());

  dirac.prepare(in, out, *x, *b, param->solution_type);
  if (getVerbosity() >= QUDA_VERBOSE) {
    double nin = norm2(*in);
    double nout = norm2(*out);
    printfQuda("Prepared source = %g\n", nin);   
    printfQuda("Prepared solution = %g\n", nout);   
  }

  massRescale(param->dslash_type, param->kappa, param->solution_type, param->mass_normalization, *in);

  if (getVerbosity() >= QUDA_VERBOSE) {
    double nin = norm2(*in);
    printfQuda("Prepared source post mass rescale = %g\n", nin);   
  }
  
  // solution_type specifies *what* system is to be solved.
  // solve_type specifies *how* the system is to be solved.
  //
  // We have the following four cases (plus preconditioned variants):
  //
  // solution_type    solve_type    Effect
  // -------------    ----------    ------
  // MAT              DIRECT        Solve Ax=b
  // MATDAG_MAT       DIRECT        Solve A^dag y = b, followed by Ax=y
  // MAT              NORMOP        Solve (A^dag A) x = (A^dag b)
  // MATDAG_MAT       NORMOP        Solve (A^dag A) x = b
  //
  // We generally require that the solution_type and solve_type
  // preconditioning match.  As an exception, the unpreconditioned MAT
  // solution_type may be used with any solve_type, including
  // DIRECT_PC and NORMOP_PC.  In these cases, preparation of the
  // preconditioned source and reconstruction of the full solution are
  // taken care of by Dirac::prepare() and Dirac::reconstruct(),
  // respectively.

  if (pc_solution && !pc_solve) {
    errorQuda("Preconditioned (PC) solution_type requires a PC solve_type");
  }

  if (!mat_solution && !pc_solution && pc_solve) {
    errorQuda("Unpreconditioned MATDAG_MAT solution_type requires an unpreconditioned solve_type");
  }

  if (mat_solution && !direct_solve) { // prepare source: b' = A^dag b
    cudaColorSpinorField tmp(*in);
    dirac.Mdag(*in, tmp);
  } else if (!mat_solution && direct_solve) { // perform the first of two solves: A^dag y = b
    DiracMdag m(dirac), mSloppy(diracSloppy), mPre(diracPre);
    Solver *solve = Solver::create(*param, m, mSloppy, mPre, profileInvert);
    (*solve)(*out, *in);
    copyCuda(*in, *out);
    delete solve;
  }

  if (direct_solve) {
    DiracM m(dirac), mSloppy(diracSloppy), mPre(diracPre);
    Solver *solve = Solver::create(*param, m, mSloppy, mPre, profileInvert);
    (*solve)(*out, *in);
    delete solve;
  } else {
    DiracMdagM m(dirac), mSloppy(diracSloppy), mPre(diracPre);
    Solver *solve = Solver::create(*param, m, mSloppy, mPre, profileInvert);
    (*solve)(*out, *in);
    delete solve;
  }

  if (getVerbosity() >= QUDA_VERBOSE){
   double nx = norm2(*x);
   printfQuda("Solution = %g\n",nx);
  }
  dirac.reconstruct(*x, *b, param->solution_type);
  
  profileInvert[QUDA_PROFILE_D2H].Start();
  *h_x = *x;
  profileInvert[QUDA_PROFILE_D2H].Stop();
  
  if (getVerbosity() >= QUDA_VERBOSE){
    double nx = norm2(*x);
    double nh_x = norm2(*h_x);
    printfQuda("Reconstructed: CUDA solution = %g, CPU copy = %g\n", nx, nh_x);
  }
  
  delete h_b;
  delete h_x;
  delete b;
  delete x;

  delete d;
  delete dSloppy;
  delete dPre;

  popVerbosity();

  // FIXME: added temporarily so that the cache is written out even if a long benchmarking job gets interrupted
  saveTuneCache(getVerbosity());

  profileInvert[QUDA_PROFILE_TOTAL].Stop();
}


/*! 
 * Generic version of the multi-shift solver. Should work for
 * most fermions. Note that offset[0] is not folded into the mass parameter.
 *
 * At present, the solution_type must be MATDAG_MAT or MATPCDAG_MATPC,
 * and solve_type must be NORMOP or NORMOP_PC.  The solution and solve
 * preconditioning have to match.
 */
void invertMultiShiftQuda(void **_hp_x, void *_hp_b, QudaInvertParam *param)
{
  profileMulti[QUDA_PROFILE_TOTAL].Start();

  if (param->dslash_type == QUDA_DOMAIN_WALL_DSLASH) setKernelPackT(true);
  if (!initialized) errorQuda("QUDA not initialized");
  // check the gauge fields have been created
  cudaGaugeField *cudaGauge = checkGauge(param);
  checkInvertParam(param);

  if (param->num_offset > QUDA_MAX_MULTI_SHIFT) 
    errorQuda("Number of shifts %d requested greater than QUDA_MAX_MULTI_SHIFT %d", 
	      param->num_offset, QUDA_MAX_MULTI_SHIFT);

  pushVerbosity(param->verbosity);

  bool pc_solution = (param->solution_type == QUDA_MATPC_SOLUTION) || (param->solution_type == QUDA_MATPCDAG_MATPC_SOLUTION);
  bool pc_solve = (param->solve_type == QUDA_DIRECT_PC_SOLVE) || (param->solve_type == QUDA_NORMOP_PC_SOLVE);
  bool mat_solution = (param->solution_type == QUDA_MAT_SOLUTION) || (param->solution_type ==  QUDA_MATPC_SOLUTION);
  bool direct_solve = (param->solve_type == QUDA_DIRECT_SOLVE) || (param->solve_type == QUDA_DIRECT_PC_SOLVE);

  if (mat_solution) {
    errorQuda("Multi-shift solver does not support MAT or MATPC solution types");
  }
  if (direct_solve) {
    errorQuda("Multi-shift solver does not support DIRECT or DIRECT_PC solve types");
  }
  if (pc_solution & !pc_solve) {
    errorQuda("Preconditioned (PC) solution_type requires a PC solve_type");
  }
  if (!pc_solution & pc_solve) {
    errorQuda("In multi-shift solver, a preconditioned (PC) solve_type requires a PC solution_type");
  }

  // No of GiB in a checkerboard of a spinor
  param->spinorGiB = cudaGauge->VolumeCB() * spinorSiteSize;
  if( !pc_solve) param->spinorGiB *= 2; // Double volume for non PC solve
  
  // **** WARNING *** this may not match implementation... 
  if( param->inv_type == QUDA_CG_INVERTER ) { 
    // CG-M needs 5 vectors for the smallest shift + 2 for each additional shift
    param->spinorGiB *= (5 + 2*(param->num_offset-1))/(double)(1<<30);
  } else {
    errorQuda("QUDA only currently supports multi-shift CG");
    // BiCGStab-M needs 7 for the original shift + 2 for each additional shift + 1 auxiliary
    // (Jegerlehner hep-lat/9612014 eq (3.13)
    param->spinorGiB *= (7 + 2*(param->num_offset-1))/(double)(1<<30);
  }

  // Timing and FLOP counters
  param->secs = 0;
  param->gflops = 0;
  param->iter = 0;
  
  for (int i=0; i<param->num_offset-1; i++) {
    for (int j=i+1; j<param->num_offset; j++) {
      if (param->offset[i] > param->offset[j])
	errorQuda("Offsets must be ordered from smallest to largest");
    }
  }
  
  // Host pointers for x, take a copy of the input host pointers
  void** hp_x;
  hp_x = new void* [ param->num_offset ];

  void* hp_b = _hp_b;
  for(int i=0;i < param->num_offset;i++){
    hp_x[i] = _hp_x[i];
  }
  
  // Create the matrix.
  // The way this works is that createDirac will create 'd' and 'dSloppy'
  // which are global. We then grab these with references...
  //
  // Balint: Isn't there a nice construction pattern we could use here? This is 
  // expedient but yucky.
  //  DiracParam diracParam; 
  if (param->dslash_type == QUDA_ASQTAD_DSLASH){
    param->mass = sqrt(param->offset[0]/4);  
  }

  Dirac *d = NULL;
  Dirac *dSloppy = NULL;
  Dirac *dPre = NULL;

  // create the dirac operator
  createDirac(d, dSloppy, dPre, *param, pc_solve);
  Dirac &dirac = *d;
  Dirac &diracSloppy = *dSloppy;

  cpuColorSpinorField *h_b = NULL; // Host RHS
  cpuColorSpinorField **h_x = NULL;
  cudaColorSpinorField *b = NULL;   // Cuda RHS
  cudaColorSpinorField **x = NULL;  // Cuda Solutions

  // Grab the dimension array of the input gauge field.
  const int *X = ( param->dslash_type == QUDA_ASQTAD_DSLASH ) ? 
    gaugeFatPrecise->X() : gaugePrecise->X();

  // Wrap CPU host side pointers
  // 
  // Balint: This creates a ColorSpinorParam struct, from the host data pointer, 
  // the definitions in param, the dimensions X, and whether the solution is on 
  // a checkerboard instruction or not. These can then be used as 'instructions' 
  // to create the actual colorSpinorField
  ColorSpinorParam cpuParam(hp_b, QUDA_CPU_FIELD_LOCATION, *param, X, pc_solution);
  h_b = new cpuColorSpinorField(cpuParam);

  h_x = new cpuColorSpinorField* [ param->num_offset ]; // DYNAMIC ALLOCATION
  for(int i=0; i < param->num_offset; i++) { 
    cpuParam.v = hp_x[i];
    h_x[i] = new cpuColorSpinorField(cpuParam);
  }


  profileMulti[QUDA_PROFILE_H2D].Start();
  // Now I need a colorSpinorParam for the device
  ColorSpinorParam cudaParam(cpuParam, *param);
  // This setting will download a host vector
  cudaParam.create = QUDA_COPY_FIELD_CREATE;
  b = new cudaColorSpinorField(*h_b, cudaParam); // Creates b and downloads h_b to it
  profileMulti[QUDA_PROFILE_H2D].Stop();

  // Create the solution fields filled with zero
  x = new cudaColorSpinorField* [ param->num_offset ];
  cudaParam.create = QUDA_ZERO_FIELD_CREATE;
  for(int i=0; i < param->num_offset; i++) { 
    x[i] = new cudaColorSpinorField(cudaParam);
  }

  // Check source norms
  if(getVerbosity() >= QUDA_VERBOSE ) {
    double nh_b = norm2(*h_b);
    double nb = norm2(*b);
    printfQuda("Source: CPU = %g, CUDA copy = %g\n", nh_b, nb);
  }

  setDslashTuning(param->tune, getVerbosity());
  setBlasTuning(param->tune, getVerbosity());
  
  massRescale(param->dslash_type, param->kappa, param->solution_type, param->mass_normalization, *b);
  double *unscaled_shifts = new double [param->num_offset];
  for(int i=0; i < param->num_offset; i++){ 
    unscaled_shifts[i] = param->offset[i];
    massRescaleCoeff(param->dslash_type, param->kappa, param->solution_type, param->mass_normalization, param->offset[i]);
  }

  // use multi-shift CG
  {
    DiracMdagM m(dirac), mSloppy(diracSloppy);
    MultiShiftCG cg_m(m, mSloppy, *param, profileMulti);
    cg_m(x, *b);  
  }

  // experimenting with Minimum residual extrapolation
  /*
  cudaColorSpinorField **q = new cudaColorSpinorField* [ param->num_offset ];
  cudaColorSpinorField **z = new cudaColorSpinorField* [ param->num_offset ];
  cudaColorSpinorField tmp(cudaParam);

  for(int i=0; i < param->num_offset; i++) {
    cudaParam.create = QUDA_ZERO_FIELD_CREATE;
    q[i] = new cudaColorSpinorField(cudaParam);
    cudaParam.create = QUDA_COPY_FIELD_CREATE;
    z[i] = new cudaColorSpinorField(*x[i], cudaParam);
  }

  for(int i=0; i < param->num_offset; i++) {
    dirac.setMass(sqrt(param->offset[i]/4));  
    DiracMdagM m(dirac);
    MinResExt mre(m, profileMulti);
    copyCuda(tmp, *b);
    mre(*x[i], tmp, z, q, param -> num_offset);
    dirac.setMass(sqrt(param->offset[0]/4));  
  }

  for(int i=0; i < param->num_offset; i++) {
    delete q[i];
    delete z[i];
  }
  delete []q;
  delete []z;
  */

  // check each shift has the desired tolerance and use sequential CG to refine
  
  cudaParam.create = QUDA_ZERO_FIELD_CREATE;
  cudaColorSpinorField r(*b, cudaParam);
  for(int i=0; i < param->num_offset; i++) { 
    if (param->dslash_type == QUDA_ASQTAD_DSLASH ) { 

      dirac.setMass(sqrt(param->offset[i]/4));  
      diracSloppy.setMass(sqrt(param->offset[i]/4));  

      double rsd = param->residual_type == QUDA_HEAVY_QUARK_RESIDUAL ?
	param->true_res_hq_offset[i] : param->true_res_offset[i];
      
      double tol = param->residual_type == QUDA_HEAVY_QUARK_RESIDUAL ?
	param->tol_hq_offset[i] : param->tol_offset[i];

      if (rsd > tol) {
	if (getVerbosity() >= QUDA_VERBOSE) 
	  printfQuda("Refining shift %d since achieved residual %e is greater than requested %e\n",
		     i, rsd, tol);
	DiracMdagM m(dirac), mSloppy(diracSloppy);

	param->use_init_guess = QUDA_USE_INIT_GUESS_YES;
	param->tol = tol;
	CG cg(m, mSloppy, *param, profileMulti);
	cg(*x[i], *b);        
	param->true_res_offset[i] = param->true_res;
	param->true_res_hq_offset[i] = param->true_res_hq;
      }

      dirac.setMass(sqrt(param->offset[0]/4)); // restore just in case
      diracSloppy.setMass(sqrt(param->offset[0]/4)); // restore just in case
    } else {
      warningQuda("Refinement only supported on staggered quarks currently");
    }
  }

  // restore shifts -- avoid side effects
  for(int i=0; i < param->num_offset; i++) { 
    param->offset[i] = unscaled_shifts[i];
  }

  delete [] unscaled_shifts;

  profileMulti[QUDA_PROFILE_D2H].Start();
  for(int i=0; i < param->num_offset; i++) { 
    if (getVerbosity() >= QUDA_VERBOSE){
      double nx = norm2(*x[i]);
      printfQuda("Solution %d = %g\n", i, nx);
    }

    *h_x[i] = *x[i];
  }
  profileMulti[QUDA_PROFILE_D2H].Stop();

  for(int i=0; i < param->num_offset; i++){ 
    delete h_x[i];
    delete x[i];
  }

  delete h_b;
  delete b;

  delete [] h_x;
  delete [] x;

  delete [] hp_x;

  delete d;
  delete dSloppy;
  delete dPre;
  
  popVerbosity();

  // FIXME: added temporarily so that the cache is written out even if a long benchmarking job gets interrupted
  saveTuneCache(getVerbosity());

  profileMulti[QUDA_PROFILE_TOTAL].Stop();
}


#ifdef GPU_FATLINK 
/*   @method  
 *   QUDA_COMPUTE_FAT_STANDARD: standard method (default)
 *   QUDA_COMPUTE_FAT_EXTENDED_VOLUME, extended volume method
 *
 */
#include <sys/time.h>

void setFatLinkPadding(QudaComputeFatMethod method, QudaGaugeParam* param)
{
  int* X    = param->X;
#ifdef MULTI_GPU
  int Vsh_x = X[1]*X[2]*X[3]/2;
  int Vsh_y = X[0]*X[2]*X[3]/2;
  int Vsh_z = X[0]*X[1]*X[3]/2;
#endif
  int Vsh_t = X[0]*X[1]*X[2]/2;

  int E[4];
  for (int i=0; i<4; i++) E[i] = X[i] + 4;

  // fat-link padding 
  param->llfat_ga_pad = Vsh_t;

  // site-link padding
  if(method ==  QUDA_COMPUTE_FAT_STANDARD) {
#ifdef MULTI_GPU
    int Vh_2d_max = MAX(X[0]*X[1]/2, X[0]*X[2]/2);
    Vh_2d_max = MAX(Vh_2d_max, X[0]*X[3]/2);
    Vh_2d_max = MAX(Vh_2d_max, X[1]*X[2]/2);
    Vh_2d_max = MAX(Vh_2d_max, X[1]*X[3]/2);
    Vh_2d_max = MAX(Vh_2d_max, X[2]*X[3]/2);
    param->site_ga_pad = 3*(Vsh_x+Vsh_y+Vsh_z+Vsh_t) + 4*Vh_2d_max;
#else
    param->site_ga_pad = Vsh_t;
#endif
  } else {
    param->site_ga_pad = (E[0]*E[1]*E[2]/2)*3;
  }
  param->ga_pad = param->site_ga_pad;

 // staple padding
  if(method == QUDA_COMPUTE_FAT_STANDARD) {
#ifdef MULTI_GPU
    param->staple_pad = 3*(Vsh_x + Vsh_y + Vsh_z+ Vsh_t);
#else
    param->staple_pad = 3*Vsh_t;
#endif
  } else {
    param->staple_pad = (E[0]*E[1]*E[2]/2)*3;
  }

  return;
}


namespace quda {
  void computeFatLinkCore(cudaGaugeField* cudaSiteLink, double* act_path_coeff,
			  QudaGaugeParam* qudaGaugeParam, QudaComputeFatMethod method,
			  cudaGaugeField* cudaFatLink, struct timeval time_array[])
  {
    gettimeofday(&time_array[0], NULL);
    
    const int flag = qudaGaugeParam->preserve_gauge;
    GaugeFieldParam gParam(0,*qudaGaugeParam);
    
    if(method == QUDA_COMPUTE_FAT_STANDARD){
      for(int dir=0; dir<4; ++dir) gParam.x[dir] = qudaGaugeParam->X[dir];
    }else{
      for(int dir=0; dir<4; ++dir) gParam.x[dir] = qudaGaugeParam->X[dir] + 4;
    }
    
    
    static cudaGaugeField* cudaStapleField=NULL, *cudaStapleField1=NULL;
    if(cudaStapleField == NULL || cudaStapleField1 == NULL){
      gParam.pad    = qudaGaugeParam->staple_pad;
      gParam.create = QUDA_NULL_FIELD_CREATE;
      gParam.reconstruct = QUDA_RECONSTRUCT_NO;
      gParam.geometry = QUDA_SCALAR_GEOMETRY; // only require a scalar matrix field for the staple
      cudaStapleField  = new cudaGaugeField(gParam);
      cudaStapleField1 = new cudaGaugeField(gParam);
    }
    
    gettimeofday(&time_array[1], NULL);
    
    if(method == QUDA_COMPUTE_FAT_STANDARD){
      llfat_cuda(*cudaFatLink, *cudaSiteLink, *cudaStapleField, *cudaStapleField1, qudaGaugeParam, act_path_coeff);
    }else{ //method == QUDA_COMPUTE_FAT_EXTENDED_VOLUME
      llfat_cuda_ex(*cudaFatLink, *cudaSiteLink, *cudaStapleField, *cudaStapleField1, qudaGaugeParam, act_path_coeff);
    }
    gettimeofday(&time_array[2], NULL);
    
    if (!(flag & QUDA_FAT_PRESERVE_GPU_GAUGE) ){
      delete cudaStapleField; cudaStapleField = NULL;
      delete cudaStapleField1; cudaStapleField1 = NULL;
    }
    gettimeofday(&time_array[3], NULL);
    
  return;
  
  }
} // namespace quda



int
computeFatLinkQuda(void* fatlink, void** sitelink, double* act_path_coeff, 
		   QudaGaugeParam* qudaGaugeParam, 
		   QudaComputeFatMethod method)
{
#define TDIFF_MS(a,b) (b.tv_sec - a.tv_sec + 0.000001*(b.tv_usec - a.tv_usec))*1000

  struct timeval t0;
  struct timeval t7, t8, t9, t10, t11;

  gettimeofday(&t0, NULL);

  static cpuGaugeField* cpuFatLink=NULL, *cpuSiteLink=NULL;
  static cudaGaugeField* cudaFatLink=NULL, *cudaSiteLink=NULL;
  int flag = qudaGaugeParam->preserve_gauge;

  QudaGaugeParam qudaGaugeParam_ex_buf;
  QudaGaugeParam* qudaGaugeParam_ex = &qudaGaugeParam_ex_buf;
  memcpy(qudaGaugeParam_ex, qudaGaugeParam, sizeof(QudaGaugeParam));

  qudaGaugeParam_ex->X[0] = qudaGaugeParam->X[0]+4;
  qudaGaugeParam_ex->X[1] = qudaGaugeParam->X[1]+4;
  qudaGaugeParam_ex->X[2] = qudaGaugeParam->X[2]+4;
  qudaGaugeParam_ex->X[3] = qudaGaugeParam->X[3]+4;

  GaugeFieldParam gParam_ex(0, *qudaGaugeParam_ex);
  
  // fat-link padding
  setFatLinkPadding(method, qudaGaugeParam);
  qudaGaugeParam_ex->llfat_ga_pad = qudaGaugeParam->llfat_ga_pad;
  qudaGaugeParam_ex->staple_pad   = qudaGaugeParam->staple_pad;
  qudaGaugeParam_ex->site_ga_pad  = qudaGaugeParam->site_ga_pad;
  
  GaugeFieldParam gParam(0, *qudaGaugeParam);

  // create the host fatlink
  if(cpuFatLink == NULL){
    gParam.create = QUDA_REFERENCE_FIELD_CREATE;
    gParam.link_type = QUDA_ASQTAD_FAT_LINKS;
    gParam.order = QUDA_MILC_GAUGE_ORDER;
    gParam.gauge= fatlink;
    cpuFatLink = new cpuGaugeField(gParam);
    if(cpuFatLink == NULL){
      errorQuda("ERROR: Creating cpuFatLink failed\n");
    }
  }else{
    cpuFatLink->setGauge((void**)fatlink);
  }
  
 // create the device fatlink
  if(cudaFatLink == NULL){
    gParam.pad    = qudaGaugeParam->llfat_ga_pad;
    gParam.create = QUDA_ZERO_FIELD_CREATE;
    gParam.link_type = QUDA_ASQTAD_FAT_LINKS;
    gParam.order = QUDA_QDP_GAUGE_ORDER;
    gParam.reconstruct = QUDA_RECONSTRUCT_NO;
    cudaFatLink = new cudaGaugeField(gParam);
  }



  // create the host sitelink	
  if(cpuSiteLink == NULL){
    gParam.pad = 0; 
    gParam.create    = QUDA_REFERENCE_FIELD_CREATE;
    gParam.link_type = qudaGaugeParam->type;
    gParam.order     = qudaGaugeParam->gauge_order;
    gParam.gauge     = sitelink;
    if(method != QUDA_COMPUTE_FAT_STANDARD){
      for(int dir=0; dir<4; ++dir) gParam.x[dir] = qudaGaugeParam_ex->X[dir];	
    }
    cpuSiteLink      = new cpuGaugeField(gParam);
    if(cpuSiteLink == NULL){
      errorQuda("ERROR: Creating cpuSiteLink failed\n");
    }
  }else{
    cpuSiteLink->setGauge(sitelink);
  }
  
  if(cudaSiteLink == NULL){
    gParam.pad         = qudaGaugeParam->site_ga_pad;
    gParam.create      = QUDA_NULL_FIELD_CREATE;
    gParam.link_type   = qudaGaugeParam->type;
    gParam.reconstruct = qudaGaugeParam->reconstruct;      
    cudaSiteLink = new cudaGaugeField(gParam);
  }
  
  initLatticeConstants(*cudaFatLink);  
  
  if(method == QUDA_COMPUTE_FAT_STANDARD){
    llfat_init_cuda(qudaGaugeParam);
    gettimeofday(&t7, NULL);
    
#ifdef MULTI_GPU
    if(qudaGaugeParam->gauge_order == QUDA_MILC_GAUGE_ORDER){
      errorQuda("Only QDP-ordered site links are supported in the multi-gpu standard fattening code\n");
    }
#endif
    loadLinkToGPU(cudaSiteLink, cpuSiteLink, qudaGaugeParam);
    
    gettimeofday(&t8, NULL);
  }else{
    llfat_init_cuda_ex(qudaGaugeParam_ex);
#ifdef MULTI_GPU
    int R[4] = {2, 2, 2, 2}; // radius of the extended region in each dimension / direction
    exchange_cpu_sitelink_ex(qudaGaugeParam->X, R, (void**)cpuSiteLink->Gauge_p(), 
			     cpuSiteLink->Order(),qudaGaugeParam->cpu_prec, 0);
#endif
    gettimeofday(&t7, NULL);
    loadLinkToGPU_ex(cudaSiteLink, cpuSiteLink);
    gettimeofday(&t8, NULL);
  }

  // time the subroutines in computeFatLinkCore
  struct timeval time_array[4];
  
  // Actually do the fattening
  computeFatLinkCore(cudaSiteLink, act_path_coeff, qudaGaugeParam, method, cudaFatLink, time_array);
 

  gettimeofday(&t9, NULL);

  storeLinkToCPU(cpuFatLink, cudaFatLink, qudaGaugeParam);
  
  gettimeofday(&t10, NULL);
  
  if (!(flag & QUDA_FAT_PRESERVE_CPU_GAUGE) ){
    delete cpuFatLink; cpuFatLink = NULL;
    delete cpuSiteLink; cpuSiteLink = NULL;
  }  
  if (!(flag & QUDA_FAT_PRESERVE_GPU_GAUGE) ){
    delete cudaFatLink; cudaFatLink = NULL;
    delete cudaSiteLink; cudaSiteLink = NULL;
  }
  
  gettimeofday(&t11, NULL);
#ifdef DSLASH_PROFILING 
  printfQuda("total time: %f ms, init(cuda/cpu gauge field creation,etc)=%f ms,"
	     " sitelink cpu->gpu=%f ms, computation in gpu =%f ms, fatlink gpu->cpu=%f ms\n",
	     TDIFF_MS(t0, t11), TDIFF_MS(t0, t7) + TDIFF_MS(time_array[0],time_array[1]), TDIFF_MS(t7, t8), TDIFF_MS(time_array[1], time_array[2]), TDIFF_MS(t9,t10));
  printfQuda("finally cleanup =%f ms\n", TDIFF_MS(t10, t11) + TDIFF_MS(time_array[2],time_array[3]));
#endif

  return 0;
}
#endif

#ifdef GPU_GAUGE_FORCE
int
computeGaugeForceQuda(void* mom, void* sitelink,  int*** input_path_buf, int* path_length,
                      void* loop_coeff, int num_paths, int max_length, double eb3,
                      QudaGaugeParam* qudaGaugeParam, double* timeinfo)
{
  struct timeval t0, t1, t2, t3;
  
  gettimeofday(&t0,NULL);

#ifdef MULTI_GPU
  int E[4];
  QudaGaugeParam qudaGaugeParam_ex_buf;
  QudaGaugeParam* qudaGaugeParam_ex=&qudaGaugeParam_ex_buf;
  memcpy(qudaGaugeParam_ex, qudaGaugeParam, sizeof(QudaGaugeParam));
  E[0] = qudaGaugeParam_ex->X[0] = qudaGaugeParam->X[0] + 4;
  E[1] = qudaGaugeParam_ex->X[1] = qudaGaugeParam->X[1] + 4;
  E[2] = qudaGaugeParam_ex->X[2] = qudaGaugeParam->X[2] + 4;
  E[3] = qudaGaugeParam_ex->X[3] = qudaGaugeParam->X[3] + 4;
#endif

  int* X = qudaGaugeParam->X;
  GaugeFieldParam gParam(0, *qudaGaugeParam);
#ifdef MULTI_GPU
  GaugeFieldParam gParam_ex(0, *qudaGaugeParam_ex);
  GaugeFieldParam& gParamSL = gParam_ex;  
  int pad = E[2]*E[1]*E[0]/2;
#else
  GaugeFieldParam& gParamSL = gParam;
  int pad = X[2]*X[1]*X[0]/2;
#endif
  
  GaugeFieldParam& gParamMom = gParam;
  
  gParamSL.create = QUDA_REFERENCE_FIELD_CREATE;
  gParamSL.gauge = sitelink;
  gParamSL.pad = 0;
  cpuGaugeField* cpuSiteLink = new cpuGaugeField(gParamSL);
  
  gParamSL.create =QUDA_NULL_FIELD_CREATE;
  gParamSL.pad = pad;
  gParamSL.precision = qudaGaugeParam->cuda_prec;
  gParamSL.reconstruct = qudaGaugeParam->reconstruct;
  cudaGaugeField* cudaSiteLink = new cudaGaugeField(gParamSL);  
  qudaGaugeParam->site_ga_pad = gParamSL.pad;//need to record this value

  gParamMom.pad = 0;
  gParamMom.order =QUDA_MILC_GAUGE_ORDER;
  gParamMom.precision = qudaGaugeParam->cpu_prec;
  gParamMom.create =QUDA_REFERENCE_FIELD_CREATE;
  gParamMom.reconstruct =QUDA_RECONSTRUCT_10;  
  gParamMom.gauge=mom;
  gParamMom.link_type = QUDA_ASQTAD_MOM_LINKS;
  cpuGaugeField* cpuMom = new cpuGaugeField(gParamMom);              


  gParamMom.pad = pad;
  gParamMom.create =QUDA_NULL_FIELD_CREATE;  
  gParamMom.reconstruct = QUDA_RECONSTRUCT_10;
  gParamMom.precision = qudaGaugeParam->cuda_prec;
  gParamMom.link_type = QUDA_ASQTAD_MOM_LINKS;
  cudaGaugeField* cudaMom = new cudaGaugeField(gParamMom);
  qudaGaugeParam->mom_ga_pad = gParamMom.pad; //need to record this value
  
  initLatticeConstants(*cudaMom);
  checkCudaError();
  gauge_force_init_cuda(qudaGaugeParam, max_length); 

#ifdef MULTI_GPU
  int R[4] = {2, 2, 2, 2}; // radius of the extended region in each dimension / direction
  exchange_cpu_sitelink_ex(qudaGaugeParam->X, R, (void**)cpuSiteLink->Gauge_p(), 
			   cpuSiteLink->Order(), qudaGaugeParam->cpu_prec, 1);
  loadLinkToGPU_ex(cudaSiteLink, cpuSiteLink);
#else  
  loadLinkToGPU(cudaSiteLink, cpuSiteLink, qudaGaugeParam);    
#endif

  cudaMom->loadCPUField(*cpuMom, QUDA_CPU_FIELD_LOCATION);

  gettimeofday(&t1,NULL);  

  gauge_force_cuda(*cudaMom, eb3, *cudaSiteLink, qudaGaugeParam, input_path_buf, 
		   path_length, loop_coeff, num_paths, max_length);

  gettimeofday(&t2,NULL);

  cudaMom->saveCPUField(*cpuMom, QUDA_CPU_FIELD_LOCATION);
  
  delete cpuSiteLink;
  delete cpuMom;
  
  delete cudaSiteLink;
  delete cudaMom;
  
  gettimeofday(&t3,NULL); 

  if(timeinfo){
    timeinfo[0] = TDIFF(t0, t1);
    timeinfo[1] = TDIFF(t1, t2);
    timeinfo[2] = TDIFF(t2, t3);
  }

  checkCudaError();
  return 0;  
}

#endif


void initCommsQuda(int argc, char **argv, const int *X, int nDim) {
#ifdef MULTI_GPU
  comm_create(argc, argv);
  comm_set_gridsize(X, nDim);  
  comm_init();
#endif
}

void endCommsQuda() {
#ifdef MULTI_GPU
  comm_cleanup();
#endif
}

/*
  The following functions are for the Fortran interface.
*/

void init_quda_(int *dev) { initQuda(*dev); }
void end_quda_() { endQuda(); }
void load_gauge_quda_(void *h_gauge, QudaGaugeParam *param) { loadGaugeQuda(h_gauge, param); }
void free_gauge_quda_() { freeGaugeQuda(); }
void load_clover_quda_(void *h_clover, void *h_clovinv, QudaInvertParam *inv_param) 
{ loadCloverQuda(h_clover, h_clovinv, inv_param); }
void free_clover_quda_(void) { freeCloverQuda(); }
void dslash_quda_(void *h_out, void *h_in, QudaInvertParam *inv_param,
		  QudaParity *parity) { dslashQuda(h_out, h_in, inv_param, *parity); }
void clover_quda_(void *h_out, void *h_in, QudaInvertParam *inv_param,
		  QudaParity *parity, int *inverse) { cloverQuda(h_out, h_in, inv_param, *parity, *inverse); }
void mat_quda_(void *h_out, void *h_in, QudaInvertParam *inv_param)
{ MatQuda(h_out, h_in, inv_param); }
void mat_dag_mat_quda_(void *h_out, void *h_in, QudaInvertParam *inv_param)
{ MatDagMatQuda(h_out, h_in, inv_param); }
void invert_quda_(void *hp_x, void *hp_b, QudaInvertParam *param) 
{ invertQuda(hp_x, hp_b, param); }    
void new_quda_gauge_param_(QudaGaugeParam *param) {
  *param = newQudaGaugeParam();
}
void new_quda_invert_param_(QudaInvertParam *param) {
  *param = newQudaInvertParam();
}
void comm_set_gridsize_(int *grid) {
#ifdef MULTI_GPU
  comm_set_gridsize(grid, 4);
#endif
}
