/*
 * Copyright (C) 2004-2010 NXP Software
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/****************************************************************************************/
/*                                                                                      */
/*    Includes                                                                          */
/*                                                                                      */
/****************************************************************************************/
#include <audio_utils/BiquadFilter.h>

#include <string.h>  // memset
#include "LVDBE.h"
#include "LVDBE_Private.h"
#include "VectorArithmetic.h"
#include "AGC.h"
#include "LVDBE_Coeffs.h" /* Filter coefficients */
#include <log/log.h>

/********************************************************************************************/
/*                                                                                          */
/* FUNCTION:                 LVDBE_Process                                                  */
/*                                                                                          */
/* DESCRIPTION:                                                                             */
/*  Process function for the Bass Enhancement module.                                       */
/*                                                                                          */
/*  Data can be processed in two formats, stereo or mono-in-stereo. Data in mono            */
/*  format is not supported, the calling routine must convert the mono stream to            */
/*  mono-in-stereo.                                                                         */
/*                                                        ___________                       */
/*       ________                                        |           |    ________          */
/*      |        |    _____   |------------------------->|           |   |        |         */
/*      | 16-bit |   |     |  |    ________              |           |   | 32-bit |         */
/* -+-->|   to   |-->| HPF |--|   |        |    _____    | AGC Mixer |-->|   to   |--|      */
/*  |   | 32-bit |   |_____|  |   | Stereo |   |     |   |           |   | 16-bit |  |      */
/*  |   |________|            |-->|   to   |-->| BPF |-->|           |   |________|  0      */
/*  |                             |  Mono  |   |_____|   |___________|                \-->  */
/*  |                             |________|                                                */
/*  |                                                     _________                  0      */
/*  |                                                    |         |                 |      */
/*  |----------------------------------------------------| Volume  |-----------------|      */
/*                                                       | Control |                        */
/*                                                       |_________|                        */
/*                                                                                          */
/* PARAMETERS:                                                                              */
/*  hInstance                 Instance handle                                               */
/*  pInData                  Pointer to the input data                                      */
/*  pOutData                 Pointer to the output data                                     */
/*  NumSamples                 Number of samples in the input buffer                        */
/*                                                                                          */
/* RETURNS:                                                                                 */
/*  LVDBE_SUCCESS            Succeeded                                                      */
/*    LVDBE_TOOMANYSAMPLES    NumSamples was larger than the maximum block size             */
/*                                                                                          */
/* NOTES:                                                                                   */
/*  1. The input and output data must be 32-bit format. The input is scaled by a shift      */
/*     when converting from 16-bit format, this scaling allows for internal headroom in the */
/*     bass enhancement algorithm.                                                          */
/*  2. For a 16-bit implementation the converstion to 32-bit is removed and replaced with   */
/*     the headroom loss. This headroom loss is compensated in the volume control so the    */
/*     overall end to end gain is odB.                                                      */
/*                                                                                          */
/********************************************************************************************/
LVDBE_ReturnStatus_en LVDBE_Process(
        LVDBE_Handle_t hInstance, const LVM_FLOAT* pInData, LVM_FLOAT* pOutData,
        const LVM_UINT16 NrFrames)  // updated to use samples = frames * channels.
{
    LVDBE_Instance_t* pInstance = (LVDBE_Instance_t*)hInstance;
    const LVM_INT32 NrChannels = pInstance->Params.NrChannels;
    const LVM_INT32 NrSamples = NrChannels * NrFrames;

    /* Space to store DBE path computation */
    LVM_FLOAT* const pScratch = (LVM_FLOAT*)pInstance->pScratch;

    /*
     * Scratch for Mono path starts at offset of
     * NrSamples float values from pScratch.
     */
    LVM_FLOAT* const pMono = pScratch + NrSamples;

    /*
     * TRICKY: pMono is used and discarded by the DBE path.
     *         so it is available for use for the pScratchVol
     *         path which is computed afterwards.
     *
     * Space to store Volume Control path computation.
     * This is identical to pMono (see TRICKY comment).
     */
    LVM_FLOAT* const pScratchVol = pMono;

    /*
     * Check the number of frames is not too large
     */
    if (NrFrames > pInstance->Capabilities.MaxBlockSize) {
        return LVDBE_TOOMANYSAMPLES;
    }

    /*
     * Check if the algorithm is enabled
     */
    /* DBE path is processed when DBE is ON or during On/Off transitions */
    if ((pInstance->Params.OperatingMode == LVDBE_ON) ||
        (LVC_Mixer_GetCurrent(&pInstance->pData->BypassMixer.MixerStream[0]) !=
         LVC_Mixer_GetTarget(&pInstance->pData->BypassMixer.MixerStream[0]))) {
        // make copy of input data
        Copy_Float(pInData, pScratch, (LVM_INT16)NrSamples);

        /*
         * Apply the high pass filter if selected
         */
        if (pInstance->Params.HPFSelect == LVDBE_HPF_ON) {
            pInstance->pHPFBiquad->process(pScratch, pScratch, NrFrames);
        }

        /*
         * Create the mono stream
         */
        FromMcToMono_Float(pScratch,            /* Source */
                           pMono,               /* Mono destination */
                           (LVM_INT16)NrFrames, /* Number of frames */
                           (LVM_INT16)NrChannels);

        /*
         * Apply the band pass filter
         */
        pInstance->pBPFBiquad->process(pMono, pMono, NrFrames);

        /*
         * Apply the AGC and mix
         */
        AGC_MIX_VOL_Mc1Mon_D32_WRA(&pInstance->pData->AGCInstance, /* Instance pointer      */
                                   pScratch,                       /* Source         */
                                   pMono,                          /* Mono band pass source */
                                   pScratch,                       /* Destination    */
                                   NrFrames,                       /* Number of frames     */
                                   NrChannels);                    /* Number of channels     */
    } else {
        // clear DBE processed path
        memset(pScratch, 0, sizeof(*pScratch) * NrSamples);
    }

    /* Bypass Volume path is processed when DBE is OFF or during On/Off transitions */
    if ((pInstance->Params.OperatingMode == LVDBE_OFF) ||
        (LVC_Mixer_GetCurrent(&pInstance->pData->BypassMixer.MixerStream[1]) !=
         LVC_Mixer_GetTarget(&pInstance->pData->BypassMixer.MixerStream[1]))) {
        /*
         * The algorithm is disabled but volume management is required to compensate for
         * headroom and volume (if enabled)
         */
        LVC_MixSoft_Mc_D16C31_SAT(&pInstance->pData->BypassVolume, pInData, pScratchVol,
                                  (LVM_INT16)NrFrames, (LVM_INT16)NrChannels);
    } else {
        // clear bypass volume path
        memset(pScratchVol, 0, sizeof(*pScratchVol) * NrSamples);
    }

    /*
     * Mix DBE processed path and bypass volume path
     */
    LVC_MixSoft_2Mc_D16C31_SAT(&pInstance->pData->BypassMixer, pScratch, pScratchVol, pOutData,
                               (LVM_INT16)NrFrames, (LVM_INT16)NrChannels);
    return LVDBE_SUCCESS;
}
