/*
 * Academic License - for use in teaching, academic research, and meeting
 * course requirements at degree granting institutions only.  Not for
 * government, commercial, or other organizational use.
 * File: rmsPeak.c
 *
 * MATLAB Coder version            : 26.2
 * C/C++ source code generated on  : 22-Oct-2026 00:31:42
 */

/* Include Files */
#include "rmsPeak.h"
#include <math.h>

/* Function Definitions */
/*
 * Arguments    : const unsigned short buffer[200]
 *                unsigned short bias
 *                unsigned short vdd
 *                unsigned int *rms
 *                unsigned int *peak
 * Return Type  : void
 */
void rmsPeak(const unsigned short buffer[200], unsigned short bias,
             unsigned short vdd, unsigned int *rms, unsigned int *peak)
{
  int idx;
  unsigned int sumsq;
  sumsq = 0U;
  *peak = 0U;
  for (idx = 0; idx < ((short)200); idx++) {
    int x;
    x = ((int)buffer[idx]) - ((int)bias);
    if (x < ((signed char)0)) {
      x = -x;
    }
    if (((unsigned int)x) > (*peak)) {
      *peak = (unsigned int)x;
    }
    sumsq += ((unsigned int)x) * ((unsigned int)x);
  }
  float k;
  k = ((float)vdd) * 0.0148F;
  *rms = (unsigned int)roundf(sqrtf(((float)sumsq) * 0.005F) * k);
  *peak = (unsigned int)roundf(((float)(*peak)) * k);
}

/*
 * File trailer for rmsPeak.c
 *
 * [EOF]
 */
