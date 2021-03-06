// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "base/logging.h"
#include "gfx_es2/gl_state.h"

#include "Common/ChunkFile.h"

#include "Core/Debugger/Breakpoints.h"
#include "Core/MemMap.h"
#include "Core/Host.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "Core/System.h"

#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/GeDisasm.h"

#include "GPU/GLES/ShaderManager.h"
#include "GPU/GLES/GLES_GPU.h"
#include "GPU/GLES/Framebuffer.h"
#include "GPU/GLES/TransformPipeline.h"
#include "GPU/GLES/TextureCache.h"

#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceGe.h"

enum {
	FLAG_FLUSHBEFORE = 1,
	FLAG_FLUSHBEFOREONCHANGE = 2,
	FLAG_EXECUTE = 4,  // needs to actually be executed. unused for now.
	FLAG_EXECUTEONCHANGE = 8,  // unused for now. not sure if checking for this will be more expensive than doing it.
	FLAG_ANY_EXECUTE = 4 | 8,
};

struct CommandTableEntry {
	u8 cmd;
	u8 flags;
};

static const CommandTableEntry commandTable[] = {
	// Changes that dirty the framebuffer
	{GE_CMD_FRAMEBUFPTR, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},
	{GE_CMD_FRAMEBUFWIDTH, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},
	{GE_CMD_FRAMEBUFPIXFORMAT, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},
	{GE_CMD_ZBUFPTR, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_ZBUFWIDTH, FLAG_FLUSHBEFOREONCHANGE},

	// Changes that dirty uniforms
	{GE_CMD_FOGCOLOR, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_FOG1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_FOG2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Should these maybe flush?
	{GE_CMD_MINZ, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_MAXZ, FLAG_FLUSHBEFOREONCHANGE},

	// Changes that dirty texture scaling.
	{GE_CMD_TEXMAPMODE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSCALEU, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSCALEV, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXOFFSETU, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXOFFSETV, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Changes that dirty the current texture. Really should be possible to avoid executing these if we compile
	// by adding some more flags.
	{GE_CMD_TEXSIZE0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE4, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE5, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE6, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSIZE7, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXFORMAT, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR4, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR5, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR6, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXADDR7, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH4, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH5, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH6, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXBUFWIDTH7, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_CLUTADDR, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},
	{GE_CMD_CLUTADDRUPPER, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},
	{GE_CMD_CLUTFORMAT, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},

	// These affect the fragment shader so need flushing.
	{GE_CMD_CLEARMODE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_TEXTUREMAPENABLE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_FOGENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_TEXMODE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXSHADELS, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_SHADEMODE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_TEXFUNC, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_COLORTEST, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_ALPHATESTENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_COLORTESTENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_COLORTESTMASK, FLAG_FLUSHBEFOREONCHANGE},

	// These change the vertex shader so need flushing.
	{GE_CMD_REVERSENORMAL, FLAG_FLUSHBEFOREONCHANGE},  // TODO: This one is actually processed during vertex decoding which is wrong.
	{GE_CMD_LIGHTINGENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTENABLE0, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTENABLE1, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTENABLE2, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTENABLE3, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTTYPE0, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTTYPE1, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTTYPE2, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LIGHTTYPE3, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_MATERIALUPDATE, FLAG_FLUSHBEFOREONCHANGE},

	// This changes both shaders so need flushing.
	{GE_CMD_LIGHTMODE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_TEXFILTER, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXWRAP, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Uniform changes
	{GE_CMD_ALPHATEST, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_COLORREF, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_TEXENVCOLOR, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Simple render state changes. Handled in StateMapping.cpp.
	{GE_CMD_OFFSETX, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_OFFSETY, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_CULL, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_CULLFACEENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_DITHERENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_STENCILOP, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_STENCILTEST, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_STENCILTESTENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_ALPHABLENDENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_BLENDMODE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_BLENDFIXEDA, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_BLENDFIXEDB, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_MASKRGB, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_MASKALPHA, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_ZTEST, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_ZTESTENABLE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_ZWRITEDISABLE, FLAG_FLUSHBEFOREONCHANGE},
#ifndef USING_GLES2
	{GE_CMD_LOGICOP, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_LOGICOPENABLE, FLAG_FLUSHBEFOREONCHANGE},
#else
	{GE_CMD_LOGICOP, 0},
	{GE_CMD_LOGICOPENABLE, 0},
#endif

	// Can probably ignore this one as we don't support AA lines.
	{GE_CMD_ANTIALIASENABLE, FLAG_FLUSHBEFOREONCHANGE},
	
	// Morph weights. TODO: Remove precomputation?
	{GE_CMD_MORPHWEIGHT0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT4, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT5, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT6, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MORPHWEIGHT7, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Control spline/bezier patches. Don't really require flushing as such, but meh.
	{GE_CMD_PATCHDIVISION, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_PATCHPRIMITIVE, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_PATCHFACING, FLAG_FLUSHBEFOREONCHANGE},
	{GE_CMD_PATCHCULLENABLE, FLAG_FLUSHBEFOREONCHANGE},

	// Viewport.
	{GE_CMD_VIEWPORTX1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_VIEWPORTY1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_VIEWPORTX2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_VIEWPORTY2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_VIEWPORTZ1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_VIEWPORTZ2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Region
	{GE_CMD_REGION1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_REGION2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Scissor
	{GE_CMD_SCISSOR1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_SCISSOR2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// These dirty various vertex shader uniforms. Could embed information about that in this table and call dirtyuniform directly, hm...
	{GE_CMD_AMBIENTCOLOR, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_AMBIENTALPHA, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MATERIALDIFFUSE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MATERIALEMISSIVE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MATERIALAMBIENT, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MATERIALALPHA, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MATERIALSPECULAR, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_MATERIALSPECULARCOEF, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// These precompute a value. not sure if worth it. Also dirty uniforms, which could be table-ized to avoid execute.
	{GE_CMD_LX0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LY0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LZ0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LX1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LY1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LZ1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LX2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LY2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LZ2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LX3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LY3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LZ3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	{GE_CMD_LDX0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDY0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDZ0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDX1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDY1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDZ1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDX2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDY2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDZ2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDX3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDY3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDZ3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	{GE_CMD_LKA0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKB0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKC0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKA1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKB1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKC1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKA2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKB2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKC2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKA3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKB3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKC3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	{GE_CMD_LKS0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKS1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKS2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKS3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	{GE_CMD_LKO0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKO1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKO2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LKO3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	{GE_CMD_LAC0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDC0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LSC0, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LAC1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDC1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LSC1, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LAC2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDC2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LSC2, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LAC3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LDC3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},
	{GE_CMD_LSC3, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	// Ignored commands
	{GE_CMD_CLIPENABLE, 0},
	{GE_CMD_TEXFLUSH, 0},
	{GE_CMD_TEXLODSLOPE, 0},
	{GE_CMD_TEXLEVEL, FLAG_EXECUTEONCHANGE},  // we don't support this anyway, no need to flush.
	{GE_CMD_TEXSYNC, 0},

	// These are just nop or part of other later commands.
	{GE_CMD_NOP, 0},
	{GE_CMD_BASE, 0},
	{GE_CMD_TRANSFERSRC, 0},
	{GE_CMD_TRANSFERSRCW, 0},
	{GE_CMD_TRANSFERDST, 0},
	{GE_CMD_TRANSFERDSTW, 0},
	{GE_CMD_TRANSFERSRCPOS, 0},
	{GE_CMD_TRANSFERDSTPOS, 0},
	{GE_CMD_TRANSFERSIZE, 0},

	// From Common. No flushing but definitely need execute.
	{GE_CMD_OFFSETADDR, FLAG_EXECUTE},
	{GE_CMD_ORIGIN, FLAG_EXECUTE},  // Really?
	{GE_CMD_PRIM, FLAG_EXECUTE},
	{GE_CMD_JUMP, FLAG_EXECUTE},
	{GE_CMD_CALL, FLAG_EXECUTE},
	{GE_CMD_RET, FLAG_EXECUTE},
	{GE_CMD_END, FLAG_EXECUTE},  // Flush?
	{GE_CMD_VADDR, FLAG_EXECUTE},
	{GE_CMD_IADDR, FLAG_EXECUTE},
	{GE_CMD_BJUMP, FLAG_EXECUTE},  // EXECUTE
	{GE_CMD_BOUNDINGBOX, FLAG_EXECUTE}, // + FLUSHBEFORE when we implement

	// Changing the vertex type requires us to flush.
	{GE_CMD_VERTEXTYPE, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTEONCHANGE},

	{GE_CMD_BEZIER, FLAG_FLUSHBEFORE | FLAG_EXECUTE},
	{GE_CMD_SPLINE, FLAG_FLUSHBEFORE | FLAG_EXECUTE},

	// These two are actually processed in CMD_END.
	{GE_CMD_SIGNAL, FLAG_FLUSHBEFORE},
	{GE_CMD_FINISH, FLAG_FLUSHBEFORE},

	// Changes that trigger data copies. Only flushing on change for LOADCLUT must be a bit of a hack...
	{GE_CMD_LOADCLUT, FLAG_FLUSHBEFOREONCHANGE | FLAG_EXECUTE},
	{GE_CMD_TRANSFERSTART, FLAG_FLUSHBEFORE | FLAG_EXECUTE},

	// We don't use the dither table.
	{GE_CMD_DITH0},
	{GE_CMD_DITH1},
	{GE_CMD_DITH2},
	{GE_CMD_DITH3},

	// These handle their own flushing.
	{GE_CMD_WORLDMATRIXNUMBER, FLAG_EXECUTE},
	{GE_CMD_WORLDMATRIXDATA,   FLAG_EXECUTE},
	{GE_CMD_VIEWMATRIXNUMBER,  FLAG_EXECUTE},
	{GE_CMD_VIEWMATRIXDATA,    FLAG_EXECUTE},
	{GE_CMD_PROJMATRIXNUMBER,  FLAG_EXECUTE},
	{GE_CMD_PROJMATRIXDATA,    FLAG_EXECUTE},
	{GE_CMD_TGENMATRIXNUMBER,  FLAG_EXECUTE},
	{GE_CMD_TGENMATRIXDATA,    FLAG_EXECUTE},
	{GE_CMD_BONEMATRIXNUMBER,  FLAG_EXECUTE},
	{GE_CMD_BONEMATRIXDATA,    FLAG_EXECUTE},

	// "Missing" commands (gaps in the sequence)
	{GE_CMD_UNKNOWN_03, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_0D, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_11, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_29, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_34, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_35, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_39, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_4E, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_4F, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_52, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_59, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_5A, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_B6, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_B7, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_D1, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_ED, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_EF, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F0, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F1, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F2, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F3, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F4, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F5, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F6, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F7, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F8, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_F9, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_FA, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_FB, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_FC, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_FD, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_FE, FLAG_EXECUTE},
	{GE_CMD_UNKNOWN_FF, FLAG_EXECUTE},
};


GLES_GPU::GLES_GPU()
: resized_(false) {
#ifdef _WIN32
	lastVsync_ = g_Config.bVSync ? 1 : 0;
	if (gl_extensions.EXT_swap_control_tear) {
		// See http://developer.download.nvidia.com/opengl/specs/WGL_EXT_swap_control_tear.txt
		glstate.SetVSyncInterval(g_Config.bVSync ? -1 :0);
	} else {
		glstate.SetVSyncInterval(g_Config.bVSync ? 1 : 0);
	}
#endif

	shaderManager_ = new ShaderManager();
	transformDraw_.SetShaderManager(shaderManager_);
	transformDraw_.SetTextureCache(&textureCache_);
	transformDraw_.SetFramebufferManager(&framebufferManager_);
	framebufferManager_.SetTextureCache(&textureCache_);
	framebufferManager_.SetShaderManager(shaderManager_);
	textureCache_.SetFramebufferManager(&framebufferManager_);

	// Sanity check gstate
	if ((int *)&gstate.transferstart - (int *)&gstate != 0xEA) {
		ERROR_LOG(G3D, "gstate has drifted out of sync!");
	}

	// Sanity check commandFlags table - no dupes please
	std::set<u8> dupeCheck;
	commandFlags_ = new u8[256];
	memset(commandFlags_, 0, 256 * sizeof(bool));
	for (size_t i = 0; i < ARRAY_SIZE(commandTable); i++) {
		u8 cmd = commandTable[i].cmd;
		if (dupeCheck.find(cmd) != dupeCheck.end()) {
			ERROR_LOG(G3D, "Command table Dupe: %02x (%i)", (int)cmd, (int)cmd);
		} else {
			dupeCheck.insert(cmd);
		}
		commandFlags_[cmd] |= commandTable[i].flags;
	}
	// Find commands missing from the table.
	for (int i = 0; i < 0xEF; i++) {
		if (dupeCheck.find((u8)i) == dupeCheck.end()) {
			ERROR_LOG(G3D, "Command missing from table: %02x (%i)", i, i);
		}
	}

	// No need to flush before the tex scale/offset commands if we are baking
	// the tex scale/offset into the vertices anyway.

	if (g_Config.bPrescaleUV) {
		commandFlags_[GE_CMD_TEXSCALEU] &= ~FLAG_FLUSHBEFOREONCHANGE;
		commandFlags_[GE_CMD_TEXSCALEV] &= ~FLAG_FLUSHBEFOREONCHANGE;
		commandFlags_[GE_CMD_TEXOFFSETU] &= ~FLAG_FLUSHBEFOREONCHANGE;
		commandFlags_[GE_CMD_TEXOFFSETV] &= ~FLAG_FLUSHBEFOREONCHANGE;
	}

	if (g_Config.bSoftwareSkinning) {
		commandFlags_[GE_CMD_VERTEXTYPE] &= ~FLAG_FLUSHBEFOREONCHANGE;
	}

	BuildReportingInfo();
}

GLES_GPU::~GLES_GPU() {
	framebufferManager_.DestroyAllFBOs();
	shaderManager_->ClearCache(true);
	delete shaderManager_;
	delete [] commandFlags_;
}

// Let's avoid passing nulls into snprintf().
static const char *GetGLStringAlways(GLenum name) {
	const GLubyte *value = glGetString(name);
	if (!value)
		return "?";
	return (const char *)value;
}

// Needs to be called on GPU thread, not reporting thread.
void GLES_GPU::BuildReportingInfo() {
	const char *glVendor = GetGLStringAlways(GL_VENDOR);
	const char *glRenderer = GetGLStringAlways(GL_RENDERER);
	const char *glVersion = GetGLStringAlways(GL_VERSION);
	const char *glSlVersion = GetGLStringAlways(GL_SHADING_LANGUAGE_VERSION);
	const char *glExtensions = GetGLStringAlways(GL_EXTENSIONS);

	char temp[16384];
	snprintf(temp, sizeof(temp), "%s (%s %s), %s (extensions: %s)", glVersion, glVendor, glRenderer, glSlVersion, glExtensions);
	reportingPrimaryInfo_ = glVendor;
	reportingFullInfo_ = temp;
}

void GLES_GPU::DeviceLost() {
	ILOG("GLES_GPU: DeviceLost");
	// Should only be executed on the GL thread.

	// Simply drop all caches and textures.
	// FBOs appear to survive? Or no?
	// TransformDraw has registered as a GfxResourceHolder.
	shaderManager_->ClearCache(false);
	textureCache_.Clear(false);
	framebufferManager_.DeviceLost();
}

void GLES_GPU::InitClear() {
	ScheduleEvent(GPU_EVENT_INIT_CLEAR);
}

void GLES_GPU::InitClearInternal() {
	bool useNonBufferedRendering = g_Config.iRenderingMode == FB_NON_BUFFERED_MODE;
	if (useNonBufferedRendering) {
		glstate.depthWrite.set(GL_TRUE);
		glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0,0,0,1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}
	glstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
}

void GLES_GPU::DumpNextFrame() {
	dumpNextFrame_ = true;
}

void GLES_GPU::BeginFrame() {
	ScheduleEvent(GPU_EVENT_BEGIN_FRAME);
}

void GLES_GPU::BeginFrameInternal() {
#ifdef _WIN32
	// Turn off vsync when unthrottled
	int desiredVSyncInterval = g_Config.bVSync ? 1 : 0;
	if ((PSP_CoreParameter().unthrottle) || (PSP_CoreParameter().fpsLimit == 1))
		desiredVSyncInterval = 0;
	if (desiredVSyncInterval != lastVsync_) {
		// Disabled EXT_swap_control_tear for now, it never seems to settle at the correct timing
		// so it just keeps tearing. Not what I hoped for...
		if (false && gl_extensions.EXT_swap_control_tear) {
			// See http://developer.download.nvidia.com/opengl/specs/WGL_EXT_swap_control_tear.txt
			glstate.SetVSyncInterval(-desiredVSyncInterval);
		} else {
			glstate.SetVSyncInterval(desiredVSyncInterval);
		}
		lastVsync_ = desiredVSyncInterval;
	}
#endif

	textureCache_.StartFrame();
	transformDraw_.DecimateTrackedVertexArrays();

	if (dumpNextFrame_) {
		NOTICE_LOG(G3D, "DUMPING THIS FRAME");
		dumpThisFrame_ = true;
		dumpNextFrame_ = false;
	} else if (dumpThisFrame_) {
		dumpThisFrame_ = false;
	}
	shaderManager_->DirtyShader();

	// Not sure if this is really needed.
	shaderManager_->DirtyUniform(DIRTY_ALL);

	framebufferManager_.BeginFrame();
}

void GLES_GPU::SetDisplayFramebuffer(u32 framebuf, u32 stride, GEBufferFormat format) {
	host->GPUNotifyDisplay(framebuf, stride, format);
	framebufferManager_.SetDisplayFramebuffer(framebuf, stride, format);
}

bool GLES_GPU::FramebufferDirty() {
	if (g_Config.bSeparateCPUThread) {
		// Allow it to process fully before deciding if it's dirty.
		SyncThread();
	}

	VirtualFramebuffer *vfb = framebufferManager_.GetDisplayVFB();
	if (vfb) {
		bool dirty = vfb->dirtyAfterDisplay;
		vfb->dirtyAfterDisplay = false;
		return dirty;
	}
	return true;
}

bool GLES_GPU::FramebufferReallyDirty() {
	if (g_Config.bSeparateCPUThread) {
		// Allow it to process fully before deciding if it's dirty.
		SyncThread();
	}

	VirtualFramebuffer *vfb = framebufferManager_.GetDisplayVFB();
	if (vfb) {
		bool dirty = vfb->reallyDirtyAfterDisplay;
		vfb->reallyDirtyAfterDisplay = false;
		return dirty;
	}
	return true;
}

void GLES_GPU::CopyDisplayToOutput() {
	ScheduleEvent(GPU_EVENT_COPY_DISPLAY_TO_OUTPUT);
}

void GLES_GPU::CopyDisplayToOutputInternal() {
	glstate.depthWrite.set(GL_TRUE);
	glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	transformDraw_.Flush();

	framebufferManager_.CopyDisplayToOutput();
	framebufferManager_.EndFrame();

	shaderManager_->DirtyLastShader();

	// If buffered, discard the depth buffer of the backbuffer. Don't even know if we need one.
#if 0
#ifdef USING_GLES2
	if (gl_extensions.EXT_discard_framebuffer && g_Config.iRenderingMode != 0) {
		GLenum attachments[] = {GL_DEPTH_EXT, GL_STENCIL_EXT};
		glDiscardFramebufferEXT(GL_FRAMEBUFFER, 2, attachments);
	}
#endif
#endif

	gstate_c.textureChanged = true;
}

// Maybe should write this in ASM...
void GLES_GPU::FastRunLoop(DisplayList &list) {
	const u8 *commandFlags = commandFlags_;
	for (; downcount > 0; --downcount) {
		// We know that display list PCs have the upper nibble == 0 - no need to mask the pointer
		const u32 op = *(const u32 *)(Memory::base + list.pc);
		const u32 cmd = op >> 24;
		const u8 cmdFlags = commandFlags[cmd];      // If we stashed the cmdFlags in the top bits of the cmdmem, we could get away with one table lookup instead of two
		const u32 diff = op ^ gstate.cmdmem[cmd];
		// Inlined CheckFlushOp here to get rid of the dumpThisFrame_ check.
		if ((cmdFlags & FLAG_FLUSHBEFORE) || (diff && (cmdFlags & FLAG_FLUSHBEFOREONCHANGE))) {
			transformDraw_.Flush();
		}
		gstate.cmdmem[cmd] = op;  // TODO: no need to write if diff==0...
		if (cmdFlags & FLAG_ANY_EXECUTE) {  // (cmdFlags & FLAG_EXECUTE) || (diff && (cmdFlags & FLAG_EXECUTEONCHANGE))) {
			ExecuteOpInternal(op, diff);
		}
		list.pc += 4;
	}
}

void GLES_GPU::ProcessEvent(GPUEvent ev) {
	switch (ev.type) {
	case GPU_EVENT_INIT_CLEAR:
		InitClearInternal();
		break;

	case GPU_EVENT_BEGIN_FRAME:
		BeginFrameInternal();
		break;

	case GPU_EVENT_COPY_DISPLAY_TO_OUTPUT:
		CopyDisplayToOutputInternal();
		break;

	case GPU_EVENT_INVALIDATE_CACHE:
		InvalidateCacheInternal(ev.invalidate_cache.addr, ev.invalidate_cache.size, ev.invalidate_cache.type);
		break;

	default:
		GPUCommon::ProcessEvent(ev);
	}
}

inline void GLES_GPU::CheckFlushOp(int cmd, u32 diff) {
	u8 cmdFlags = commandFlags_[cmd];
	if ((cmdFlags & FLAG_FLUSHBEFORE) || (diff && (cmdFlags & FLAG_FLUSHBEFOREONCHANGE))) {
		if (dumpThisFrame_) {
			NOTICE_LOG(G3D, "================ FLUSH ================");
		}
		transformDraw_.Flush();
	}
}

void GLES_GPU::PreExecuteOp(u32 op, u32 diff) {
	CheckFlushOp(op >> 24, diff);
}

void GLES_GPU::ExecuteOp(u32 op, u32 diff) {
	return ExecuteOpInternal(op, diff);
}

void GLES_GPU::ExecuteOpInternal(u32 op, u32 diff) {
	u32 cmd = op >> 24;
	u32 data = op & 0xFFFFFF;

	// Handle control and drawing commands here directly. The others we delegate.
	switch (cmd) {
	case GE_CMD_BASE:
		break;

	case GE_CMD_VADDR:
		gstate_c.vertexAddr = gstate_c.getRelativeAddress(data);
		break;

	case GE_CMD_IADDR:
		gstate_c.indexAddr	= gstate_c.getRelativeAddress(data);
		break;

	case GE_CMD_PRIM:
		{
			// This drives all drawing. All other state we just buffer up, then we apply it only
			// when it's time to draw. As most PSP games set state redundantly ALL THE TIME, this is a huge optimization.

			u32 count = data & 0xFFFF;
			GEPrimitiveType prim = static_cast<GEPrimitiveType>(data >> 16);
			
			if (count == 0)
				break;
				
			// Discard AA lines as we can't do anything that makes sense with these anyway. The SW plugin might, though.

			if (gstate.isAntiAliasEnabled()) {
				// Discard AA lines in DOA
				if (prim == GE_PRIM_LINE_STRIP)
					break;
				// Discard AA lines in Summon Night 5 
				if ((prim == GE_PRIM_LINES) && gstate.isSkinningEnabled())
					break;
			}

			// This also make skipping drawing very effective.
			framebufferManager_.SetRenderFrameBuffer();
			if (gstate_c.skipDrawReason & (SKIPDRAW_SKIPFRAME | SKIPDRAW_NON_DISPLAYED_FB))	{
				transformDraw_.SetupVertexDecoder(gstate.vertType);
				// Rough estimate, not sure what's correct.
				int vertexCost = transformDraw_.EstimatePerVertexCost();
				cyclesExecuted += vertexCost * count;
				return;
			}

			if (!Memory::IsValidAddress(gstate_c.vertexAddr)) {
				ERROR_LOG_REPORT(G3D, "Bad vertex address %08x!", gstate_c.vertexAddr);
				break;
			}

			// TODO: Split this so that we can collect sequences of primitives, can greatly speed things up
			// on platforms where draw calls are expensive like mobile and D3D
			void *verts = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
			void *inds = 0;
			if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
				if (!Memory::IsValidAddress(gstate_c.indexAddr)) {
					ERROR_LOG_REPORT(G3D, "Bad index address %08x!", gstate_c.indexAddr);
					break;
				}
				inds = Memory::GetPointerUnchecked(gstate_c.indexAddr);
			}

#ifndef USING_GLES2
			if (prim > GE_PRIM_RECTANGLES) {
				ERROR_LOG_REPORT_ONCE(reportPrim, G3D, "Unexpected prim type: %d", prim);
			}
#endif

			int bytesRead;
			transformDraw_.SubmitPrim(verts, inds, prim, count, gstate.vertType, &bytesRead);

			int vertexCost = transformDraw_.EstimatePerVertexCost();
			gpuStats.vertexGPUCycles += vertexCost * count;
			cyclesExecuted += vertexCost * count;

			// After drawing, we advance the vertexAddr (when non indexed) or indexAddr (when indexed).
			// Some games rely on this, they don't bother reloading VADDR and IADDR.
			// Q: Are these changed reflected in the real registers? Needs testing.
			if (inds) {
				int indexSize = 1;
				if ((gstate.vertType & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_16BIT)
					indexSize = 2;
				gstate_c.indexAddr += count * indexSize;
			} else {
				gstate_c.vertexAddr += bytesRead;
			}
		}
		break;

	// The arrow and other rotary items in Puzbob are bezier patches, strangely enough.
	case GE_CMD_BEZIER:
		{
			// This also make skipping drawing very effective.
			framebufferManager_.SetRenderFrameBuffer();
			if (gstate_c.skipDrawReason & (SKIPDRAW_SKIPFRAME | SKIPDRAW_NON_DISPLAYED_FB))	{
				// TODO: Should this eat some cycles?  Probably yes.  Not sure if important.
				return;
			}

			if (!Memory::IsValidAddress(gstate_c.vertexAddr)) {
				ERROR_LOG_REPORT(G3D, "Bad vertex address %08x!", gstate_c.vertexAddr);
				break;
			}

			void *control_points = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
			void *indices = NULL;
			if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
				if (!Memory::IsValidAddress(gstate_c.indexAddr)) {
					ERROR_LOG_REPORT(G3D, "Bad index address %08x!", gstate_c.indexAddr);
					break;
				}
				indices = Memory::GetPointerUnchecked(gstate_c.indexAddr);
			}

			if (gstate.getPatchPrimitiveType() != GE_PATCHPRIM_TRIANGLES) {
				ERROR_LOG_REPORT(G3D, "Unsupported patch primitive %x", gstate.getPatchPrimitiveType());
				break;
			}

			if (gstate.vertType & GE_VTYPE_MORPHCOUNT_MASK) {
				DEBUG_LOG_REPORT(G3D, "Bezier + morph: %i", (gstate.vertType & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT);
			}
			if (vertTypeIsSkinningEnabled(gstate.vertType)) {
				DEBUG_LOG_REPORT(G3D, "Bezier + skinning: %i", vertTypeGetNumBoneWeights(gstate.vertType));
			}

			GEPatchPrimType patchPrim = gstate.getPatchPrimitiveType();
			int bz_ucount = data & 0xFF;
			int bz_vcount = (data >> 8) & 0xFF;
			transformDraw_.SubmitBezier(control_points, indices, bz_ucount, bz_vcount, patchPrim, gstate.vertType);
		}
		break;

	case GE_CMD_SPLINE:
		{
			// This also make skipping drawing very effective.
			framebufferManager_.SetRenderFrameBuffer();
			if (gstate_c.skipDrawReason & (SKIPDRAW_SKIPFRAME | SKIPDRAW_NON_DISPLAYED_FB))	{
				// TODO: Should this eat some cycles?  Probably yes.  Not sure if important.
				return;
			}

			if (!Memory::IsValidAddress(gstate_c.vertexAddr)) {
				ERROR_LOG_REPORT(G3D, "Bad vertex address %08x!", gstate_c.vertexAddr);
				break;
			}

			void *control_points = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
			void *indices = NULL;
			if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
				if (!Memory::IsValidAddress(gstate_c.indexAddr)) {
					ERROR_LOG_REPORT(G3D, "Bad index address %08x!", gstate_c.indexAddr);
					break;
				}
				indices = Memory::GetPointerUnchecked(gstate_c.indexAddr);
			}

			if (gstate.getPatchPrimitiveType() != GE_PATCHPRIM_TRIANGLES) {
				ERROR_LOG_REPORT(G3D, "Unsupported patch primitive %x", gstate.getPatchPrimitiveType());
				break;
			}

			if (gstate.vertType & GE_VTYPE_MORPHCOUNT_MASK) {
				DEBUG_LOG_REPORT(G3D, "Spline + morph: %i", (gstate.vertType & GE_VTYPE_MORPHCOUNT_MASK) >> GE_VTYPE_MORPHCOUNT_SHIFT);
			}
			if (vertTypeIsSkinningEnabled(gstate.vertType)) {
				DEBUG_LOG_REPORT(G3D, "Spline + skinning: %i", vertTypeGetNumBoneWeights(gstate.vertType));
			}

			int sp_ucount = data & 0xFF;
			int sp_vcount = (data >> 8) & 0xFF;
			int sp_utype = (data >> 16) & 0x3;
			int sp_vtype = (data >> 18) & 0x3;
			GEPatchPrimType patchPrim = gstate.getPatchPrimitiveType();
			transformDraw_.SubmitSpline(control_points, indices, sp_ucount, sp_vcount, sp_utype, sp_vtype, patchPrim, gstate.vertType);
		}
		break;

	case GE_CMD_BOUNDINGBOX:
		// Just resetting, nothing to bound.
		if (data == 0) {
			// TODO: Should this set the bboxResult?  Let's set it true for now.
			currentList->bboxResult = true;
			break;
		}
		if (((data & 7) == 0) && data <= 64) {  // Sanity check
			void *control_points = Memory::GetPointer(gstate_c.vertexAddr);
			if (gstate.vertType & GE_VTYPE_IDX_MASK) {
				ERROR_LOG_REPORT_ONCE(boundingbox, G3D, "Indexed bounding box data not supported.");
				// Data seems invalid. Let's assume the box test passed.
				currentList->bboxResult = true;
				break;
			}

			// Test if the bounding box is within the drawing region.
			currentList->bboxResult = transformDraw_.TestBoundingBox(control_points, data, gstate.vertType);
		} else {
			ERROR_LOG_REPORT_ONCE(boundingbox, G3D, "Bad bounding box data: %06x", data);
			// Data seems invalid. Let's assume the box test passed.
			currentList->bboxResult = true;
		}
		break;

	case GE_CMD_VERTEXTYPE:
		if (diff) {
			if (!g_Config.bSoftwareSkinning) {
				if (diff & (GE_VTYPE_TC_MASK | GE_VTYPE_THROUGH_MASK))
					shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
			} else {
				if (diff & ~GE_VTYPE_WEIGHTCOUNT_MASK) {
					// Restore and flush
					gstate.vertType ^= diff;
					Flush();
					gstate.vertType ^= diff;
					if (diff & (GE_VTYPE_TC_MASK | GE_VTYPE_THROUGH_MASK))
						shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
				}
			}
		}
		break;

	case GE_CMD_REGION1:
	case GE_CMD_REGION2:
		if (diff) {
			gstate_c.framebufChanged = true;
			gstate_c.textureChanged = true;
		}
		break;

	case GE_CMD_CLIPENABLE:
		//we always clip, this is opengl
		break;

	case GE_CMD_CULLFACEENABLE:
	case GE_CMD_CULL:
		break;

	case GE_CMD_TEXTUREMAPENABLE:
		if (diff)
			gstate_c.textureChanged = true;
		break;

	case GE_CMD_LIGHTINGENABLE:
		break;

	case GE_CMD_FOGCOLOR:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_FOGCOLOR);
		break;

	case GE_CMD_FOG1:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_FOGCOEF);
		break;

	case GE_CMD_FOG2:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_FOGCOEF);
		break;

	case GE_CMD_FOGENABLE:
		break;

	case GE_CMD_DITHERENABLE:
		break;

	case GE_CMD_OFFSETX:
		break;

	case GE_CMD_OFFSETY:
		break;

	case GE_CMD_TEXSCALEU:
		if (diff) {
			gstate_c.uv.uScale = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
		}
		break;

	case GE_CMD_TEXSCALEV:
		if (diff) {
			gstate_c.uv.vScale = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
		}
		break;

	case GE_CMD_TEXOFFSETU:
		if (diff) {
			gstate_c.uv.uOff = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
		}
		break;

	case GE_CMD_TEXOFFSETV:
		if (diff) {
			gstate_c.uv.vOff = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
		}
		break;

	case GE_CMD_SCISSOR1:
	case GE_CMD_SCISSOR2:
		if (diff) {
			gstate_c.framebufChanged = true;
			gstate_c.textureChanged = true;
		}
		break;

		///
	case GE_CMD_MINZ:
	case GE_CMD_MAXZ:
		break;

	case GE_CMD_FRAMEBUFPTR:
	case GE_CMD_FRAMEBUFWIDTH:
	case GE_CMD_FRAMEBUFPIXFORMAT:
		if (diff) {
			gstate_c.framebufChanged = true;
			gstate_c.textureChanged = true;
		}
		break;

	case GE_CMD_TEXADDR0:
	case GE_CMD_TEXADDR1:
	case GE_CMD_TEXADDR2:
	case GE_CMD_TEXADDR3:
	case GE_CMD_TEXADDR4:
	case GE_CMD_TEXADDR5:
	case GE_CMD_TEXADDR6:
	case GE_CMD_TEXADDR7:
		if (diff) {
			gstate_c.textureChanged = true;
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
		}
		break;

	case GE_CMD_TEXBUFWIDTH0:
	case GE_CMD_TEXBUFWIDTH1:
	case GE_CMD_TEXBUFWIDTH2:
	case GE_CMD_TEXBUFWIDTH3:
	case GE_CMD_TEXBUFWIDTH4:
	case GE_CMD_TEXBUFWIDTH5:
	case GE_CMD_TEXBUFWIDTH6:
	case GE_CMD_TEXBUFWIDTH7:
		if (diff) {
			gstate_c.textureChanged = true;
		}
		break;

	case GE_CMD_CLUTFORMAT:
		if (diff) {
			gstate_c.textureChanged = true;
		}
		// This could be used to "dirty" textures with clut.
		break;

	case GE_CMD_CLUTADDR:
	case GE_CMD_CLUTADDRUPPER:
		// Hm, LOADCLUT actually changes the CLUT so no need to dirty here.
		break;

	case GE_CMD_LOADCLUT:
		gstate_c.textureChanged = true;
		textureCache_.LoadClut();
		// This could be used to "dirty" textures with clut.
		break;

	case GE_CMD_TEXMAPMODE:
		if (diff) {
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
		}
		break;

	case GE_CMD_TEXSHADELS:
		break;

	case GE_CMD_TRANSFERSRC:
	case GE_CMD_TRANSFERSRCW:
	case GE_CMD_TRANSFERDST:
	case GE_CMD_TRANSFERDSTW:
	case GE_CMD_TRANSFERSRCPOS:
	case GE_CMD_TRANSFERDSTPOS:
		break;

	case GE_CMD_TRANSFERSIZE:
		break;

	case GE_CMD_TRANSFERSTART:  // Orphis calls this TRXKICK
		{
			// TODO: Here we should check if the transfer overlaps a framebuffer or any textures,
			// and take appropriate action. This is a block transfer between RAM and VRAM, or vice versa.
			// Can we skip this on SkipDraw?
			DoBlockTransfer();

			// Fixes Gran Turismo's funky text issue, since it overwrites the current texture.
			gstate_c.textureChanged = true;
			break;
		}

	case GE_CMD_TEXSIZE0:
		// Render to texture may have overridden the width/height.
		// Don't reset it unless the size is different / the texture has changed.
		if (diff || gstate_c.textureChanged) {
			gstate_c.curTextureWidth = gstate.getTextureWidth(0);
			gstate_c.curTextureHeight = gstate.getTextureHeight(0);
			shaderManager_->DirtyUniform(DIRTY_UVSCALEOFFSET);
			// We will need to reset the texture now.
			gstate_c.textureChanged = true;
		}
		//fall thru - ignoring the mipmap sizes for now

	case GE_CMD_TEXSIZE1:
	case GE_CMD_TEXSIZE2:
	case GE_CMD_TEXSIZE3:
	case GE_CMD_TEXSIZE4:
	case GE_CMD_TEXSIZE5:
	case GE_CMD_TEXSIZE6:
	case GE_CMD_TEXSIZE7:
		if (diff) {
			gstate_c.textureChanged = true;
		}
		break;

	case GE_CMD_ZBUFPTR:
	case GE_CMD_ZBUFWIDTH:
		break;

	case GE_CMD_AMBIENTCOLOR:
	case GE_CMD_AMBIENTALPHA:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_AMBIENT);
		break;

	case GE_CMD_MATERIALDIFFUSE:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_MATDIFFUSE);
		break;

	case GE_CMD_MATERIALEMISSIVE:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_MATEMISSIVE);
		break;

	case GE_CMD_MATERIALAMBIENT:
	case GE_CMD_MATERIALALPHA:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_MATAMBIENTALPHA);
		break;

	case GE_CMD_MATERIALSPECULAR:
	case GE_CMD_MATERIALSPECULARCOEF:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_MATSPECULAR);
		break;

	case GE_CMD_LIGHTTYPE0:
	case GE_CMD_LIGHTTYPE1:
	case GE_CMD_LIGHTTYPE2:
	case GE_CMD_LIGHTTYPE3:
		break;

	case GE_CMD_LX0:case GE_CMD_LY0:case GE_CMD_LZ0:
	case GE_CMD_LX1:case GE_CMD_LY1:case GE_CMD_LZ1:
	case GE_CMD_LX2:case GE_CMD_LY2:case GE_CMD_LZ2:
	case GE_CMD_LX3:case GE_CMD_LY3:case GE_CMD_LZ3:
		if (diff) {
			int n = cmd - GE_CMD_LX0;
			int l = n / 3;
			int c = n % 3;
			gstate_c.lightpos[l][c] = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_LIGHT0 << l);
		}
		break;

	case GE_CMD_LDX0:case GE_CMD_LDY0:case GE_CMD_LDZ0:
	case GE_CMD_LDX1:case GE_CMD_LDY1:case GE_CMD_LDZ1:
	case GE_CMD_LDX2:case GE_CMD_LDY2:case GE_CMD_LDZ2:
	case GE_CMD_LDX3:case GE_CMD_LDY3:case GE_CMD_LDZ3:
		if (diff) {
			int n = cmd - GE_CMD_LDX0;
			int l = n / 3;
			int c = n % 3;
			gstate_c.lightdir[l][c] = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_LIGHT0 << l);
		}
		break;

	case GE_CMD_LKA0:case GE_CMD_LKB0:case GE_CMD_LKC0:
	case GE_CMD_LKA1:case GE_CMD_LKB1:case GE_CMD_LKC1:
	case GE_CMD_LKA2:case GE_CMD_LKB2:case GE_CMD_LKC2:
	case GE_CMD_LKA3:case GE_CMD_LKB3:case GE_CMD_LKC3:
		if (diff)	{
			int n = cmd - GE_CMD_LKA0;
			int l = n / 3;
			int c = n % 3;
			gstate_c.lightatt[l][c] = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_LIGHT0 << l);
		}
		break;

	case GE_CMD_LKS0:
	case GE_CMD_LKS1:
	case GE_CMD_LKS2:
	case GE_CMD_LKS3:
		if (diff) {
			int l = cmd - GE_CMD_LKS0;
			gstate_c.lightspotCoef[l] = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_LIGHT0 << l);
		}
		break;

	case GE_CMD_LKO0:
	case GE_CMD_LKO1:
	case GE_CMD_LKO2:
	case GE_CMD_LKO3:
		if (diff) {
			int l = cmd - GE_CMD_LKO0;
			gstate_c.lightangle[l] = getFloat24(data);
			shaderManager_->DirtyUniform(DIRTY_LIGHT0 << l);
		}
		break;

	case GE_CMD_LAC0:case GE_CMD_LAC1:case GE_CMD_LAC2:case GE_CMD_LAC3:
	case GE_CMD_LDC0:case GE_CMD_LDC1:case GE_CMD_LDC2:case GE_CMD_LDC3:
	case GE_CMD_LSC0:case GE_CMD_LSC1:case GE_CMD_LSC2:case GE_CMD_LSC3:
		if (diff)	{
			float r = (float)(data & 0xff) * (1.0f / 255.0f);
			float g = (float)((data >> 8) & 0xff) * (1.0f / 255.0f);
			float b = (float)(data >> 16) * (1.0f / 255.0f);

			int l = (cmd - GE_CMD_LAC0) / 3;
			int t = (cmd - GE_CMD_LAC0) % 3;
			gstate_c.lightColor[t][l][0] = r;
			gstate_c.lightColor[t][l][1] = g;
			gstate_c.lightColor[t][l][2] = b;
			shaderManager_->DirtyUniform(DIRTY_LIGHT0 << l);
		}
		break;

	case GE_CMD_VIEWPORTX1:
	case GE_CMD_VIEWPORTY1:
	case GE_CMD_VIEWPORTX2:
	case GE_CMD_VIEWPORTY2:
	case GE_CMD_VIEWPORTZ1:
	case GE_CMD_VIEWPORTZ2:
		if (diff) {
			gstate_c.framebufChanged = true;
			gstate_c.textureChanged = true;
		}
		break;

	case GE_CMD_LIGHTENABLE0:
	case GE_CMD_LIGHTENABLE1:
	case GE_CMD_LIGHTENABLE2:
	case GE_CMD_LIGHTENABLE3:
		break;

	case GE_CMD_SHADEMODE:
		break;

	case GE_CMD_PATCHDIVISION:
	case GE_CMD_PATCHPRIMITIVE:
	case GE_CMD_PATCHFACING:
		break;


	case GE_CMD_MATERIALUPDATE:
		break;

	//////////////////////////////////////////////////////////////////
	//	CLEARING
	//////////////////////////////////////////////////////////////////
	case GE_CMD_CLEARMODE:
		break;

	//////////////////////////////////////////////////////////////////
	//	ALPHA BLENDING
	//////////////////////////////////////////////////////////////////
	case GE_CMD_ALPHABLENDENABLE:
	case GE_CMD_BLENDMODE:
	case GE_CMD_BLENDFIXEDA:
	case GE_CMD_BLENDFIXEDB:
		break;

	case GE_CMD_ALPHATESTENABLE:
	case GE_CMD_COLORTESTENABLE:
		// They are done in the fragment shader.
		break;

	case GE_CMD_COLORTEST:
	case GE_CMD_COLORTESTMASK:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_COLORMASK);
		break;

	case GE_CMD_ALPHATEST:
#ifndef USING_GLES2
		if (((data >> 16) & 0xFF) != 0xFF && (data & 7) > 1)
			WARN_LOG_REPORT_ONCE(alphatestmask, G3D, "Unsupported alphatest mask: %02x", (data >> 16) & 0xFF);
#endif
		// Intentional fallthrough. (?)

	case GE_CMD_COLORREF:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_ALPHACOLORREF);
		break;

	case GE_CMD_TEXENVCOLOR:
		if (diff)
			shaderManager_->DirtyUniform(DIRTY_TEXENV);
		break;

	case GE_CMD_TEXFUNC:
	case GE_CMD_TEXFLUSH:
		break;

	case GE_CMD_TEXMODE:
	case GE_CMD_TEXFORMAT:
	case GE_CMD_TEXFILTER:
	case GE_CMD_TEXWRAP:
		if (diff)
			gstate_c.textureChanged = true;
		break;

	//////////////////////////////////////////////////////////////////
	//	DEPTH TESTING
	//////////////////////////////////////////////////////////////////

	case GE_CMD_ZTESTENABLE:
	case GE_CMD_ZTEST:
	case GE_CMD_ZWRITEDISABLE:
		break;

	case GE_CMD_MORPHWEIGHT0:
	case GE_CMD_MORPHWEIGHT1:
	case GE_CMD_MORPHWEIGHT2:
	case GE_CMD_MORPHWEIGHT3:
	case GE_CMD_MORPHWEIGHT4:
	case GE_CMD_MORPHWEIGHT5:
	case GE_CMD_MORPHWEIGHT6:
	case GE_CMD_MORPHWEIGHT7:
		if (diff)
			gstate_c.morphWeights[cmd - GE_CMD_MORPHWEIGHT0] = getFloat24(data);
		break;

	case GE_CMD_DITH0:
	case GE_CMD_DITH1:
	case GE_CMD_DITH2:
	case GE_CMD_DITH3:
		break;

	case GE_CMD_WORLDMATRIXNUMBER:
		gstate.worldmtxnum &= 0xFF00000F;
		break;

	case GE_CMD_WORLDMATRIXDATA:
		{
			int num = gstate.worldmtxnum & 0xF;
			u32 newVal = data << 8;
			if (num < 12 && newVal != ((const u32 *)gstate.worldMatrix)[num]) {
				Flush();
				((u32 *)gstate.worldMatrix)[num] = newVal;
				shaderManager_->DirtyUniform(DIRTY_WORLDMATRIX);
			}
			num++;
			gstate.worldmtxnum = (GE_CMD_WORLDMATRIXNUMBER << 24) | (num & 0xF);
		}
		break;

	case GE_CMD_VIEWMATRIXNUMBER:
		gstate.viewmtxnum &= 0xFF00000F;
		break;

	case GE_CMD_VIEWMATRIXDATA:
		{
			int num = gstate.viewmtxnum & 0xF;
			u32 newVal = data << 8;
			if (num < 12 && newVal != ((const u32 *)gstate.viewMatrix)[num]) {
				Flush();
				((u32 *)gstate.viewMatrix)[num] = newVal;
				shaderManager_->DirtyUniform(DIRTY_VIEWMATRIX);
			}
			num++;
			gstate.viewmtxnum = (GE_CMD_VIEWMATRIXNUMBER << 24) | (num & 0xF);
		}
		break;

	case GE_CMD_PROJMATRIXNUMBER:
		gstate.projmtxnum &= 0xFF00000F;
		break;

	case GE_CMD_PROJMATRIXDATA:
		{
			int num = gstate.projmtxnum & 0xF;
			u32 newVal = data << 8;
			if (newVal != ((const u32 *)gstate.projMatrix)[num]) {
				Flush();
				((u32 *)gstate.projMatrix)[num] = newVal;
				shaderManager_->DirtyUniform(DIRTY_PROJMATRIX);
			}
			num++;
			gstate.projmtxnum = (GE_CMD_PROJMATRIXNUMBER << 24) | (num & 0xF);
		}
		break;

	case GE_CMD_TGENMATRIXNUMBER:
		gstate.texmtxnum &= 0xFF00000F;
		break;

	case GE_CMD_TGENMATRIXDATA:
		{
			int num = gstate.texmtxnum & 0xF;
			u32 newVal = data << 8;
			if (num < 12 && newVal != ((const u32 *)gstate.tgenMatrix)[num]) {
				Flush();
				((u32 *)gstate.tgenMatrix)[num] = newVal;
				shaderManager_->DirtyUniform(DIRTY_TEXMATRIX);
			}
			num++;
			gstate.texmtxnum = (GE_CMD_TGENMATRIXNUMBER << 24) | (num & 0xF);
		}
		break;

	case GE_CMD_BONEMATRIXNUMBER:
		gstate.boneMatrixNumber &= 0xFF00007F;
		break;

	case GE_CMD_BONEMATRIXDATA:
		{
			int num = gstate.boneMatrixNumber & 0x7F;
			u32 newVal = data << 8;
			if (num < 96 && newVal != ((const u32 *)gstate.boneMatrix)[num]) {
				// Bone matrices should NOT flush when software skinning is enabled!
				// TODO: Also check for morph...
				if (!g_Config.bSoftwareSkinning) {
					Flush();
					shaderManager_->DirtyUniform(DIRTY_BONEMATRIX0 << (num / 12));
				}
				((u32 *)gstate.boneMatrix)[num] = newVal;
			}
			num++;
			gstate.boneMatrixNumber = (GE_CMD_BONEMATRIXNUMBER << 24) | (num & 0x7F);
		}
		break;

#ifndef USING_GLES2
	case GE_CMD_ANTIALIASENABLE:
		if (data != 0)
			WARN_LOG_REPORT_ONCE(antiAlias, G3D, "Unsupported antialias enabled: %06x", data);
		break;

	case GE_CMD_TEXLODSLOPE:
		if (data != 0)
			WARN_LOG_REPORT_ONCE(texLodSlope, G3D, "Unsupported texture lod slope: %06x", data);
		break;

	case GE_CMD_TEXLEVEL:
		if (data == 1)
			WARN_LOG_REPORT_ONCE(texLevel1, G3D, "Unsupported texture level bias settings: %06x", data)
		else if (data != 0)
			WARN_LOG_REPORT_ONCE(texLevel2, G3D, "Unsupported texture level bias settings: %06x", data);
		if (diff)
			gstate_c.textureChanged = true;
		break;
#endif

	//////////////////////////////////////////////////////////////////
	//	STENCIL TESTING
	//////////////////////////////////////////////////////////////////

	case GE_CMD_STENCILTEST:
		// Handled in StateMapping.
		if (diff) {
			shaderManager_->DirtyUniform(DIRTY_STENCILREPLACEVALUE);
		}
		break;

	case GE_CMD_STENCILTESTENABLE:
	case GE_CMD_STENCILOP:
		break;

	case GE_CMD_MASKRGB:
	case GE_CMD_MASKALPHA:
		break;

	case GE_CMD_REVERSENORMAL:
		break;

	case GE_CMD_UNKNOWN_03: 
	case GE_CMD_UNKNOWN_0D:
	case GE_CMD_UNKNOWN_11:
	case GE_CMD_UNKNOWN_29:
	case GE_CMD_UNKNOWN_34:
	case GE_CMD_UNKNOWN_35:
	case GE_CMD_UNKNOWN_39:
	case GE_CMD_UNKNOWN_4E:
	case GE_CMD_UNKNOWN_4F:
	case GE_CMD_UNKNOWN_52:
	case GE_CMD_UNKNOWN_59:
	case GE_CMD_UNKNOWN_5A:
	case GE_CMD_UNKNOWN_B6:
	case GE_CMD_UNKNOWN_B7:
	case GE_CMD_UNKNOWN_D1:
	case GE_CMD_UNKNOWN_ED:
	case GE_CMD_UNKNOWN_EF:
	case GE_CMD_UNKNOWN_F0:
	case GE_CMD_UNKNOWN_F1:
	case GE_CMD_UNKNOWN_F2:
	case GE_CMD_UNKNOWN_F3:
	case GE_CMD_UNKNOWN_F4:
	case GE_CMD_UNKNOWN_F5:
	case GE_CMD_UNKNOWN_F6:
	case GE_CMD_UNKNOWN_F7:
	case GE_CMD_UNKNOWN_F8:
	case GE_CMD_UNKNOWN_F9:
	case GE_CMD_UNKNOWN_FA:
	case GE_CMD_UNKNOWN_FB:
	case GE_CMD_UNKNOWN_FC:
	case GE_CMD_UNKNOWN_FD:
	case GE_CMD_UNKNOWN_FE:
		if (data != 0)
			WARN_LOG_REPORT_ONCE(unknowncmd, G3D, "Unknown GE command : %08x ", op);
		break;
	case GE_CMD_UNKNOWN_FF:
		// This is hit in quite a few games, supposedly it is a no-op.
		// Might be used for debugging or something?
		break;
		
	default:
		GPUCommon::ExecuteOp(op, diff);
		break;
	}
}

void GLES_GPU::UpdateStats() {
	gpuStats.numVertexShaders = shaderManager_->NumVertexShaders();
	gpuStats.numFragmentShaders = shaderManager_->NumFragmentShaders();
	gpuStats.numShaders = shaderManager_->NumPrograms();
	gpuStats.numTextures = (int)textureCache_.NumLoadedTextures();
	gpuStats.numFBOs = (int)framebufferManager_.NumVFBs();
}

void GLES_GPU::DoBlockTransfer() {
	// TODO: This is used a lot to copy data around between render targets and textures,
	// and also to quickly load textures from RAM to VRAM. So we should do checks like the following:
	//  * Does dstBasePtr point to an existing texture? If so maybe reload it immediately.
	//
	//  * Does srcBasePtr point to a render target, and dstBasePtr to a texture? If so
	//    either copy between rt and texture or reassign the texture to point to the render target
	//
	// etc....

	u32 srcBasePtr = gstate.getTransferSrcAddress();
	u32 srcStride = gstate.getTransferSrcStride();

	u32 dstBasePtr = gstate.getTransferDstAddress();
	u32 dstStride = gstate.getTransferDstStride();

	int srcX = gstate.getTransferSrcX();
	int srcY = gstate.getTransferSrcY();

	int dstX = gstate.getTransferDstX();
	int dstY = gstate.getTransferDstY();

	int width = gstate.getTransferWidth();
	int height = gstate.getTransferHeight();

	int bpp = gstate.getTransferBpp();

	DEBUG_LOG(G3D, "Block transfer: %08x/%x -> %08x/%x, %ix%ix%i (%i,%i)->(%i,%i)", srcBasePtr, srcStride, dstBasePtr, dstStride, width, height, bpp, srcX, srcY, dstX, dstY);

	if (!Memory::IsValidAddress(srcBasePtr)) {
		ERROR_LOG_REPORT(G3D, "BlockTransfer: Bad source transfer address %08x!", srcBasePtr);
		return;
	}

	if (!Memory::IsValidAddress(dstBasePtr)) {
		ERROR_LOG_REPORT(G3D, "BlockTransfer: Bad destination transfer address %08x!", dstBasePtr);
		return;
	}
	
	// Check that the last address of both source and dest are valid addresses

	u32 srcLastAddr = srcBasePtr + ((height - 1 + srcY) * srcStride + (srcX + width - 1)) * bpp;
	u32 dstLastAddr = dstBasePtr + ((height - 1 + dstY) * dstStride + (dstX + width - 1)) * bpp;

	if (!Memory::IsValidAddress(srcLastAddr)) {
		ERROR_LOG_REPORT(G3D, "Bottom-right corner of source of block transfer is at an invalid address: %08x", srcLastAddr);
		return;
	}
	if (!Memory::IsValidAddress(dstLastAddr)) {
		ERROR_LOG_REPORT(G3D, "Bottom-right corner of destination of block transfer is at an invalid address: %08x", srcLastAddr);
		return;
	}

	// Do the copy! (Hm, if we detect a drawn video frame (see below) then we could maybe skip this?)
	// Can use GetPointerUnchecked because we checked the addresses above. We could also avoid them
	// entirely by walking a couple of pointers...
	// GetPointerUnchecked crash in windows 64 bit of issue 2301
	for (int y = 0; y < height; y++) {
		u32 srcLineStartAddr = srcBasePtr + ((y + srcY) * srcStride + srcX) * bpp;
		u32 dstLineStartAddr = dstBasePtr + ((y + dstY) * dstStride + dstX) * bpp;

		const u8 *src = Memory::GetPointerUnchecked(srcLineStartAddr);
		u8 *dst = Memory::GetPointerUnchecked(dstLineStartAddr);
		memcpy(dst, src, width * bpp);
	}

	// TODO: Notify all overlapping FBOs that they need to reload.
	framebufferManager_.NotifyBlockTransfer(dstBasePtr, srcBasePtr);

	textureCache_.Invalidate(dstBasePtr + (dstY * dstStride + dstX) * bpp, height * dstStride * bpp, GPU_INVALIDATE_HINT);
	if (Memory::IsRAMAddress(srcBasePtr) && Memory::IsVRAMAddress(dstBasePtr)) {
		// TODO: This causes glitches in Tactics Ogre if we don't implement both ways (which will probably be slow...)
		// The main thing this helps is videos, which will have a matching stride, and zero x/y.
		if (dstStride == srcStride && dstY == 0 && dstX == 0 && srcX == 0 && srcY == 0) {
			framebufferManager_.UpdateFromMemory(dstBasePtr, (dstY + height) * dstStride * bpp, true);
		}
	}
	
	// A few games use this INSTEAD of actually drawing the video image to the screen, they just blast it to
	// the backbuffer. Detect this and have the framebuffermanager draw the pixels.

	u32 backBuffer = framebufferManager_.PrevDisplayFramebufAddr();
	u32 displayBuffer = framebufferManager_.DisplayFramebufAddr();

	if (((backBuffer != 0 && dstBasePtr == backBuffer) ||
		  (displayBuffer != 0 && dstBasePtr == displayBuffer)) &&
			dstStride == 512 && height == 272) {
		framebufferManager_.DrawPixels(Memory::GetPointerUnchecked(dstBasePtr), GE_FORMAT_8888, 512);
	}

#ifndef USING_GLES2
	CBreakPoints::ExecMemCheck(srcBasePtr + (srcY * srcStride + srcX) * bpp, false, height * srcStride * bpp, currentMIPS->pc);
	CBreakPoints::ExecMemCheck(dstBasePtr + (srcY * dstStride + srcX) * bpp, true, height * dstStride * bpp, currentMIPS->pc);
#endif
}

void GLES_GPU::InvalidateCache(u32 addr, int size, GPUInvalidationType type) {
	GPUEvent ev(GPU_EVENT_INVALIDATE_CACHE);
	ev.invalidate_cache.addr = addr;
	ev.invalidate_cache.size = size;
	ev.invalidate_cache.type = type;
	ScheduleEvent(ev);
}

void GLES_GPU::InvalidateCacheInternal(u32 addr, int size, GPUInvalidationType type) {
	if (size > 0)
		textureCache_.Invalidate(addr, size, type);
	else
		textureCache_.InvalidateAll(type);

	if (type != GPU_INVALIDATE_ALL)
		framebufferManager_.UpdateFromMemory(addr, size, type == GPU_INVALIDATE_SAFE);
}

void GLES_GPU::UpdateMemory(u32 dest, u32 src, int size) {
	InvalidateCache(dest, size, GPU_INVALIDATE_HINT);

	// Track stray copies of a framebuffer in RAM. MotoGP does this.
	if (Memory::IsVRAMAddress(src) && Memory::IsRAMAddress(dest)) {
		framebufferManager_.NotifyFramebufferCopy(src, dest, size);
	}
}

void GLES_GPU::ClearCacheNextFrame() {
	textureCache_.ClearNextFrame();
}

void GLES_GPU::Resized() {
	framebufferManager_.Resized();
}

void GLES_GPU::ClearShaderCache() {
	shaderManager_->ClearCache(true);
}

std::vector<FramebufferInfo> GLES_GPU::GetFramebufferList() {
	return framebufferManager_.GetFramebufferList();
}

void GLES_GPU::DoState(PointerWrap &p) {
	GPUCommon::DoState(p);

	// TODO: Some of these things may not be necessary.
	// None of these are necessary when saving.
	// In Freeze-Frame mode, we don't want to do any of this.
	if (p.mode == p.MODE_READ && !PSP_CoreParameter().frozen) {
		textureCache_.Clear(true);
		transformDraw_.ClearTrackedVertexArrays();

		gstate_c.textureChanged = true;
		framebufferManager_.DestroyAllFBOs();
	}
}

bool GLES_GPU::GetCurrentFramebuffer(GPUDebugBuffer &buffer) {
	return framebufferManager_.GetCurrentFramebuffer(buffer);
}

bool GLES_GPU::GetCurrentDepthbuffer(GPUDebugBuffer &buffer) {
	return framebufferManager_.GetCurrentDepthbuffer(buffer);
}

bool GLES_GPU::GetCurrentStencilbuffer(GPUDebugBuffer &buffer) {
	return framebufferManager_.GetCurrentStencilbuffer(buffer);
}

bool GLES_GPU::GetCurrentTexture(GPUDebugBuffer &buffer) {
	if (!gstate.isTextureMapEnabled()) {
		return false;
	}

#ifndef USING_GLES2
	textureCache_.SetTexture(true);
	int w = gstate.getTextureWidth(0);
	int h = gstate.getTextureHeight(0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);

	buffer.Allocate(w, h, GE_FORMAT_8888, gstate_c.flipTexture);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer.GetData());

	return true;
#else
	return false;
#endif
}

bool GLES_GPU::GetCurrentSimpleVertices(int count, std::vector<GPUDebugVertex> &vertices, std::vector<u16> &indices) {
	return transformDraw_.GetCurrentSimpleVertices(count, vertices, indices);
}

bool GLES_GPU::DescribeCodePtr(const u8 *ptr, std::string &name) {
	if (transformDraw_.IsCodePtrVertexDecoder(ptr)) {
		name = "VertexDecoderJit";
		return true;
	}
	return false;
}
