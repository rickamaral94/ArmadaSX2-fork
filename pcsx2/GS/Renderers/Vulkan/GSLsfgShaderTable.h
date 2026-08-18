// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <map>
#include <string>

/// Which RCDATA resource of a Lossless.dll holds which shader.
///
/// Extracted into its own header because there are now two readers: the extractor in GSLsfg.cpp,
/// which pulls the blobs out at first use, and ForkLsfgPackage, which only checks — at import
/// time, with no GPU and no renderer — whether the file the user picked actually carries a
/// complete family. Two hand-kept copies of these numbers would diverge on the first Lossless
/// Scaling release that renumbers a resource, and the copy that diverges silently is the one that
/// tells the user their DLL is fine when it is not.
///
/// Nothing about Lossless Scaling ships here: these are offsets into the user's own file, the
/// same way a BIOS checksum table is not a BIOS.
namespace GSLsfgShaderTable
{
	/// True for the LSFG 3.1p (performance) family. The `p_` prefix is upstream's own naming.
	inline bool IsPerformanceShader(const std::string& name) { return name.compare(0, 2, "p_") == 0; }

	/// Shader name -> resource id. Only the names framegen asks for are listed; anything else
	/// fails the load cleanly rather than feeding it a wrong shader.
	///
	/// Two families: the plain names are LSFG 3.1 and the `p_` prefixed ones are 3.1p, the lighter
	/// pipeline. `p_mipmaps` and `p_generate` deliberately share 255/256 with 3.1 — those two
	/// resources are common to both, so they are extracted twice under the two names framegen asks
	/// for rather than special-cased.
	inline const std::map<std::string, u32>& Get()
	{
		static const std::map<std::string, u32> table = {
			{"mipmaps", 255},
			{"alpha[0]", 267}, {"alpha[1]", 268}, {"alpha[2]", 269}, {"alpha[3]", 270},
			{"beta[0]", 275}, {"beta[1]", 276}, {"beta[2]", 277}, {"beta[3]", 278},
			{"beta[4]", 279},
			{"gamma[0]", 257}, {"gamma[1]", 259}, {"gamma[2]", 260}, {"gamma[3]", 261},
			{"gamma[4]", 262},
			{"delta[0]", 257}, {"delta[1]", 263}, {"delta[2]", 264}, {"delta[3]", 265},
			{"delta[4]", 266}, {"delta[5]", 258}, {"delta[6]", 271}, {"delta[7]", 272},
			{"delta[8]", 273}, {"delta[9]", 274},
			{"generate", 256},
			{"p_mipmaps", 255},
			{"p_alpha[0]", 290}, {"p_alpha[1]", 291}, {"p_alpha[2]", 292}, {"p_alpha[3]", 293},
			{"p_beta[0]", 298}, {"p_beta[1]", 299}, {"p_beta[2]", 300}, {"p_beta[3]", 301},
			{"p_beta[4]", 302},
			{"p_gamma[0]", 280}, {"p_gamma[1]", 282}, {"p_gamma[2]", 283}, {"p_gamma[3]", 284},
			{"p_gamma[4]", 285},
			{"p_delta[0]", 280}, {"p_delta[1]", 286}, {"p_delta[2]", 287}, {"p_delta[3]", 288},
			{"p_delta[4]", 289}, {"p_delta[5]", 281}, {"p_delta[6]", 294}, {"p_delta[7]", 295},
			{"p_delta[8]", 296}, {"p_delta[9]", 297},
			{"p_generate", 256},
		};
		return table;
	}
} // namespace GSLsfgShaderTable
