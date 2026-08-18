// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

class SettingsInterface;

/// Cola entre a configuração do fork e os módulos que a consomem.
///
/// Existe para que o único ponto de contato com o upstream seja UMA linha em
/// VMManager::LoadSettings. `ForkConfig` não conhece o GS, o GS não conhece o carregamento de
/// settings, e ninguém precisa editar VMManager de novo quando o próximo módulo do fork chegar:
/// ele se registra aqui.
namespace ForkRuntime
{
	/// Carrega as opções do fork a partir do `si` em camadas (base + jogo) e aplica nos módulos.
	/// Idempotente: chamada em todo apply de configuração, como a do upstream.
	void LoadSettings(const SettingsInterface& si);
} // namespace ForkRuntime
