/******************************************************************************
*
* Copyright (C) 2017 Allegro DVT2.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX OR ALLEGRO DVT2 BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of  Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
*
* Except as contained in this notice, the name of Allegro DVT2 shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Allegro DVT2.
*
******************************************************************************/

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_encode
   @{
   \file
 *****************************************************************************/

#pragma once

#include <assert.h>
#include <stdlib.h>

#include "lib_preprocess/ChooseLda.h"

#include "lib_rtos/lib_rtos.h"
#include "lib_common/SEI.h"
#include "lib_common_enc/EncBuffersInternal.h"

#include "Encoder.h"
#include "IP_EncoderCtx.h"

#include "IP_Utils.h"

#include "lib_encode/IScheduler.h"

void AL_Common_Encoder_InitCtx(AL_TEncCtx* pCtx, AL_TEncChanParam* pChParam, AL_TAllocator* pAllocator);

AL_ERR AL_Common_Encoder_CreateChannel(AL_TEncCtx* pCtx, TScheduler* pScheduler, AL_TAllocator* pAlloc, AL_TEncSettings const* pSettings);

/*************************************************************************//*!
   \brief The Encoder_NotifySceneChange function informs the encoder that a scene
   change will shortly happen.
   \param[in] pEnc Handle to an encoder object
   \param[in] iAhead Number of frame until the scene change will happen.
   Allowed range is [0..31]
*****************************************************************************/
void AL_Common_Encoder_NotifySceneChange(AL_TEncoder* pEnc, int iAhead);

/*************************************************************************//*!
   \brief The Encoder_NotifyLongTerm function informs the encoder that a long
   term reference picture will be used
   \param[in] pEnc Handle to an encoder object
*****************************************************************************/
void AL_Common_Encoder_NotifyLongTerm(AL_TEncoder* pEnc);


/*************************************************************************//*!
   \brief The Encoder_PutStreamBuffer function push a stream buffer to be filled
   \param[in] pEnc Handle to an encoder object
   \param[in] pStream The stream buffer to be filled
*****************************************************************************/
bool AL_Common_Encoder_PutStreamBuffer(AL_TEncoder* pEnc, AL_TBuffer* pStream);

/***************************************************************************/
bool AL_Common_Encoder_GetRecPicture(AL_TEncoder* pEnc, TRecPic* pRecPic);

/***************************************************************************/
void AL_Common_Encoder_ReleaseRecPicture(AL_TEncoder* pEnc, TRecPic* pRecPic);

/*************************************************************************//*!
   \brief The Encoder_Process function allows to push a frame buffer to the
   encoder. According to the GOP pattern, this frame buffer
   could or couldn't be encoded immediately.
   \param[in] pEnc Handle to an encoder object
   \param[in] pFrame Pointer to the frame buffer to encode
   \param[in] pQPTable Pointer to an optional qp table used if the external qp table mode is enabled
   \warning The tMD member of each TBufferYuv struct pointed to by pFrame
   shall not be altered.
   \return If the function succeeds the return value is nonzero (true)
   If the function fails the return value is zero (false)
   \see AL_Encoder_PutStreamBuffer
*****************************************************************************/
bool AL_Common_Encoder_Process(AL_TEncoder* pEnc, AL_TBuffer* pFrame, AL_TBuffer* pQPTable);

/*************************************************************************//*!
   \brief The Encoder_GetLastError function return the last error if any
   \param[in] pEnc Pointer on an encoder context object
   \return if an error occurs the function returns the corresponding error code,
   otherwise, the function returns AL_SUCCESS.
*****************************************************************************/
AL_ERR AL_Common_Encoder_GetLastError(AL_TEncoder* pEnc);

/*************************************************************************//*!
   \brief The Encoder_WaitReadiness function wait until the encoder context object is available
   \param[in] pCtx Pointer on an encoder context object
   \return If the function succeeds the return value is nonzero (true)
   If the function fails the return value is zero (false)
*****************************************************************************/
void AL_Common_Encoder_WaitReadiness(AL_TEncCtx* pCtx);

/*************************************************************************//*!
   \brief The Encoder_Destroy function destroy an encoder object
   \param[in] pEnc Pointer on an encoder object
*****************************************************************************/
void AL_Common_Encoder_Destroy(AL_TEncoder* pEnc);

/*************************************************************************//*!
   \brief The AL_Encoder_RestartGop requests the encoder to insert a Keyframe
   and restart a new Gop.
   \param[in] pEnc Pointer on an encoder object
   \return true on success, false on error : call AL_Common_Encoder_GetLastError
   to retrieve the error code
*****************************************************************************/
bool AL_Common_Encoder_RestartGop(AL_TEncoder* pEnc);

/*************************************************************************//*!
   \brief The AL_Encoder_SetGopLength changes the GopLength. If the on-going
   Gop is already longer than the new GopLength the encoder will restart a new
   Gop immediately. If the on-going GOP is shorter than the new GopLength,
   the encoder will restart the gop when the on-going one will reach the new
   GopLength
   \param[in] pEnc Pointer on an encoder object
   \param[in] iGopLength New Gop Length
   \return true on success, false on error : call AL_Common_Encoder_GetLastError
   to retrieve the error code
*****************************************************************************/
bool AL_Common_Encoder_SetGopLength(AL_TEncoder* pEnc, int iGopLength);

/*************************************************************************//*!
   \brief The AL_Encoder_SetGopNumB changes the Number of consecutive B
   frame in-between 2 I/P frames.
   \param[in] pEnc Pointer on an encoder object
   \param[in] iNumB the new number of B frames
   \return true on success, false on error : call AL_Common_Encoder_GetLastError
   to retrieve the error code
*****************************************************************************/
bool AL_Common_Encoder_SetGopNumB(AL_TEncoder* pEnc, int iNumB);

/*************************************************************************//*!
   \brief The AL_Encoder_SetBitRate changes the target bitrate
   \param[in] pEnc Pointer on an encoder object
   \param[in] iGopLength New Gop Length
   \return true on success, false on error : call AL_Common_Encoder_GetLastError
   to retrieve the error code
*****************************************************************************/
bool AL_Common_Encoder_SetBitRate(AL_TEncoder* pEnc, int iBitRate);

/*************************************************************************//*!
   \brief The AL_Encoder_SetFrameRate changes the encoding frame rate
   \param[in] pEnc Pointer on an encoder object
   \param[in] uFrameRate the new frame rate
   \param[in] uClkRatio the ClkRatio
   \return true on success, false on error : call AL_Common_Encoder_GetLastError
   to retrieve the error code
   \note Fps = uFrameRate * 1000 / uClkRatio. for example uFrameRate = 60 and
   uClkRatio = 1001 gives 59.94 fps
*****************************************************************************/
bool AL_Common_Encoder_SetFrameRate(AL_TEncoder* pEnc, uint16_t uFrameRate, uint16_t uClkRatio);



/*************************************************************************//*!
   \brief The SetME function initializes the motion estimation parameters
   \param[in]  iHrzRange_P Horizontal range for P Slice
   \param[in]  iVrtRange_P Vertical range for P Slice
   \param[in]  iHrzRange_B Horizontal range for B Slice
   \param[in]  iVrtRange_B Vertical range for B Slice
   \param[out] pChParam Pointer to the structure receiving the motion estimation parameters
*****************************************************************************/
void AL_Common_Encoder_SetME(int iHrzRange_P, int iVrtRange_P, int iHrzRange_B, int iVrtRange_B, AL_TEncChanParam* pChParam);


void AL_Common_Encoder_SetHlsParam(AL_TEncChanParam* pChParam);
bool AL_Common_Encoder_IsInitialQpProvided(AL_TEncChanParam* pChParam);
uint32_t AL_Common_Encoder_ComputeBitPerPixel(AL_TEncChanParam* pChParam);
int8_t AL_Common_Encoder_GetInitialQP(uint32_t iBitPerPixel);

/*@}*/

