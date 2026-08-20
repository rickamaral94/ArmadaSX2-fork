// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string_view>

/// Gramática do `gsaspectratio=` de um pnach.
///
/// Extraída de `Patch::PatchFunc::gsaspectratio` para poder ser exercitada sem emulador. O parser
/// anterior morava inteiro dentro da função que aplica o efeito, e por isso nunca teve um teste —
/// foi preciso um log de aparelho para descobrir que ele recusava três das formas que a própria
/// lista oficial do PCSX2 (`GSOptions::AspectRatioNames`) considera válidas.
///
/// Não depende de `Config.h` de propósito: quem chama traduz [Kind::Stretch] para o enum do
/// emulador. Assim o módulo — e o teste — não arrastam o mundo do PCSX2 atrás de si.
namespace ForkPnachAspect
{
	enum class Kind : u8
	{
		/// Não é nenhuma forma reconhecida. Quem chama registra o erro.
		Invalid,
		/// Uma razão numérica, em [Result::ratio].
		Ratio,
		/// "Stretch": preencher a viewport. Não é uma razão, é a ausência de uma — por isso não
		/// cabe num float e vira override de MODO.
		Stretch,
	};

	struct Result
	{
		Kind kind = Kind::Invalid;
		float ratio = 0.0f;
	};

	/// Aceita `N:M` com N possivelmente fracionário (para "19.5:9", que a lista oficial traz e o
	/// parser antigo recusava por ler o dividendo como `uint`), e o nome "Stretch",
	/// insensível a maiúsculas e a espaços em volta.
	Result Parse(std::string_view param);
} // namespace ForkPnachAspect
