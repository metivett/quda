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

// set the required parameters for the inner solver
void fillInnerInvertParam(QudaInvertParam &inner, const QudaInvertParam &outer) {
  inner.tol = outer.tol_sloppy;
  inner.maxiter = outer.maxiter_sloppy;
  inner.reliable_delta = 1e-20; // no reliable updates within the inner solver
  
  inner.cuda_prec = outer.cuda_prec_sloppy; // only use sloppy precision on inner solver
  inner.cuda_prec_sloppy = outer.cuda_prec_sloppy;
  
  inner.verbosity = outer.verbosity_sloppy;
  
  inner.iter = 0;
  inner.gflops = 0;
  inner.secs = 0;

  inner.inv_type_sloppy = QUDA_GCR_INVERTER; // used to tell the inner solver it is an inner solver
}

void invertGCRCuda(const DiracMatrix &mat, const DiracMatrix &matSloppy, cudaColorSpinorField &x, 
		   cudaColorSpinorField &b, QudaInvertParam *invert_param)
{
  typedef std::complex<double> Complex;

  int Nkrylov = invert_param->gcrNkrylov; // size of Krylov space

  ColorSpinorParam param(x);
  param.create = QUDA_ZERO_FIELD_CREATE;
  cudaColorSpinorField r(x, param); 

  cudaColorSpinorField *p[Nkrylov], *Ap[Nkrylov];
  for (int i=0; i<Nkrylov; i++) {
    p[i] = new cudaColorSpinorField(x, param);
    Ap[i] = new cudaColorSpinorField(x, param);
  }

  cudaColorSpinorField tmp(x, param); //temporary for mat-vec

  // these low precision fields are used by the inner solver
  param.precision = invert_param->cuda_prec_sloppy;
  cudaColorSpinorField rSloppy(x, param);
  cudaColorSpinorField pSloppy(x, param);

  QudaInvertParam invert_param_inner = newQudaInvertParam();
  fillInnerInvertParam(invert_param_inner, *invert_param);

  Complex alpha = 0.0;
  Complex *beta = new Complex[Nkrylov];

  double b2 = normCuda(b);

  double stop = b2*invert_param->tol*invert_param->tol; // stopping condition of solver

  int k = 0;

  // calculate initial residual
  mat(r, x, tmp);
  double r2 = xmyNormCuda(b, r);  

  blas_quda_flops = 0;

  stopwatchStart();

  int total_iter = 0;
  int restart = 0;
  double r2_old = r2;

  if (invert_param->verbosity >= QUDA_VERBOSE) 
      printfQuda("GCR: %d total iterations, %d Krylov iterations, r2 = %e\n", total_iter+k, k, r2);

  while (r2 > stop && total_iter < invert_param->maxiter) {
    
    if (invert_param->inv_type_sloppy != QUDA_INVALID_INVERTER) {
      if (invert_param->tol/(sqrt(r2/b2)) > invert_param->tol_sloppy) 
	invert_param_inner.tol = invert_param->tol/sqrt(r2/b2);
      copyCuda(rSloppy, r);
      if (invert_param->inv_type_sloppy == QUDA_CG_INVERTER) // inner CG preconditioner
	invertCgCuda(matSloppy, matSloppy, pSloppy, rSloppy, &invert_param_inner);
      else // inner BiCGstab preconditioner
	invertBiCGstabCuda(matSloppy, matSloppy, pSloppy, rSloppy, &invert_param_inner);
      copyCuda(*p[k], pSloppy);
    } else { // no preconditioner
      *p[k] = r;
    } 

    mat(*Ap[k], *p[k], tmp);
    
    for (int i=0; i<k; i++) {
      beta[i] = cDotProductCuda(*Ap[i], *Ap[k]); // partial fusion here with previous iter
      caxpyCuda(-beta[i], *p[i], *p[k]);
      caxpyCuda(-beta[i], *Ap[i], *Ap[k]);
    }

    double scale = sqrt(norm2(*Ap[k]));
    if (scale == 0.0) errorQuda("GCR breakdown\n");

    axCuda(1.0/scale, *p[k]); // kernel fusion here with proceeding caxpy calls
    axCuda(1.0/scale, *Ap[k]);

    alpha = cDotProductCuda(*Ap[k], r);

    caxpyCuda(alpha, *p[k], x);
    caxpyCuda(-alpha, *Ap[k], r);

    r2 = norm2(r);

    if (invert_param->verbosity >= QUDA_DEBUG_VERBOSE) 
      printfQuda("GCR: alpha = (%e,%e), x2 = %e\n", real(alpha), imag(alpha), norm2(x));

    k++;
    total_iter++;

    if (invert_param->verbosity >= QUDA_VERBOSE) 
      printfQuda("GCR: %d total iterations, %d Krylov iterations, r2 = %e\n", total_iter, k, r2);

    if (k==Nkrylov) { // restart the solver since max Nkrylov reached
      restart++;

      if (invert_param->verbosity >= QUDA_VERBOSE) printfQuda("\nGCR: restart %d, r2 = %e\n", restart, r2);

      k = 0;
      mat(r, x, tmp);
      double r2 = xmyNormCuda(b, r);  

      if (r2_old < r2) {
	if (invert_param->verbosity >= QUDA_VERBOSE) 
	  printfQuda("GCR: precision limit reached, r2_old = %e < r2 = %e\n", r2_old, r2);
	break;
      }

      r2_old = r2;
    }
  }
  
  if (k>=invert_param->maxiter) warningQuda("Exceeded maximum iterations %d", invert_param->maxiter);

  if (invert_param->verbosity >= QUDA_VERBOSE) printfQuda("GCR: number of restarts = %d\n", restart);
  
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
    
    printfQuda("GCR: Converged after %d iterations, relative residua: iterated = %e, true = %e\n", 
	       total_iter, sqrt(r2/b2), sqrt(true_res / b2));    
  }

  for (int i=0; i<Nkrylov; i++) {
    delete p[i];
    delete Ap[i];
  }

  delete []beta;

  return;
}