/*
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* ZBAL is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
* See the Mulan PSL v2 for more details.
*/
#ifndef FUSED_DEEP_MOE_EPILOGUE_TILE_TILE_STRIDE_MULS_H
#define FUSED_DEEP_MOE_EPILOGUE_TILE_TILE_STRIDE_MULS_H
#include "catlass/catlass.hpp"

namespace Catlass::Epilogue::Tile {

template<class ArchTag_, class ElementCompute_, class TileShape_, class DstTileShape_, class SrcTileShape_>
struct TileStrideMuls {
    using ArchTag = ArchTag_;
    using ElementCompute = ElementCompute_;
    using TileShape = TileShape_;
    using DstTileShape = DstTileShape_;
    using SrcTileShape = SrcTileShape_;

    static_assert(DstTileShape::ROW == SrcTileShape::ROW && DstTileShape::ROW == TileShape::ROW, "Error");

    CATLASS_DEVICE
    TileStrideMuls() {}

    CATLASS_DEVICE
    void operator()(AscendC::LocalTensor<ElementCompute> const &ubDst,
                    AscendC::LocalTensor<ElementCompute> const &ubSrc, ElementCompute scalar)
    {
        constexpr uint32_t maxRepeatTimes = 255;
        constexpr uint32_t eleNumPerBlk = BYTE_PER_BLK / sizeof(ElementCompute);

        constexpr uint32_t dstBlkNumPerColumn = DstTileShape::COLUMN / eleNumPerBlk;
        constexpr uint32_t srcBlkNumPerColumn = SrcTileShape::COLUMN / eleNumPerBlk;
        AscendC::UnaryRepeatParams repeatParams;
        repeatParams.dstBlkStride = 1;
        repeatParams.srcBlkStride = 1;
        repeatParams.dstRepStride = dstBlkNumPerColumn;
        repeatParams.srcRepStride = srcBlkNumPerColumn;

        constexpr uint32_t rowNumPerCompute = maxRepeatTimes;
        constexpr uint32_t colNumPerCompute = BYTE_PER_VECTOR_FRACTAL / sizeof(ElementCompute);
        for (uint32_t rowOffset = 0; rowOffset < TileShape::ROW; rowOffset += rowNumPerCompute) {
            uint32_t residueM = TileShape::ROW - rowOffset;
            uint8_t repeatTimes = static_cast<uint8_t>((residueM > rowNumPerCompute) ? rowNumPerCompute : residueM);
            for (uint32_t colOffset = 0; colOffset < TileShape::COLUMN; colOffset += colNumPerCompute) {
                uint32_t residueN = TileShape::COLUMN - colOffset;
                uint64_t mask = (residueN > colNumPerCompute) ? colNumPerCompute : residueN;
                AscendC::Muls(ubDst[rowOffset * DstTileShape::COLUMN + colOffset],
                              ubSrc[rowOffset * SrcTileShape::COLUMN + colOffset], scalar, mask, repeatTimes,
                              repeatParams);
            }
        }
    }
};

} // namespace Catlass::Epilogue::Tile

#endif // FUSED_DEEP_MOE_EPILOGUE_TILE_TILE_STRIDE_MULS_H
