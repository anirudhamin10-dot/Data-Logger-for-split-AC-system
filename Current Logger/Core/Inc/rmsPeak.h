/*
 * Academic License - for use in teaching, academic research, and meeting
 * course requirements at degree granting institutions only.  Not for
 * government, commercial, or other organizational use.
 * File: rmsPeak.h
 *
 * MATLAB Coder version            : 26.2
 * C/C++ source code generated on  : 22-Oct-2026 00:31:42
 */

#ifndef RMSPEAK_H
#define RMSPEAK_H

/* Include Files */
#include "rtwtypes.h"
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Function Declarations */
extern void rmsPeak(const unsigned short buffer[200], unsigned short bias,
                    unsigned short vdd, unsigned int *rms, unsigned int *peak);

#ifdef __cplusplus
}
#endif

#endif
/*
 * File trailer for rmsPeak.h
 *
 * [EOF]
 */
