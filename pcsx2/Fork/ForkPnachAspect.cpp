// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkPnachAspect.h"

#include "common/StringUtil.h"

#include <sstream>
#include <string>

ForkPnachAspect::Result ForkPnachAspect::Parse(std::string_view param)
{
	const std::string_view trimmed = StringUtil::StripWhitespace(param);
	if (trimmed.empty())
		return {};

	if (StringUtil::compareNoCase(trimmed, "Stretch"))
		return {Kind::Stretch, 0.0f};

	// Dividendo em `float`, não em `uint`: é o que admite "19.5:9". O divisor segue inteiro porque
	// nenhuma das formas da lista oficial tem denominador fracionário, e aceitar um abriria a porta
	// para "16:9.0" — que ninguém escreve e que só ampliaria a superfície de engano.
	std::string str(trimmed);
	std::istringstream ss(str);
	float dividend = 0.0f;
	uint divisor = 0;
	char delimiter = 0;

	ss >> dividend >> delimiter >> divisor;
	if (ss.fail() || delimiter != ':' || divisor == 0 || dividend <= 0.0f)
		return {};

	// Lixo depois do número é recusado: "16:9abc" era aceito pelo parser antigo, porque ele parava
	// de ler assim que tinha os três campos. Um pnach com erro de digitação passava como válido.
	std::string leftover;
	ss >> leftover;
	if (!leftover.empty())
		return {};

	return {Kind::Ratio, dividend / static_cast<float>(divisor)};
}
