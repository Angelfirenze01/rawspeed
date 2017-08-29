/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2010 Klaus Post
    Copyright (C) 2014-2015 Pedro Côrte-Real

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "decoders/SrwDecoder.h"
#include "common/Common.h"                       // for uint32, ushort16
#include "common/Point.h"                        // for iPoint2D
#include "decoders/RawDecoderException.h"        // for ThrowRDE
#include "decompressors/SamsungV0Decompressor.h" // for SamsungV0Decompressor
#include "decompressors/SamsungV1Decompressor.h" // for SamsungV1Decompressor
#include "io/BitPumpMSB32.h"                     // for BitPumpMSB32
#include "metadata/Camera.h"                     // for Hints
#include "metadata/CameraMetaData.h"             // for CameraMetaData
#include "tiff/TiffEntry.h"                      // for TiffEntry
#include "tiff/TiffIFD.h"                        // for TiffRootIFD, TiffIFD
#include "tiff/TiffTag.h"                        // for TiffTag::STRIPOFFSETS
#include <algorithm>                             // for max
#include <cassert>                               // for assert
#include <memory>                                // for unique_ptr
#include <sstream>                               // for operator<<, ostring...
#include <string>                                // for string, operator==
#include <vector>                                // for vector

using std::max;
using std::vector;
using std::string;
using std::ostringstream;

namespace rawspeed {

bool SrwDecoder::isAppropriateDecoder(const TiffRootIFD* rootIFD,
                                      const Buffer* file) {
  const auto id = rootIFD->getID();
  const std::string& make = id.make;

  // FIXME: magic

  return make == "SAMSUNG";
}

RawImage SrwDecoder::decodeRawInternal() {
  auto raw = mRootIFD->getIFDWithTag(STRIPOFFSETS);

  int compression = raw->getEntry(COMPRESSION)->getU32();
  const int bits = raw->getEntry(BITSPERSAMPLE)->getU32();

  if (12 != bits && 14 != bits)
    ThrowRDE("Unsupported bits per sample");

  if (32769 != compression && 32770 != compression && 32772 != compression && 32773 != compression)
    ThrowRDE("Unsupported compression");

  if (32769 == compression)
  {
    bool bit_order = hints.get("msb_override", false);
    this->decodeUncompressed(raw, bit_order ? BitOrder_MSB : BitOrder_LSB);
    return mRaw;
  }

  if (32770 == compression)
  {
    if (!raw->hasEntry(static_cast<TiffTag>(40976))) {
      bool bit_order = hints.get("msb_override", bits == 12);
      this->decodeUncompressed(raw, bit_order ? BitOrder_MSB : BitOrder_LSB);
      return mRaw;
    }
    uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
    if (nslices != 1)
      ThrowRDE("Only one slice supported, found %u", nslices);

    decodeCompressed(raw);
    return mRaw;
  }
  if (32772 == compression)
  {
    uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
    if (nslices != 1)
      ThrowRDE("Only one slice supported, found %u", nslices);

    decodeCompressed2(raw, bits);
    return mRaw;
  }
  if (32773 == compression)
  {
    decodeCompressed3(raw, bits);
    return mRaw;
  }
  ThrowRDE("Unsupported compression");
}

// Decoder for compressed srw files (NX300 and later)
void SrwDecoder::decodeCompressed( const TiffIFD* raw )
{
  SamsungV0Decompressor s0(mRaw, raw, mFile);
  s0.decompress();
}

// Decoder for compressed srw files (NX3000 and later)
void SrwDecoder::decodeCompressed2( const TiffIFD* raw, int bits)
{
  SamsungV1Decompressor s1(mRaw, raw, mFile, bits);
  s1.decompress();
}

// Decoder for third generation compressed SRW files (NX1)
// Seriously Samsung just use lossless jpeg already, it compresses better too :)

// Thanks to Michael Reichmann (Luminous Landscape) for putting me in contact
// and Loring von Palleske (Samsung) for pointing to the open-source code of
// Samsung's DNG converter at http://opensource.samsung.com/
void SrwDecoder::decodeCompressed3(const TiffIFD* raw, int bits)
{
  uint32 offset = raw->getEntry(STRIPOFFSETS)->getU32();
  BitPumpMSB32 startpump(mFile, offset);

  // Process the initial metadata bits, we only really use initVal, width and
  // height (the last two match the TIFF values anyway)
  startpump.getBits(16); // NLCVersion
  startpump.getBits(4);  // ImgFormat
  uint32 bitDepth = startpump.getBits(4)+1;
  startpump.getBits(4);  // NumBlkInRCUnit
  startpump.getBits(4);  // CompressionRatio
  uint32 width    = startpump.getBits(16);
  uint32 height    = startpump.getBits(16);
  startpump.getBits(16); // TileWidth
  startpump.getBits(4);  // reserved

  // The format includes an optimization code that sets 3 flags to change the
  // decoding parameters
  uint32 optflags = startpump.getBits(4);

#define OPT_SKIP 1 // Skip checking if we need differences from previous line
#define OPT_MV 2   // Simplify motion vector definition
#define OPT_QP 4   // Don't scale the diff values

  startpump.getBits(8);  // OverlapWidth
  startpump.getBits(8);  // reserved
  startpump.getBits(8);  // Inc
  startpump.getBits(2);  // reserved
  uint32 initVal = startpump.getBits(14);

  if (width == 0 || height == 0 || width % 16 != 0 || width > 6496 ||
      height > 4336)
    ThrowRDE("Unexpected image dimensions found: (%u; %u)", width, height);

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  // The format is relatively straightforward. Each line gets encoded as a set
  // of differences from pixels from another line. Pixels are grouped in blocks
  // of 16 (8 green, 8 red or blue). Each block is encoded in three sections.
  // First 1 or 4 bits to specify which reference pixels to use, then a section
  // that specifies for each pixel the number of bits in the difference, then
  // the actual difference bits
  uint32 diffBitsMode[3][2] = {{0}};
  uint32 line_offset = startpump.getBufferPosition();
  for (uint32 row=0; row < height; row++) {
    // Align pump to 16byte boundary
    if ((line_offset & 0xf) != 0)
      line_offset += 16 - (line_offset & 0xf);
    BitPumpMSB32 pump(mFile, offset+line_offset);

    auto* img = reinterpret_cast<ushort16*>(mRaw->getData(0, row));
    ushort16* img_up = reinterpret_cast<ushort16*>(
        mRaw->getData(0, max(0, static_cast<int>(row) - 1)));
    ushort16* img_up2 = reinterpret_cast<ushort16*>(
        mRaw->getData(0, max(0, static_cast<int>(row) - 2)));
    // Initialize the motion and diff modes at the start of the line
    uint32 motion = 7;
    // By default we are not scaling values at all
    int32 scale = 0;
    for (auto &i : diffBitsMode)
      i[0] = i[1] = (row == 0 || row == 1) ? 7 : 4;

    assert(width >= 16);
    for (uint32 col=0; col < width; col += 16) {
      if (!(optflags & OPT_QP) && !(col & 63)) {
        int32 scalevals[] = {0,-2,2};
        uint32 i = pump.getBits(2);
        scale = i < 3 ? scale+scalevals[i] : pump.getBits(12);
      }

      // First we figure out which reference pixels mode we're in
      if (optflags & OPT_MV)
        motion = pump.getBits(1) ? 3 : 7;
      else if (!pump.getBits(1))
        motion = pump.getBits(3);
      if ((row==0 || row==1) && (motion != 7))
        ThrowRDE("At start of image and motion isn't 7. File corrupted?");
      if (motion == 7) {
        // The base case, just set all pixels to the previous ones on the same line
        // If we're at the left edge we just start at the initial value
        for (uint32 i = 0; i < 16; i++)
          img[i] = (col == 0) ? initVal : *(img+i-2);
      } else {
        // The complex case, we now need to actually lookup one or two lines above
        if (row < 2)
          ThrowRDE(
              "Got a previous line lookup on first two lines. File corrupted?");
        int32 motionOffset[7] =    {-4,-2,-2,0,0,2,4};
        int32 motionDoAverage[7] = { 0, 0, 1,0,1,0,0};

        int32 slideOffset = motionOffset[motion];
        int32 doAverage = motionDoAverage[motion];

        for (uint32 i=0; i<16; i++) {
          ushort16* refpixel;
          if ((row+i) & 0x1) // Red or blue pixels use same color two lines up
            refpixel = img_up2 + i + slideOffset;
          else // Green pixel N uses Green pixel N from row above (top left or top right)
            refpixel = img_up + i + slideOffset + (((i % 2) != 0) ? -1 : 1);

          // In some cases we use as reference interpolation of this pixel and the next
          if (doAverage)
            img[i] = (*refpixel + *(refpixel+2) + 1) >> 1;
          else
            img[i] = *refpixel;
        }
      }

      // Figure out how many difference bits we have to read for each pixel
      uint32 diffBits[4] = {0};
      if (optflags & OPT_SKIP || !pump.getBits(1)) {
        uint32 flags[4];
        for (unsigned int &flag : flags)
          flag = pump.getBits(2);
        for (uint32 i=0; i<4; i++) {
          // The color is 0-Green 1-Blue 2-Red
          uint32 colornum = (row % 2 != 0) ? i>>1 : ((i>>1)+2) % 3;
          assert(flags[i] <= 3);
          switch (flags[i]) {
          case 0:
            diffBits[i] = diffBitsMode[colornum][0];
            break;
          case 1:
            diffBits[i] = diffBitsMode[colornum][0] + 1;
            break;
          case 2:
            diffBits[i] = diffBitsMode[colornum][0] - 1;
            break;
          case 3:
            diffBits[i] = pump.getBits(4);
            break;
          default:
            __builtin_unreachable();
          }
          diffBitsMode[colornum][0] = diffBitsMode[colornum][1];
          diffBitsMode[colornum][1] = diffBits[i];
          if(diffBits[i] > bitDepth+1)
            ThrowRDE("Too many difference bits. File corrupted?");
        }
      }

      // Actually read the differences and write them to the pixels
      for (uint32 i=0; i<16; i++) {
        uint32 len = diffBits[i>>2];
        int32 diff = pump.getBits(len);

        // If the first bit is 1 we need to turn this into a negative number
        if (len != 0 && diff >> (len - 1))
          diff -= (1 << len);

        ushort16 *value = nullptr;
        // Apply the diff to pixels 0 2 4 6 8 10 12 14 1 3 5 7 9 11 13 15
        if (row % 2)
          value = &img[((i&0x7)<<1)+1-(i>>3)];
        else
          value = &img[((i&0x7)<<1)+(i>>3)];

        diff = diff * (scale*2+1) + scale;
        *value = clampBits(static_cast<int>(*value) + diff, bits);
      }

      img += 16;
      img_up += 16;
      img_up2 += 16;
    }
    line_offset += pump.getBufferPosition();
  }
}

string SrwDecoder::getMode() {
  vector<const TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);
  ostringstream mode;
  if (!data.empty() && data[0]->hasEntryRecursive(BITSPERSAMPLE)) {
    mode << data[0]->getEntryRecursive(BITSPERSAMPLE)->getU32() << "bit";
    return mode.str();
  }
  return "";
}

void SrwDecoder::checkSupportInternal(const CameraMetaData* meta) {
  auto id = mRootIFD->getID();
  string mode = getMode();
  if (meta->hasCamera(id.make, id.model, mode))
    this->checkCameraSupported(meta, id, getMode());
  else
    this->checkCameraSupported(meta, id, "");
}

void SrwDecoder::decodeMetaDataInternal(const CameraMetaData* meta) {
  int iso = 0;
  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getU32();

  auto id = mRootIFD->getID();
  string mode = getMode();
  if (meta->hasCamera(id.make, id.model, mode))
    setMetaData(meta, id, mode, iso);
  else
    setMetaData(meta, id, "", iso);

  // Set the whitebalance
  if (mRootIFD->hasEntryRecursive(SAMSUNG_WB_RGGBLEVELSUNCORRECTED) &&
      mRootIFD->hasEntryRecursive(SAMSUNG_WB_RGGBLEVELSBLACK)) {
    TiffEntry *wb_levels = mRootIFD->getEntryRecursive(SAMSUNG_WB_RGGBLEVELSUNCORRECTED);
    TiffEntry *wb_black = mRootIFD->getEntryRecursive(SAMSUNG_WB_RGGBLEVELSBLACK);
    if (wb_levels->count == 4 && wb_black->count == 4) {
      mRaw->metadata.wbCoeffs[0] = wb_levels->getFloat(0) - wb_black->getFloat(0);
      mRaw->metadata.wbCoeffs[1] = wb_levels->getFloat(1) - wb_black->getFloat(1);
      mRaw->metadata.wbCoeffs[2] = wb_levels->getFloat(3) - wb_black->getFloat(3);
    }
  }
}

} // namespace rawspeed
