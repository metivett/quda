#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <complex>

#include <quda_internal.h>
#include <blas_quda.h>
#include <dslash_quda.h>
#include <invert_quda.h>
#include <util_quda.h>

#include<face_quda.h>

#include <color_spinor_field.h>

cudaColorSpinorField *rp = 0;
cudaColorSpinorField *Arp = 0;
cudaColorSpinorField *tmpp = 0;

bool initMR = false;

void freeMR() {
  if (initMR) {
    if (rp) delete rp;
    delete Arp;
    delete tmpp;

    initMR = false;
  }
}

void invertMRCuda(const DiracMatrix &mat, cudaColorSpinorField &x, cudaColorSpinorField &b, 
		  QudaInvertParam *invert_param)
{
  typedef std::complex<double> Complex;

  if (!initMR) {
    ColorSpinorParam param(x);
    param.create = QUDA_ZERO_FIELD_CREATE;
    if (invert_param->preserve_source) rp = new cudaColorSpinorField(x, param); 
    Arp = new cudaColorSpinorField(x);
    tmpp = new cudaColorSpinorField(x, param); //temporary for mat-vec

    initMR = true;
  }
  cudaColorSpinorField &r = (invert_param->preserve_source ? *Arp : b);
  cudaColorSpinorField &Ar = *Arp;
  cudaColorSpinorField &tmp = *tmpp;

  double b2 = normCuda(b);
  double stop = b2*invert_param->tol*invert_param->tol; // stopping condition of solver

  // calculate initial residual
  mat(r, x, tmp);
  double r2 = xmyNormCuda(b, r);  

  if (invert_param->inv_type_sloppy == QUDA_GCR_INVERTER) {
    blas_quda_flops = 0;
    stopwatchStart();
  }

  int k = 0;
  if (invert_param->verbosity >= QUDA_VERBOSE) printfQuda("MR: %d iterations, r2 = %e\n", k, r2);

  while (r2 > stop && k < invert_param->maxiter) {
    
    mat(Ar, r, tmp);
    
    double3 Ar3 = cDotProductNormACuda(Ar, r);
    Complex alpha = Complex(Ar3.x, Ar3.y) / Ar3.z;

    // fuse the following 3 kernels (7 -> 5 i/o transactions)
    caxpyCuda(omega*alpha, r, x);
    caxpyCuda(-omega*alpha, Ar, r);
    r2 = norm2(r);

    k++;

    if (invert_param->verbosity >= QUDA_VERBOSE) printfQuda("MR: %d iterations, r2 = %e\n", k, r2);
  }
  
  if (k>=invert_param->maxiter) warningQuda("Exceeded maximum iterations %d", invert_param->maxiter);
  
  if (invert_param->inv_type_sloppy == QUDA_GCR_INVERTER) {
    invert_param->secs += stopwatchReadSeconds();
  
    double gflops = (blas_quda_flops + mat.flops() + matSloppy.flops())*1e-9;
    reduceDouble(gflops);

    //  printfQuda("%f gflops\n", gflops / stopwatchReadSeconds());
    invert_param->gflops += gflops;
    invert_param->iter += total_iter;
    
    if (invert_param->verbosity >= QUDA_SUMMARIZE) {
      // Calculate the true residual
      mat(r, x);
      double true_res = xmyNormCuda(b, r);
      
      printfQuda("MR: Converged after %d iterations, relative residua: iterated = %e, true = %e\n", 
		 total_iter, sqrt(r2/b2), sqrt(true_res / b2));    
    }
  }

  return;
}