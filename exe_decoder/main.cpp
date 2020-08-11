/******************************************************************************
*
* Copyright (C) 2008-2020 Allegro DVT2.  All rights reserved.
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

#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <climits>
#include <atomic>
#include <memory>
#include <fstream>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
#include <sstream>
#include <mutex>
#include <queue>
#include <map>
#include <thread>

extern "C"
{
#include "lib_common/PixMapBuffer.h"
#include "lib_decode/lib_decode.h"
#include "lib_common_dec/DecBuffers.h"
#include "lib_common_dec/IpDecFourCC.h"
#include "lib_common/StreamBuffer.h"
#include "lib_common/BufferStreamMeta.h"
#include "lib_common/BufferHandleMeta.h"
#include "lib_common/BufferSeiMeta.h"
#include "lib_common_dec/HDRMeta.h"
}

#include "lib_app/BufPool.h"
#include "lib_app/PixMapBufPool.h"
#include "lib_app/console.h"
#include "lib_app/convert.h"
#include "lib_app/timing.h"
#include "lib_app/utils.h"
#include "lib_app/CommandLineParser.h"
#include "lib_app/plateform.h"

#include "Conversion.h"
#include "IpDevice.h"
#include "CodecUtils.h"
#include "crc.h"
#include "InputLoader.h"
#include "HDRWriter.h"

using namespace std;

const char* ToString(AL_ERR eErrCode)
{
  switch(eErrCode)
  {
  case AL_ERR_CHAN_CREATION_NO_CHANNEL_AVAILABLE: return "Channel not created, no channel available";
  case AL_ERR_CHAN_CREATION_RESOURCE_UNAVAILABLE: return "Channel not created, processing power of the available cores insufficient";
  case AL_ERR_CHAN_CREATION_NOT_ENOUGH_CORES: return "Channel not created, couldn't spread the load on enough cores";
  case AL_ERR_REQUEST_MALFORMED: return "Channel not created: request was malformed";
  case AL_ERR_NO_MEMORY: return "Memory shortage detected (dma, embedded memory or virtual memory shortage)";
  case AL_SUCCESS: return "Success";
  default: return "Unknown error";
  }
}

struct codec_error : public runtime_error
{
  explicit codec_error(AL_ERR eErrCode) : runtime_error(ToString(eErrCode)), Code(eErrCode)
  {
  }

  const AL_ERR Code;
};

/* duplicated from Utils.h as we can't take these from inside the libraries */
static inline int RoundUp(int iVal, int iRnd)
{
  return (iVal + iRnd - 1) / iRnd * iRnd;
}

static uint32_t constexpr uDefaultNumBuffersHeldByNextComponent = 1; /* We need at least 1 buffer to copy the output on a file */
static bool bCertCRC = false;
static bool g_MultiChunk = false;
static int g_DecodeAPBId = 1;
static uint8_t constexpr NUMCORE_AUTO = 0;

AL_TDecSettings getDefaultDecSettings()
{
  AL_TDecSettings settings {};

  settings.iStackSize = 2;
  settings.iBitDepth = -1;
  settings.uNumCore = NUMCORE_AUTO;
  settings.uFrameRate = 60000;
  settings.uClkRatio = 1000;
  settings.uDDRWidth = 32;
  settings.eDecUnit = AL_AU_UNIT;
  settings.eDpbMode = AL_DPB_NORMAL;
  settings.eFBStorageMode = AL_FB_RASTER;
  settings.tStream.tDim = { -1, -1 };
  settings.tStream.eChroma = AL_CHROMA_MAX_ENUM;
  settings.tStream.iBitDepth = -1;
  settings.tStream.iProfileIdc = -1;
  settings.tStream.eSequenceMode = AL_SM_MAX_ENUM;
  settings.eCodec = AL_CODEC_HEVC;
  settings.eBufferOutputMode = AL_OUTPUT_INTERNAL;
  settings.bUseIFramesAsSyncPoint = false;
  settings.eInputMode = AL_DEC_UNSPLIT_INPUT;
  return settings;
}

static int const zDefaultInputBufferSize = 32 * 1024;

struct Config
{
  bool help = false;

  string sIn;
  string sMainOut = ""; // Output rec file
  string sCrc;

  AL_TDecSettings tDecSettings = getDefaultDecSettings();

  int iDeviceType = DEVICE_TYPE_BOARD; // board
  SCHEDULER_TYPE iSchedulerType = SCHEDULER_TYPE_MCU;
  int iNumTrace = -1;
  int iNumberTrace = 0;
  bool bForceCleanBuffers = false;
  bool bConceal = false;
  bool bEnableYUVOutput = true;
  unsigned int uInputBufferNum = 2;
  size_t zInputBufferSize = zDefaultInputBufferSize;
  IpCtrlMode ipCtrlMode = IPCTRL_MODE_STANDARD;
  string logsFile = "";
  bool trackDma = false;
  int hangers = 0;
  int iLoop = 1;
  int iTimeoutInSeconds = -1;
  int iMaxFrames = INT_MAX;
  string seiFile = "";
  string hdrFile = "";
};

/******************************************************************************/
static void Usage(CommandLineParser const& opt, char* ExeName)
{
  cerr << "Usage: " << ExeName << " -in <bitstream_file> -out <yuv_file> [options]" << endl;
  cerr << "Options:" << endl;

  opt.usage();

  cerr << endl << "Examples:" << endl;
  cerr << "  " << ExeName << " -avc  -in bitstream.264 -out decoded.yuv -bd 8 " << endl;
  cerr << "  " << ExeName << " -hevc -in bitstream.265 -out decoded.yuv -bd 10" << endl;
  cerr << endl;
}

template<int Offset>
static int IntWithOffset(const string& word)
{
  return atoi(word.c_str()) + Offset;
}

/******************************************************************************/
static AL_EFbStorageMode getMainOutputStorageMode(const AL_TDecSettings& decSettings, bool& bOutputCompression)
{
  AL_EFbStorageMode eOutputStorageMode = decSettings.eFBStorageMode;
  bOutputCompression = decSettings.bFrameBufferCompression;

  return eOutputStorageMode;
}

/******************************************************************************/
void processOutputArgs(Config& config, string sOut, string sRasterOut)
{
  (void)sRasterOut;

  config.tDecSettings.eBufferOutputMode = AL_OUTPUT_INTERNAL;

  if(!config.bEnableYUVOutput)
    return;

  if(sOut.empty())
    sOut = "dec.yuv";
  switch(config.tDecSettings.eBufferOutputMode)
  {
  case AL_OUTPUT_INTERNAL:
    config.sMainOut = sOut;
    break;
  default:
    throw runtime_error("Invalid output buffer mode.");
  }
}

/******************************************************************************/
void getExpectedSeparator(stringstream& ss, char expectedSep)
{
  char sep;
  ss >> sep;

  if(sep != expectedSep)
    throw runtime_error("wrong prealloc arguments separator");
}

bool invalidPreallocSettings(AL_TStreamSettings const& settings)
{
  return settings.iProfileIdc <= 0 || settings.iLevel <= 0
         || settings.tDim.iWidth <= 0 || settings.tDim.iHeight <= 0 || settings.eChroma == AL_CHROMA_MAX_ENUM || settings.eSequenceMode == AL_SM_MAX_ENUM;
}

void parsePreAllocArgs(AL_TStreamSettings* settings, string& toParse)
{
  stringstream ss(toParse);
  ss.unsetf(ios::dec);
  ss.unsetf(ios::hex);
  ss >> settings->tDim.iWidth;
  getExpectedSeparator(ss, 'x');
  ss >> settings->tDim.iHeight;
  getExpectedSeparator(ss, ':');
  char vm[6] {};
  ss >> vm[0];
  ss >> vm[1];
  ss >> vm[2];
  ss >> vm[3];
  ss >> vm[4];
  getExpectedSeparator(ss, ':');
  char chroma[4] {};
  ss >> chroma[0];
  ss >> chroma[1];
  ss >> chroma[2];
  getExpectedSeparator(ss, ':');
  ss >> settings->iBitDepth;
  getExpectedSeparator(ss, ':');
  ss >> settings->iProfileIdc;
  getExpectedSeparator(ss, ':');
  ss >> settings->iLevel;

  /* For pre-allocation, we must use 8x8 (HEVC) or MB (AVC) rounded dimensions, like the SPS. */
  /* Actually, round up to the LCU so we're able to support resolution changes with the same LCU sizes. */
  /* And because we don't know the codec here, always use 64 as MB/LCU size. */
  settings->tDim.iWidth = RoundUp(settings->tDim.iWidth, 64);
  settings->tDim.iHeight = RoundUp(settings->tDim.iHeight, 64);

  if(string(chroma) == "400")
    settings->eChroma = AL_CHROMA_4_0_0;
  else if(string(chroma) == "420")
    settings->eChroma = AL_CHROMA_4_2_0;
  else if(string(chroma) == "422")
    settings->eChroma = AL_CHROMA_4_2_2;
  else if(string(chroma) == "444")
    settings->eChroma = AL_CHROMA_4_4_4;
  else
    throw runtime_error("wrong prealloc chroma format");

  if(string(vm) == "unkwn")
    settings->eSequenceMode = AL_SM_UNKNOWN;
  else if(string(vm) == "progr")
    settings->eSequenceMode = AL_SM_PROGRESSIVE;
  else if(string(vm) == "inter")
    settings->eSequenceMode = AL_SM_INTERLACED;
  else
    throw runtime_error("wrong prealloc video format");

  if(ss.fail() || (ss.tellg() != streampos(-1)))
    throw runtime_error("wrong prealloc arguments format");

  if(invalidPreallocSettings(*settings))
    throw runtime_error("wrong prealloc arguments");
}

/******************************************************************************/
static Config ParseCommandLine(int argc, char* argv[])
{
  Config Config;

  int fps = 0;
  bool version = false;
  bool helpJson = false;

  string sOut;
  string sRasterOut;

  auto opt = CommandLineParser();

  opt.addFlag("--help,-h", &Config.help, "Shows this help");
  opt.addFlag("--help-json", &helpJson, "Show this help (json)");
  opt.addFlag("--version", &version, "Show version");

  opt.addString("-in,-i", &Config.sIn, "Input bitstream");
  opt.addString("-out,-o", &sOut, "Output YUV");

  opt.addFlag("-avc", &Config.tDecSettings.eCodec,
              "Specify the input bitstream codec (default: HEVC)",
              AL_CODEC_AVC);

  opt.addFlag("-hevc", &Config.tDecSettings.eCodec,
              "Specify the input bitstream codec (default: HEVC)",
              AL_CODEC_HEVC);

  opt.addInt("-fps", &fps, "force framerate");
  opt.addCustom("-clk", &Config.tDecSettings.uClkRatio, &IntWithOffset<1000>, "Set clock ratio, (0 for 1000, 1 for 1001)", "number");
  opt.addInt("-bd", &Config.tDecSettings.iBitDepth, "Output YUV bitdepth (0:auto, 8, 10, 12)");
  opt.addFlag("--sync-i-frames", &Config.tDecSettings.bUseIFramesAsSyncPoint,
              "Allow decoder to sync on I frames if configurations' nals are presents",
              true);

  opt.addFlag("-wpp", &Config.tDecSettings.bParallelWPP, "Wavefront parallelization processing activation");
  opt.addFlag("-lowlat", &Config.tDecSettings.bLowLat, "Low latency decoding activation");
  opt.addOption("-slicelat", [&](string)
  {
    Config.tDecSettings.eDecUnit = AL_VCL_NAL_UNIT;
    Config.tDecSettings.eDpbMode = AL_DPB_NO_REORDERING;
  }, "Specify decoder latency (default: Frame Latency)");

  opt.addFlag("-framelat", &Config.tDecSettings.eDecUnit,
              "Specify decoder latency (default: Frame Latency)",
              AL_AU_UNIT);

  opt.addFlag("--no-reordering", &Config.tDecSettings.eDpbMode,
              "Indicates to decoder that the stream doesn't contain B-frame & reference must be at best 1",
              AL_DPB_NO_REORDERING);

  opt.addFlag("--split-input", &Config.tDecSettings.eInputMode,
              "Send stream by decoding unit",
              AL_DEC_SPLIT_INPUT);

  opt.addString("--sei-file", &Config.seiFile, "File in which the SEI decoded by the decoder will be dumped");

  opt.addString("--hdr-file", &Config.hdrFile, "Parse and dump HDR data in the specified file");

  string preAllocArgs = "";
  opt.addString("--prealloc-args", &preAllocArgs, "Specify stream's parameters: '1920x1080:video-mode:422:10:profile-idc:level'.");

  opt.startSection("Run");

  opt.addInt("--max-frames", &Config.iMaxFrames, "Abort after max number of decoded frames (approximative abort)");
  opt.addInt("-loop", &Config.iLoop, "Number of Decoding loop (optional)");
  opt.addInt("--timeout", &Config.iTimeoutInSeconds, "Specify timeout in seconds");

  opt.endSection("Run");

  opt.startSection("Trace && Debug");

  opt.addFlag("--multi-chunk", &g_MultiChunk, "Allocate luma and chroma of decoded frames on different memory chunks");
  opt.addInt("-nbuf", &Config.uInputBufferNum, "Specify the number of input feeder buffer");
  opt.addInt("-nsize", &Config.zInputBufferSize, "Specify the size (in bytes) of input feeder buffer");

  opt.addString("-crc_ip", &Config.sCrc, "Output crc file");

  opt.addOption("-t", [&](string)
  {
    Config.iNumTrace = opt.popInt();
    Config.iNumberTrace = 1;
  }, "First frame to trace (optional)", "number");

  opt.addInt("-num", &Config.iNumberTrace, "Number of frames to trace");

  opt.addFlag("--use-early-callback", &Config.tDecSettings.bUseEarlyCallback, "Low latency phase 2. Call end decoding at decoding launch. This only makes sense with special support for hardware synchronization");
  opt.addInt("-core", &Config.tDecSettings.uNumCore, "number of decoder cores");
  opt.addInt("-ddrwidth", &Config.tDecSettings.uDDRWidth, "Width of DDR requests (16, 32, 64) (default: 32)");
  opt.addFlag("-nocache", &Config.tDecSettings.bDisableCache, "Inactivate the cache");

  opt.addFlag("-noyuv", &Config.bEnableYUVOutput,
              "Disable writing output YUV file",
              false);

  opt.addString("--log", &Config.logsFile, "A file where logged events will be dumped");

  opt.endSection("Trace && Debug");

  opt.startSection("Misc");
  opt.addOption("--color", [&](string)
  {
    SetEnableColor(true);
  }, "Enable color (Default: Auto)");

  opt.addOption("--no-color", [&](string)
  {
    SetEnableColor(false);
  }, "Disable color");

  opt.addFlag("--quiet,-q", &g_Verbosity, "Do not print anything", 0);
  opt.addInt("--verbosity", &g_Verbosity, "Choose the verbosity level (-q is equivalent to --verbosity 0)");
  opt.endSection("Misc");

  opt.startSection("Deprecated");
  opt.addFlag("-lowref", &Config.tDecSettings.eDpbMode,
              "[DEPRECATED] Use --no-reordering instead. Indicates to decoder that the stream doesn't contain B-frame & reference must be at best 1",
              AL_DPB_NO_REORDERING);
  opt.endSection("Deprecated");

  opt.parse(argc, argv);

  if(Config.help)
  {
    Usage(opt, argv[0]);
    return Config;
  }

  if(helpJson)
  {
    opt.usageJson();
    exit(0);
  }

  if(version)
  {
    DisplayVersionInfo();
    DisplayBuildInfo();
    exit(0);
  }

  processOutputArgs(Config, sOut, sRasterOut);

  bool bMainOutputCompression;
  getMainOutputStorageMode(Config.tDecSettings, bMainOutputCompression);

  if(bMainOutputCompression)
  {
    if(bCertCRC)
      throw runtime_error("Certification CRC unavaible with fbc");
    bCertCRC = false;
  }

  if(fps > 0)
  {
    Config.tDecSettings.uFrameRate = fps * 1000;
    Config.tDecSettings.bForceFrameRate = true;
  }

  {
    if(!preAllocArgs.empty())
      parsePreAllocArgs(&Config.tDecSettings.tStream, preAllocArgs);

    if(Config.tDecSettings.uNumCore > AL_DEC_NUM_CORES)
      throw runtime_error("Invalid number of cores");

    if(Config.tDecSettings.uDDRWidth != 16 && Config.tDecSettings.uDDRWidth != 32 && Config.tDecSettings.uDDRWidth != 64)
      throw runtime_error("Invalid DDR width");

    // silently correct user settings
    Config.uInputBufferNum = max(1u, Config.uInputBufferNum);
    Config.zInputBufferSize = max(size_t(1), Config.zInputBufferSize);
    Config.zInputBufferSize = (!preAllocArgs.empty() && Config.zInputBufferSize == zDefaultInputBufferSize) ? AL_GetMaxNalSize(Config.tDecSettings.eCodec, Config.tDecSettings.tStream.tDim, Config.tDecSettings.tStream.eChroma, Config.tDecSettings.tStream.iBitDepth, Config.tDecSettings.tStream.iLevel, Config.tDecSettings.tStream.iProfileIdc) : Config.zInputBufferSize;
    Config.tDecSettings.iStackSize = max(1, Config.tDecSettings.iStackSize);
  }

  if(Config.sIn.empty())
    throw runtime_error("No input file specified (use -h to get help)");

  return Config;
}

typedef function<void (AL_TBuffer const*, AL_TBuffer*)> AL_TO_IP;
typedef void AL_TO_IP_SCALE (const AL_TBuffer*, AL_TBuffer*, uint8_t, uint8_t);

AL_TO_IP Bind(AL_TO_IP_SCALE* convertFunc, int horzScale, int vertScale)
{
  auto conversion = [=](AL_TBuffer const* src, AL_TBuffer* dst)
                    {
                      convertFunc(src, dst, horzScale, vertScale);
                    };

  return conversion;
}

#define GetConvFormat(ChromaMode, iBdIn, iBdOut) ((ChromaMode) | ((iBdIn) << 8) | ((iBdOut) << 16))

AL_TO_IP Get8BitsConversionFunction(int iPicFmt)
{
  auto constexpr AL_CHROMA_MONO_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 8, 8);
  auto constexpr AL_CHROMA_MONO_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 8, 10);

  auto constexpr AL_CHROMA_420_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 8, 8);
  auto constexpr AL_CHROMA_420_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 8, 10);

  auto constexpr AL_CHROMA_422_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 8, 8);
  auto constexpr AL_CHROMA_422_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 8, 10);

  auto constexpr AL_CHROMA_444_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 8, 8);
  auto constexpr AL_CHROMA_444_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 8, 10);
  switch(iPicFmt)
  {
  case AL_CHROMA_420_8bitTo8bit:
    return NV12_To_I420;
  case AL_CHROMA_420_8bitTo10bit:
    return NV12_To_I0AL;
  case AL_CHROMA_422_8bitTo8bit:
    return NV16_To_I422;
  case AL_CHROMA_422_8bitTo10bit:
    return NV16_To_I2AL;
  case AL_CHROMA_444_8bitTo8bit:
    return CopyPixMapBuffer;
  case AL_CHROMA_444_8bitTo10bit:
    return I444_To_I4AL;
  case AL_CHROMA_MONO_8bitTo8bit:
    return CopyPixMapBuffer;
  case AL_CHROMA_MONO_8bitTo10bit:
    return Y800_To_Y010;
  default:
    assert(0);
    return nullptr;
  }
}

AL_TO_IP Get10BitsConversionFunction(int iPicFmt)
{
  auto const AL_CHROMA_MONO_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 10, 10);
  auto const AL_CHROMA_MONO_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 10, 8);

  auto const AL_CHROMA_420_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 10, 10);
  auto const AL_CHROMA_420_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 10, 8);

  auto const AL_CHROMA_422_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 10, 10);
  auto const AL_CHROMA_422_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 10, 8);

  auto const AL_CHROMA_444_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 10, 10);
  auto const AL_CHROMA_444_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 10, 8);
  switch(iPicFmt)
  {
  case AL_CHROMA_420_10bitTo10bit:
    return XV15_To_I0AL;
  case AL_CHROMA_420_10bitTo8bit:
    return XV15_To_I420;
  case AL_CHROMA_422_10bitTo10bit:
    return XV20_To_I2AL;
  case AL_CHROMA_422_10bitTo8bit:
    return XV20_To_I422;

  case AL_CHROMA_444_10bitTo10bit:
    return CopyPixMapBuffer;

  case AL_CHROMA_444_10bitTo8bit:
    return I4AL_To_I444;

  case AL_CHROMA_MONO_10bitTo10bit:
    return XV10_To_Y010;
  case AL_CHROMA_MONO_10bitTo8bit:
    return XV10_To_Y800;
  default:
    assert(0);
    return nullptr;
  }
}

AL_TO_IP Get12BitsConversionFunction(int iPicFmt)
{
  auto const AL_CHROMA_MONO_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 12, 12);
  auto const AL_CHROMA_MONO_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 12, 10);
  auto const AL_CHROMA_MONO_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_0_0, 12, 8);

  auto const AL_CHROMA_420_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 12, 12);
  auto const AL_CHROMA_420_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 12, 10);
  auto const AL_CHROMA_420_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 12, 8);

  auto const AL_CHROMA_422_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 12, 12);
  auto const AL_CHROMA_422_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 12, 10);
  auto const AL_CHROMA_422_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 12, 8);

  auto const AL_CHROMA_444_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 12, 12);
  auto const AL_CHROMA_444_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 12, 10);
  auto const AL_CHROMA_444_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 12, 8);
  switch(iPicFmt)
  {
  case AL_CHROMA_420_12bitTo12bit:
    return P012_To_I0CL;
  case AL_CHROMA_420_12bitTo10bit:
    return P012_To_I0AL;
  case AL_CHROMA_420_12bitTo8bit:
    return P012_To_I420;

  case AL_CHROMA_422_12bitTo12bit:
    return P212_To_I2CL;
  case AL_CHROMA_422_12bitTo10bit:
    return P212_To_I2AL;
  case AL_CHROMA_422_12bitTo8bit:
    return P212_To_I422;

  case AL_CHROMA_444_12bitTo12bit:
    return CopyPixMapBuffer;
  case AL_CHROMA_444_12bitTo10bit:
    return I4CL_To_I4AL;
  case AL_CHROMA_444_12bitTo8bit:
    return I4CL_To_I444;

  case AL_CHROMA_MONO_12bitTo12bit:
    return CopyPixMapBuffer;
  case AL_CHROMA_MONO_12bitTo10bit:
    return Y012_To_Y010;
  case AL_CHROMA_MONO_12bitTo8bit:
    return Y012_To_Y800;

  default:
    assert(0);
    return nullptr;
  }
}

AL_TO_IP GetTileConversionFunction(int iPicFmt)
{
  auto const AL_CHROMA_MONO_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 8, 8);
  auto const AL_CHROMA_MONO_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 8, 10);

  auto const AL_CHROMA_420_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 8, 8);
  auto const AL_CHROMA_420_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 8, 10);

  auto const AL_CHROMA_422_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 8, 8);
  auto const AL_CHROMA_422_8bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 8, 10);

  auto const AL_CHROMA_444_8bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 8, 8);
  auto const AL_CHROMA_444_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 10, 10);
  auto const AL_CHROMA_444_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_4_4, 12, 12);

  auto const AL_CHROMA_MONO_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 10, 10);
  auto const AL_CHROMA_MONO_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 10, 8);

  auto const AL_CHROMA_420_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 10, 10);
  auto const AL_CHROMA_420_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 10, 8);

  auto const AL_CHROMA_422_10bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 10, 10);
  auto const AL_CHROMA_422_10bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 10, 8);

  auto const AL_CHROMA_MONO_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 12, 12);
  auto const AL_CHROMA_MONO_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 12, 10);
  auto const AL_CHROMA_MONO_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_MONO, 12, 8);

  auto const AL_CHROMA_420_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 12, 12);
  auto const AL_CHROMA_420_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 12, 10);
  auto const AL_CHROMA_420_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_0, 12, 8);

  auto const AL_CHROMA_422_12bitTo12bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 12, 12);
  auto const AL_CHROMA_422_12bitTo10bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 12, 10);
  auto const AL_CHROMA_422_12bitTo8bit = GetConvFormat(AL_EChromaMode::AL_CHROMA_4_2_2, 12, 8);
  switch(iPicFmt)
  {
  case AL_CHROMA_420_8bitTo8bit: return T608_To_I420;
  case AL_CHROMA_422_8bitTo8bit: return T628_To_I422;
  case AL_CHROMA_444_8bitTo8bit: return T648_To_I444;

  case AL_CHROMA_420_8bitTo10bit: return T608_To_I0AL;
  case AL_CHROMA_422_8bitTo10bit: return T628_To_I2AL;

  case AL_CHROMA_MONO_8bitTo8bit: return T608_To_Y800;
  case AL_CHROMA_MONO_8bitTo10bit: return T608_To_Y010;

  case AL_CHROMA_420_10bitTo10bit: return T60A_To_I0AL;
  case AL_CHROMA_420_10bitTo8bit: return T60A_To_I420;

  case AL_CHROMA_422_10bitTo10bit: return T62A_To_I2AL;
  case AL_CHROMA_422_10bitTo8bit: return T62A_To_I422;

  case AL_CHROMA_444_10bitTo10bit: return T64A_To_I4AL;

  case AL_CHROMA_MONO_10bitTo10bit: return T60A_To_Y010;
  case AL_CHROMA_MONO_10bitTo8bit: return T60A_To_Y800;

  case AL_CHROMA_MONO_12bitTo12bit: return T60C_To_Y012;
  case AL_CHROMA_MONO_12bitTo10bit: return T60C_To_Y010;
  case AL_CHROMA_MONO_12bitTo8bit: return T60C_To_Y800;

  case AL_CHROMA_420_12bitTo12bit: return T60C_To_I0CL;
  case AL_CHROMA_420_12bitTo10bit: return T60C_To_I0AL;
  case AL_CHROMA_420_12bitTo8bit: return T60C_To_I420;

  case AL_CHROMA_422_12bitTo12bit: return T62C_To_I2CL;
  case AL_CHROMA_422_12bitTo10bit: return T62C_To_I2AL;
  case AL_CHROMA_422_12bitTo8bit: return T62C_To_I422;

  case AL_CHROMA_444_12bitTo12bit: return T64C_To_I4CL;

  default:
  {
    assert(0 && "Unknown picture format");
    return nullptr;
  }
  }
}

AL_TO_IP GetConversionFunction(TFourCC input, int iBdOut)
{
  auto const eChromaMode = AL_GetChromaMode(input);
  auto const iBdIn = AL_GetBitDepth(input);
  int iPicFmt = GetConvFormat(eChromaMode, iBdIn, iBdOut);

  if(AL_IsTiled(input))
    return GetTileConversionFunction(iPicFmt);
  else if(iBdIn == 8)
    return Get8BitsConversionFunction(iPicFmt);
  else if(iBdIn == 10)
    return Get10BitsConversionFunction(iPicFmt);
  else
    return Get12BitsConversionFunction(iPicFmt);
}

static void ConvertFrameBuffer(AL_TBuffer& input, AL_TBuffer*& pOutput, int iBdOut)
{
  TFourCC tRecFourCC = AL_PixMapBuffer_GetFourCC(&input);
  AL_TDimension tRecDim = AL_PixMapBuffer_GetDimension(&input);

  if(pOutput != NULL)
  {
    AL_TDimension tYuvDim = AL_PixMapBuffer_GetDimension(pOutput);

    if(tRecDim.iWidth != tYuvDim.iWidth || tRecDim.iHeight != tYuvDim.iHeight)
    {
      AL_Buffer_Destroy(pOutput);
      pOutput = NULL;
    }
  }

  if(pOutput == NULL)
  {
    AL_EChromaMode eChromaMode = AL_GetChromaMode(tRecFourCC);
    AL_TPicFormat tConvPicFormat = AL_TPicFormat {
      eChromaMode, static_cast<uint8_t>(iBdOut), AL_FB_RASTER,
      eChromaMode == AL_CHROMA_MONO ? AL_C_ORDER_NO_CHROMA : AL_C_ORDER_U_V, false, false
    };
    TFourCC tConvFourCC = AL_GetFourCC(tConvPicFormat);
    pOutput = AL_PixMapBuffer_Create(AL_GetDefaultAllocator(), NULL, tRecDim, tConvFourCC);

    if(pOutput == NULL)
      throw runtime_error("Couldn't allocate YuvBuffer");

    int sx = 1, sy = 1;
    AL_GetSubsampling(tConvFourCC, &sx, &sy);

    auto const iSizePix = (iBdOut + 7) >> 3;
    auto const iPitchY = iSizePix * tRecDim.iWidth;
    auto const iPitchC = (iPitchY + sx - 1) / sx;
    auto const iSizeY = iPitchY * tRecDim.iHeight;
    auto const iSizeC = iPitchC * ((tRecDim.iHeight + sy - 1) / sy);

    AL_TPlaneDescription tPlaneDesc[3] =
    {
      { AL_PLANE_Y, 0, iPitchY },
      { AL_PLANE_U, iSizeY, iPitchC },
      { AL_PLANE_V, iSizeY + iSizeC, iPitchC }
    };

    uint32_t uSize = iSizeY;

    if(tConvPicFormat.eChromaMode != AL_CHROMA_MONO)
      uSize += 2 * iSizeC;

    if(!AL_PixMapBuffer_Allocate_And_AddPlanes(pOutput, uSize, &tPlaneDesc[0], tConvPicFormat.eChromaMode == AL_CHROMA_MONO ? 1 : 3, "conversion frame buffer"))
      throw runtime_error("Couldn't allocate YuvBuffer planes");
  }

  auto AllegroConvert = GetConversionFunction(tRecFourCC, iBdOut);
  AllegroConvert(&input, pOutput);
}

/******************************************************************************/
class BaseOutputWriter
{
public:
  BaseOutputWriter(const string& sYuvFileName, const string& sIPCrcFileName);
  virtual ~BaseOutputWriter() {};

  void ProcessOutput(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut);
  virtual void ProcessFrame(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut) = 0;

protected:
  ofstream YuvFile;
  ofstream IpCrcFile;
};

BaseOutputWriter::BaseOutputWriter(const string& sYuvFileName, const string& sIPCrcFileName)
{
  if(!sYuvFileName.empty())
  {
    OpenOutput(YuvFile, sYuvFileName);
  }

  if(!sIPCrcFileName.empty())
  {
    OpenOutput(IpCrcFile, sIPCrcFileName, false);
    IpCrcFile << hex << uppercase;
  }
}

void BaseOutputWriter::ProcessOutput(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut)
{
  if(IpCrcFile.is_open())
    IpCrcFile << std::setfill('0') << std::setw(8) << (int)info.uCRC << std::endl;

  ProcessFrame(tRecBuf, info, iBdOut);
}

/******************************************************************************/
class UncompressedOutputWriter : public BaseOutputWriter
{
public:
  ~UncompressedOutputWriter();

  UncompressedOutputWriter(const string& sYuvFileName, const string& sIPCrcFileName, const string& sCertCrcFileName);
  void ProcessFrame(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut) override;

private:
  ofstream CertCrcFile; // Cert crc only computed for uncompressed output
  AL_TBuffer* YuvBuffer = NULL;
  int convertBitDepthToEven(int iBd) const;
};

UncompressedOutputWriter::~UncompressedOutputWriter()
{
  if(NULL != YuvBuffer)
  {
    AL_Buffer_Destroy(YuvBuffer);
  }
}

UncompressedOutputWriter::UncompressedOutputWriter(const string& sYuvFileName, const string& sIPCrcFileName, const string& sCertCrcFileName) :
  BaseOutputWriter(sYuvFileName, sIPCrcFileName)
{
  if(!sCertCrcFileName.empty())
  {
    OpenOutput(CertCrcFile, sCertCrcFileName, false);
    CertCrcFile << hex << uppercase;
  }
}

int UncompressedOutputWriter::convertBitDepthToEven(int iBd) const
{
  return ((iBd % 2) != 0) ? iBd + 1 : iBd;
}

void UncompressedOutputWriter::ProcessFrame(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut)
{
  if(!(YuvFile.is_open() || CertCrcFile.is_open()))
    return;
  iBdOut = convertBitDepthToEven(iBdOut);

  ConvertFrameBuffer(tRecBuf, YuvBuffer, iBdOut);

  auto const iSizePix = (iBdOut + 7) >> 3;

  if(info.tCrop.bCropping)
    CropFrame(YuvBuffer, iSizePix, info.tCrop.uCropOffsetLeft, info.tCrop.uCropOffsetRight, info.tCrop.uCropOffsetTop, info.tCrop.uCropOffsetBottom);

  TFourCC tRecFourCC = AL_PixMapBuffer_GetFourCC(&tRecBuf);

  int sx = 1, sy = 1;
  AL_GetSubsampling(tRecFourCC, &sx, &sy);
  AL_TDimension tYuvDim = AL_PixMapBuffer_GetDimension(YuvBuffer);
  int const iNumPix = tYuvDim.iHeight * tYuvDim.iWidth;
  int const iNumPixC = AL_GetChromaMode(tRecFourCC) == AL_CHROMA_MONO ? 0 : ((tYuvDim.iWidth + sx - 1) / sx) * ((tYuvDim.iHeight + sy - 1) / sy);

  if(CertCrcFile.is_open())
  {
    // compute crc
    auto eChromaMode = AL_GetChromaMode(tRecFourCC);

    uint8_t* pBuf = AL_PixMapBuffer_GetPlaneAddress(YuvBuffer, AL_PLANE_Y);

    if(iBdOut == 8)
      Compute_CRC(info.uBitDepthY, info.uBitDepthC, iBdOut, iNumPix, iNumPixC, eChromaMode, pBuf, CertCrcFile);
    else
      Compute_CRC(info.uBitDepthY, info.uBitDepthC, iBdOut, iNumPix, iNumPixC, eChromaMode, (uint16_t*)pBuf, CertCrcFile);
  }

  if(YuvFile.is_open())
    YuvFile.write((const char*)AL_PixMapBuffer_GetPlaneAddress(YuvBuffer, AL_PLANE_Y), (iNumPix + 2 * iNumPixC) * iSizePix);
}

/******************************************************************************/
struct Display
{
  Display()
  {
    hExitMain = Rtos_CreateEvent(false);
  }

  ~Display()
  {
    Rtos_DeleteEvent(hExitMain);
  }

  void AddOutputWriter(AL_e_FbStorageMode eFbStorageMode, bool bCompressionEnabled, const string& sYuvFileName, const string& sIPCrcFileName, const string& sCertCrcFileName, AL_ECodec eCodec);

  void Process(AL_TBuffer* pFrame, AL_TInfoDecode* pInfo);
  void ProcessFrame(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut);

  AL_HDecoder hDec = NULL;
  AL_EVENT hExitMain = NULL;
  std::map<AL_EFbStorageMode, std::shared_ptr<BaseOutputWriter>> writers;
  AL_EFbStorageMode eMainOutputStorageMode;
  int iBitDepth = 8;
  unsigned int NumFrames = 0;
  unsigned int MaxFrames = UINT_MAX;
  unsigned int FirstFrame = 0;
  mutex hMutex;
  int iNumFrameConceal = 0;
  std::shared_ptr<HDRWriter> pHDRWriter;
};

struct ResChgParam
{
  AL_HDecoder hDec;
  bool bPoolIsInit;
  PixMapBufPool bufPool;
  AL_TDecSettings* pDecSettings;
  AL_TAllocator* pAllocator;
  AL_TDecSettings* pSettings;
  bool bAddHDRMetaData;
  mutex hMutex;
};

struct DecodeParam
{
  AL_HDecoder hDec;
  AL_EVENT hExitMain = NULL;
  atomic<int> decodedFrames;
  ofstream* seiSyncOutput;
};

/******************************************************************************/
void Display::AddOutputWriter(AL_e_FbStorageMode eFbStorageMode, bool bCompressionEnabled, const string& sYuvFileName, const string& sIPCrcFileName, const string& sCertCrcFileName, AL_ECodec eCodec)
{
  (void)eCodec;
  (void)bCompressionEnabled;
  {
    writers[eFbStorageMode] = std::shared_ptr<BaseOutputWriter>(new UncompressedOutputWriter(sYuvFileName, sIPCrcFileName, sCertCrcFileName));
  }
}

/******************************************************************************/
static void printHexdump(ostream* logger, uint8_t* data, int size)
{
  int column = 0;
  int toPrint = size;

  *logger << std::hex;

  while(toPrint > 0)
  {
    *logger << setfill('0') << setw(2) << (int)data[size - toPrint];
    --toPrint;
    ++column;

    if(toPrint > 0)
    {
      if(column % 8 == 0)
        *logger << endl;
      else
        *logger << " ";
    }
  }

  *logger << std::dec;
}

static void writeSei(bool bIsPrefix, int iPayloadType, uint8_t* pPayload, int iPayloadSize, ostream* seiOut)
{
  if(!seiOut)
    return;
  *seiOut << "is_prefix: " << boolalpha << bIsPrefix << endl
          << "sei_payload_type: " << iPayloadType << endl
          << "sei_payload_size: " << iPayloadSize << endl
          << "raw:" << endl;
  printHexdump(seiOut, pPayload, iPayloadSize);
  *seiOut << endl << endl;
}

/******************************************************************************/
static void WriteSyncSei(AL_TBuffer* pDecodedFrame, ofstream* seiOut)
{
  auto pInput = (AL_THandleMetaData*)AL_Buffer_GetMetaData(pDecodedFrame, AL_META_TYPE_HANDLE);

  if(!pInput)
    return;

  int numHandles = AL_HandleMetaData_GetNumHandles(pInput);

  for(auto handle = 0; handle < numHandles; ++handle)
  {
    AL_TDecMetaHandle* pDecMetaHandle = (AL_TDecMetaHandle*)AL_HandleMetaData_GetHandle(pInput, handle);
    auto pSei = (AL_TSeiMetaData*)AL_Buffer_GetMetaData(pDecMetaHandle->pHandle, AL_META_TYPE_SEI);

    if(!pSei)
      continue;

    auto pPayload = pSei->payload;

    for(auto i = 0; i < pSei->numPayload; ++i, ++pPayload)
      writeSei(pPayload->bPrefix, pPayload->type, pPayload->pData, pPayload->size, seiOut);
  }
}

/******************************************************************************/
static void sInputParsed(AL_TBuffer* pParsedFrame, void* pUserParam, int iParsingID)
{
  (void)pUserParam;

  AL_THandleMetaData* pHandlesMeta = (AL_THandleMetaData*)AL_Buffer_GetMetaData(pParsedFrame, AL_META_TYPE_HANDLE);

  if(!pHandlesMeta)
    return;

/* Ref pStream because we use it in end decoding to dump sei and avoid a copy
 * Avoiding the copy increase the latency because we delay the release of the pStream buffer
 */

  int numHandles = AL_HandleMetaData_GetNumHandles(pHandlesMeta);
  assert(iParsingID < numHandles);

  AL_TDecMetaHandle* pDecMetaHandle = (AL_TDecMetaHandle*)AL_HandleMetaData_GetHandle(pHandlesMeta, iParsingID);

  if(pDecMetaHandle->eState == AL_DEC_HANDLE_STATE_PROCESSED)
  {
    AL_TBuffer* pStream = pDecMetaHandle->pHandle;
    AL_Buffer_Ref(pStream);
    return;
  }

  AL_Assert(0);
}

/******************************************************************************/
static void sFrameDecoded(AL_TBuffer* pDecodedFrame, void* pUserParam)
{
  auto pParam = static_cast<DecodeParam*>(pUserParam);

  pParam->decodedFrames++;

  if(!pDecodedFrame)
  {
    Rtos_SetEvent(pParam->hExitMain);
    return;
  }

  if(pParam->seiSyncOutput)
    WriteSyncSei(pDecodedFrame, pParam->seiSyncOutput);

  /* Unref all handles once Seis are dumped */
  AL_THandleMetaData* pMeta = (AL_THandleMetaData*)AL_Buffer_GetMetaData(pDecodedFrame, AL_META_TYPE_HANDLE);

  if(pMeta)
  {
    int numHandles = AL_HandleMetaData_GetNumHandles(pMeta);

    for(int handle = 0; handle < numHandles; handle++)
    {
      AL_TDecMetaHandle* pDecMetaHandle = (AL_TDecMetaHandle*)AL_HandleMetaData_GetHandle(pMeta, handle);

      if(pDecMetaHandle->eState == AL_DEC_HANDLE_STATE_PROCESSED)
        AL_Buffer_Unref(pDecMetaHandle->pHandle);
    }
  }
}

/******************************************************************************/
static bool isEOS(AL_TBuffer* pFrame, AL_TInfoDecode* pInfo)
{
  return !pFrame && !pInfo;
}

/******************************************************************************/
static bool isReleaseFrame(AL_TBuffer* pFrame, AL_TInfoDecode* pInfo)
{
  return pFrame && !pInfo;
}

/******************************************************************************/
static void sFrameDisplay(AL_TBuffer* pFrame, AL_TInfoDecode* pInfo, void* pUserParam)
{
  auto pDisplay = reinterpret_cast<Display*>(pUserParam);
  pDisplay->Process(pFrame, pInfo);
}

void Display::Process(AL_TBuffer* pFrame, AL_TInfoDecode* pInfo)
{
  unique_lock<mutex> lock(hMutex);

  AL_ERR err = AL_Decoder_GetFrameError(hDec, pFrame);
  bool bExitError = AL_IS_ERROR_CODE(err);

  if(bExitError || isEOS(pFrame, pInfo))
  {
    if(err == AL_WARN_SPS_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS)
      LogDimmedWarning("\nDecoder has discarded some SPS not compatible with the channel settings\n");

    if(err == AL_WARN_SEI_OVERFLOW)
      LogDimmedWarning("\nDecoder has discarded some SEI while the SEI metadata buffer was too small\n");

    if(bExitError)
      LogError("Error: %d", err);
    else
      LogVerbose(CC_GREY, "Complete\n\n");
    Rtos_SetEvent(hExitMain);
    return;
  }

  if(err == AL_WARN_CONCEAL_DETECT)
    iNumFrameConceal++;

  if(isReleaseFrame(pFrame, pInfo))
    return;

  if(iBitDepth == 0)
    iBitDepth = max(pInfo->uBitDepthY, pInfo->uBitDepthC);
  else if(iBitDepth == -1)
    iBitDepth = AL_Decoder_GetMaxBD(hDec);

  assert(AL_Buffer_GetData(pFrame));

  ProcessFrame(*pFrame, *pInfo, iBitDepth);

  if(pInfo->eFbStorageMode == eMainOutputStorageMode)
  {
    auto pHDR = (AL_THDRMetaData*)AL_Buffer_GetMetaData(pFrame, AL_META_TYPE_HDR);

    if(pHDR != nullptr && pHDRWriter != nullptr)
      pHDRWriter->WriteHDRSEIs(pHDR->eColourDescription, pHDR->eTransferCharacteristics, pHDR->eColourMatrixCoeffs, pHDR->tHDRSEIs);

    AL_Decoder_PutDisplayPicture(hDec, pFrame);

    // TODO: increase only when last frame
    DisplayFrameStatus(NumFrames);
    NumFrames++;

    if(NumFrames > MaxFrames)
      Rtos_SetEvent(hExitMain);
  }
}

/******************************************************************************/
void Display::ProcessFrame(AL_TBuffer& tRecBuf, AL_TInfoDecode info, int iBdOut)
{
  if(writers.find(info.eFbStorageMode) != writers.end() && (NumFrames >= FirstFrame))
    writers[info.eFbStorageMode]->ProcessOutput(tRecBuf, info, iBdOut);
}

static string FourCCToString(TFourCC tFourCC)
{
  stringstream ss;
  ss << static_cast<char>(tFourCC & 0xFF) << static_cast<char>((tFourCC & 0xFF00) >> 8) << static_cast<char>((tFourCC & 0xFF0000) >> 16) << static_cast<char>((tFourCC & 0xFF000000) >> 24);
  return ss.str();
}

static string SequencePictureToString(AL_ESequenceMode sequencePicture)
{
  if(sequencePicture == AL_SM_UNKNOWN)
    return "unknown";

  if(sequencePicture == AL_SM_PROGRESSIVE)
    return "progressive";

  if(sequencePicture == AL_SM_INTERLACED)
    return "interlaced";
  return "max enum";
}

static void showStreamInfo(int BufferNumber, int BufferSize, AL_TStreamSettings const* pSettings, AL_TCropInfo const* pCropInfo, TFourCC tFourCC)
{
  auto& tDim = pSettings->tDim;
  int iWidth = tDim.iWidth;
  int iHeight = tDim.iHeight;

  stringstream ss;
  ss << "Resolution: " << iWidth << "x" << iHeight << endl;
  ss << "FourCC: " << FourCCToString(tFourCC) << endl;
  ss << "Profile: " << pSettings->iProfileIdc << endl;
  ss << "Level: " << pSettings->iLevel << endl;
  ss << "Bitdepth: " << pSettings->iBitDepth << endl;

  if(AL_NeedsCropping(pCropInfo))
  {
    auto uCropWidth = pCropInfo->uCropOffsetLeft + pCropInfo->uCropOffsetRight;
    auto uCropHeight = pCropInfo->uCropOffsetTop + pCropInfo->uCropOffsetBottom;
    ss << "Crop top: " << pCropInfo->uCropOffsetTop << endl;
    ss << "Crop bottom: " << pCropInfo->uCropOffsetBottom << endl;
    ss << "Crop left: " << pCropInfo->uCropOffsetLeft << endl;
    ss << "Crop right: " << pCropInfo->uCropOffsetRight << endl;
    ss << "Display resolution: " << iWidth - uCropWidth << "x" << iHeight - uCropHeight << endl;
  }
  ss << "Sequence picture: " << SequencePictureToString(pSettings->eSequenceMode) << endl;
  ss << "Buffers needed: " << BufferNumber << " of size " << BufferSize << endl;

  LogInfo(CC_DARK_BLUE, "%s\n", ss.str().c_str());
}

static void sParsedSei(bool bIsPrefix, int iPayloadType, uint8_t* pPayload, int iPayloadSize, void* pUserParam)
{
  auto seiOutput = static_cast<ostream*>(pUserParam);
  writeSei(bIsPrefix, iPayloadType, pPayload, iPayloadSize, seiOutput);
}

void AddHDRMetaData(AL_TBuffer* pBufStream)
{
  if(AL_Buffer_GetMetaData(pBufStream, AL_META_TYPE_HDR))
    return;

  auto pHDReta = AL_HDRMetaData_Create();

  if(pHDReta)
    AL_Buffer_AddMetaData(pBufStream, (AL_TMetaData*)pHDReta);
}

static int sConfigureDecBufPool(PixMapBufPool& SrcBufPool, AL_TPicFormat tPicFormat, AL_TDimension tDim, int iPitchY)
{
  auto const tFourCC = AL_GetDecFourCC(tPicFormat);
  SrcBufPool.SetFormat(tDim, tFourCC);

  std::vector<AL_TPlaneDescription> vPlaneDesc;
  int iOffset = 0;

  AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES];
  int iNbPlanes = AL_Plane_GetBufferPixelPlanes(tPicFormat.eChromaOrder, usedPlanes);

  for(int iPlane = 0; iPlane < iNbPlanes; iPlane++)
  {
    int iPitch = usedPlanes[iPlane] == AL_PLANE_Y ? iPitchY : AL_GetChromaPitch(tFourCC, iPitchY);
    vPlaneDesc.push_back(AL_TPlaneDescription { usedPlanes[iPlane], iOffset, iPitch });
    iOffset += AL_DecGetAllocSize_Frame_PixPlane(tPicFormat.eStorageMode, tDim, iPitch, tPicFormat.eChromaMode, usedPlanes[iPlane]);

    if(g_MultiChunk)
    {
      SrcBufPool.AddChunk(iOffset, vPlaneDesc);
      vPlaneDesc.clear();
      iOffset = 0;
    }
  }

  if(!g_MultiChunk)
    SrcBufPool.AddChunk(iOffset, vPlaneDesc);

  return iOffset;
}

static AL_ERR sResolutionFound(int BufferNumber, int BufferSizeLib, AL_TStreamSettings const* pSettings, AL_TCropInfo const* pCropInfo, void* pUserParam)
{
  ResChgParam* p = (ResChgParam*)pUserParam;
  AL_TDecSettings* pDecSettings = p->pDecSettings;

  unique_lock<mutex> lock(p->hMutex);

  if(!p->hDec)
    return AL_ERROR;

  bool bMainOutputCompression;
  AL_e_FbStorageMode eMainOutputStorageMode = getMainOutputStorageMode(*pDecSettings, bMainOutputCompression);

  auto tPicFormat = AL_GetDecPicFormat(pSettings->eChroma, pSettings->iBitDepth, eMainOutputStorageMode, bMainOutputCompression);
  auto tFourCC = AL_GetDecFourCC(tPicFormat);

  int minPitch = AL_Decoder_GetMinPitch(pSettings->tDim.iWidth, pSettings->iBitDepth, eMainOutputStorageMode);

  /* get size for print */
  int BufferSize = 0;

  if(p->bPoolIsInit)
    BufferSize = AL_DecGetAllocSize_Frame(pSettings->tDim, minPitch, pSettings->eChroma, bMainOutputCompression, eMainOutputStorageMode);
  else
    BufferSize = sConfigureDecBufPool(p->bufPool, tPicFormat, pSettings->tDim, minPitch);

  assert(BufferSize >= BufferSizeLib);

  showStreamInfo(BufferNumber, BufferSize, pSettings, pCropInfo, tFourCC);

  /* stream resolution change */
  if(p->bPoolIsInit)
    return AL_SUCCESS;

  int iNumBuf = BufferNumber + uDefaultNumBuffersHeldByNextComponent;

  if(!p->bufPool.Init(p->pAllocator, iNumBuf, "decoded picture buffer"))
    return AL_ERR_NO_MEMORY;

  p->bPoolIsInit = true;

  for(int i = 0; i < iNumBuf; ++i)
  {
    auto pDecPict = p->bufPool.GetBuffer(AL_BUF_MODE_NONBLOCK);
    assert(pDecPict);
    AL_Buffer_MemSet(pDecPict, 0xDE);

    if(p->bAddHDRMetaData)
      AddHDRMetaData(pDecPict);
    AL_Decoder_PutDisplayPicture(p->hDec, pDecPict);
    AL_Buffer_Unref(pDecPict);
  }

  return AL_SUCCESS;
}

/******************************************************************************/

void ShowStatistics(double durationInSeconds, int iNumFrameConceal, int decodedFrameNumber, bool timeoutOccured)
{
  string guard = "Decoded time = ";

  if(timeoutOccured)
    guard = "TIMEOUT = ";

  auto msg = guard + "%.4f s;  Decoding FrameRate ~ %.4f Fps; Frame(s) conceal = %d\n";
  LogInfo(msg.c_str(),
          durationInSeconds,
          decodedFrameNumber / durationInSeconds,
          iNumFrameConceal);
}

/******************************************************************************/
struct AsyncFileInput
{
  AsyncFileInput(AL_HDecoder hDec_, string path, BufPool& bufPool_, bool bSplitInput, AL_ECodec eCodec, bool bVclSplit)
    : hDec(hDec_), bufPool(bufPool_)
  {
    exit = false;
    OpenInput(ifFileStream, path);

    if(bSplitInput)
      m_Loader.reset(new SplitInput(bufPool.m_pool.zBufSize, eCodec, bVclSplit));
    else
      m_Loader.reset(new BasicLoader());

    m_thread = thread(&AsyncFileInput::run, this);
  }

  ~AsyncFileInput()
  {
    exit = true;
    m_thread.join();
  }

private:
  void run()
  {
    Rtos_SetCurrentThreadName("FileInput");

    while(!exit)
    {
      shared_ptr<AL_TBuffer> pBufStream;
      try
      {
        pBufStream = shared_ptr<AL_TBuffer>(
          bufPool.GetBuffer(),
          &AL_Buffer_Unref);
      }
      catch(bufpool_decommited_error &)
      {
        continue;
      }

      auto uAvailSize = m_Loader->ReadStream(ifFileStream, pBufStream.get());

      if(!uAvailSize)
      {
        // end of input
        AL_Decoder_Flush(hDec);
        break;
      }

      auto bRet = AL_Decoder_PushBuffer(hDec, pBufStream.get(), uAvailSize);

      if(!bRet)
        throw runtime_error("Failed to push buffer");
    }
  }

  const AL_HDecoder hDec;
  ifstream ifFileStream;
  BufPool& bufPool;
  atomic<bool> exit;
  std::unique_ptr<InputLoader> m_Loader;
  thread m_thread;
};

constexpr int MAX_CHANNELS = 1;

int GetChannelsArgv(vector<char*>* argvChannels, int argc, char** argv)
{
  int curChan = 0;

  for(int i = 0; i < argc; ++i)
  {
    if(string(argv[i]) == "--next-chan")
    {
      ++curChan;

      if(curChan >= MAX_CHANNELS)
        throw runtime_error("Too many channels");

      argvChannels[curChan].push_back(argv[0]);
      continue;
    }

    argvChannels[curChan].push_back(argv[i]);
  }

  return curChan;
}

struct WorkerConfig
{
  Config* pConfig;
  CIpDevice* pIpDevice;
  bool bUseBoard;
};

void SafeChannelMain(WorkerConfig& w)
{
  auto pAllocator = w.pIpDevice->m_pAllocator.get();
  auto pScheduler = w.pIpDevice->m_pScheduler;
  auto& Config = *w.pConfig;
  auto pIpDevice = w.pIpDevice;
  bool bUseBoard = w.bUseBoard;

  ofstream seiOutput;
  ofstream seiSyncOutput;

  if(!Config.seiFile.empty())
  {
    OpenOutput(seiOutput, Config.seiFile);

    if(Config.tDecSettings.eInputMode == AL_DEC_SPLIT_INPUT)
      OpenOutput(seiSyncOutput, Config.seiFile + "_sync.txt");
  }

  BufPool bufPool;

  {
    AL_TBufPoolConfig BufPoolConfig {};
    BufPoolConfig.debugName = "stream";
    BufPoolConfig.zBufSize = Config.zInputBufferSize;
    BufPoolConfig.uNumBuf = Config.uInputBufferNum;
    AL_TMetaData* meta = nullptr;
    auto pBufPoolAllocator = AL_GetDefaultAllocator();

    if(Config.tDecSettings.eInputMode == AL_DEC_SPLIT_INPUT)
    {
      meta = (AL_TMetaData*)AL_StreamMetaData_Create(1);
      pBufPoolAllocator = pIpDevice->m_pAllocator.get();
    }

    BufPoolConfig.pMetaData = meta;
    auto ret = bufPool.Init(pBufPoolAllocator, BufPoolConfig);

    if(!ret)
      throw runtime_error("Can't create BufPool");
  }

  Display display;

  bool bMainOutputCompression;
  display.eMainOutputStorageMode = getMainOutputStorageMode(Config.tDecSettings, bMainOutputCompression);

  bool bHasOutput = Config.bEnableYUVOutput || bCertCRC || !Config.sCrc.empty();
  const AL_ECodec eCodec = Config.tDecSettings.eCodec;

  if(bHasOutput)
  {
    const string sCertCrcFile = bCertCRC ? "crc_certif_res.hex" : "";

    display.AddOutputWriter(display.eMainOutputStorageMode, bMainOutputCompression, Config.sMainOut, Config.sCrc, sCertCrcFile, eCodec);

  }

  display.iBitDepth = Config.tDecSettings.iBitDepth;
  display.MaxFrames = Config.iMaxFrames;

  if(!Config.hdrFile.empty())
    display.pHDRWriter = shared_ptr<HDRWriter>(new HDRWriter(Config.hdrFile));

  AL_TDecSettings Settings = Config.tDecSettings;

  ResChgParam ResolutionFoundParam;
  ResolutionFoundParam.pAllocator = pAllocator;
  ResolutionFoundParam.bPoolIsInit = false;
  ResolutionFoundParam.pDecSettings = &Settings;
  ResolutionFoundParam.bAddHDRMetaData = display.pHDRWriter != nullptr;

  DecodeParam tDecodeParam {};
  tDecodeParam.hExitMain = display.hExitMain;
  tDecodeParam.seiSyncOutput = &seiSyncOutput;

  AL_TDecCallBacks CB {};
  CB.endParsingCB = { &sInputParsed, nullptr };
  CB.endDecodingCB = { &sFrameDecoded, &tDecodeParam };
  CB.displayCB = { &sFrameDisplay, &display };
  CB.resolutionFoundCB = { &sResolutionFound, &ResolutionFoundParam };
  CB.parsedSeiCB = { &sParsedSei, (void*)&seiOutput };

  Settings.iBitDepth = HW_IP_BIT_DEPTH;

  AL_HDecoder hDec;
  auto error = AL_Decoder_Create(&hDec, (AL_IDecScheduler*)pScheduler, pAllocator, &Settings, &CB);

  if(AL_IS_ERROR_CODE(error))
    throw codec_error(error);

  assert(hDec);

  auto decoderAlreadyDestroyed = false;
  auto scopeDecoder = scopeExit([&]() {
    if(!decoderAlreadyDestroyed)
      AL_Decoder_Destroy(hDec);
  });

  // Param of Display Callback assignment
  display.hDec = hDec;
  tDecodeParam.hDec = hDec;
  ResolutionFoundParam.hDec = hDec;

  AL_Decoder_SetParam(hDec, bUseBoard ? "Fpga" : "Ref", Config.iNumTrace, Config.iNumberTrace, Config.bForceCleanBuffers, Config.ipCtrlMode == IPCTRL_MODE_TRACE);

  if(!invalidPreallocSettings(Config.tDecSettings.tStream))
  {
    if(!AL_Decoder_PreallocateBuffers(hDec))
      if(auto eErr = AL_Decoder_GetLastError(hDec))
        throw codec_error(eErr);
  }

  // Initial stream buffer filling
  auto const uBegin = GetPerfTime();
  bool timeoutOccured = false;

  for(int iLoop = 0; iLoop < Config.iLoop; ++iLoop)
  {
    bufPool.Commit();

    if(iLoop > 0)
      LogVerbose(CC_GREY, "  Looping\n");

    AsyncFileInput producer(hDec, Config.sIn, bufPool, Config.tDecSettings.eInputMode == AL_DEC_SPLIT_INPUT, eCodec, Config.tDecSettings.eDecUnit == AL_VCL_NAL_UNIT);

    auto const maxWait = Config.iTimeoutInSeconds * 1000;
    auto const timeout = maxWait >= 0 ? maxWait : AL_WAIT_FOREVER;

    if(!Rtos_WaitEvent(display.hExitMain, timeout))
    {
      timeoutOccured = true;
    }
    bufPool.Decommit();
  }

  auto const uEnd = GetPerfTime();

  unique_lock<mutex> lock(display.hMutex);
  auto eErr = AL_Decoder_GetLastError(hDec);

  if(AL_IS_ERROR_CODE(eErr))
    throw codec_error(eErr);

  if(!tDecodeParam.decodedFrames)
    throw runtime_error("No frame decoded");

  auto const duration = (uEnd - uBegin) / 1000.0;
  ShowStatistics(duration, display.iNumFrameConceal, tDecodeParam.decodedFrames, timeoutOccured);
}

static void ChannelMain(WorkerConfig& w, std::exception_ptr& exception)
{
  try
  {
    SafeChannelMain(w);
    exception = nullptr;
    return;
  }
  catch(codec_error const& error)
  {
    exception = std::current_exception();
  }
  catch(runtime_error const& error)
  {
    exception = std::current_exception();
  }
}

/******************************************************************************/
void SafeMain(int argc, char** argv)
{
  InitializePlateform();

  vector<char*> argvChannels[MAX_CHANNELS] {};
  int const maxChan = GetChannelsArgv(argvChannels, argc, argv);

  Config cfgChannels[MAX_CHANNELS];
  std::exception_ptr errorChannels[MAX_CHANNELS] {};
  WorkerConfig workerConfigs[MAX_CHANNELS];
  std::thread worker[MAX_CHANNELS];

  for(int chan = 0; chan <= maxChan; ++chan)
    cfgChannels[chan] = ParseCommandLine(argvChannels[chan].size(), argvChannels[chan].data());

  // Use first channel to configure the ip device
  auto Config = cfgChannels[0];

  if(Config.help)
    return;

  DisplayVersionInfo();

  // IP Device ------------------------------------------------------------

  function<AL_TIpCtrl* (AL_TIpCtrl*)> wrapIpCtrl;
  switch(Config.ipCtrlMode)
  {
  default:
    wrapIpCtrl = [](AL_TIpCtrl* ipCtrl) -> AL_TIpCtrl*
                 {
                   return ipCtrl;
                 };
    break;
  }

  CIpDeviceParam param;
  param.iSchedulerType = Config.iSchedulerType;
  param.iDeviceType = Config.iDeviceType;
  param.bTrackDma = Config.trackDma;
  param.uNumCore = Config.tDecSettings.uNumCore;
  param.iHangers = Config.hangers;
  auto pIpDevice = CreateIpDevice(param, wrapIpCtrl);

  bool bUseBoard = (param.iDeviceType == DEVICE_TYPE_BOARD); // retrieve auto-detected device type

  if(!pIpDevice)
    throw runtime_error("Can't create IpDevice");

  // mono channel case
  if(maxChan == 0)
  {
    WorkerConfig w
    {
      &cfgChannels[maxChan],
      pIpDevice.get(),
      bUseBoard,
    };

    workerConfigs[maxChan] = w;
    ChannelMain(workerConfigs[maxChan], errorChannels[maxChan]);

    if(errorChannels[maxChan])
      std::rethrow_exception(errorChannels[maxChan]);
  }
  else
  {
    throw std::runtime_error("Local multichannel isn't supported in this configuration");
  }
}

template<typename T, typename U, typename V>
V clip_it(V value, T min, U max)
{
  if(value < min)
    return min;

  if(value > max)
    return max;
  return value;
}

/******************************************************************************/

int main(int argc, char** argv)
{
  try
  {
    SafeMain(argc, argv);
    return 0;
  }
  catch(codec_error const& error)
  {
    cerr << endl << "Codec error: " << error.what() << endl;
    return error.Code;
  }
  catch(runtime_error const& error)
  {
    cerr << endl << "Exception caught: " << error.what() << endl;
    return 1;
  }
}

/******************************************************************************/

