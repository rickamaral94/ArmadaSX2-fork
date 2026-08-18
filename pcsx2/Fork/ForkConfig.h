// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

class SettingsInterface;

/// Superfície de configuração DO FORK.
///
/// Motivo de existir: cada opção nova do upstream custa um bit em `Config.h` mais a propagação por
/// `Pcsx2Config.cpp`, `GS.cpp`, `FullscreenUI`, o Qt, o `Settings.kt` do Android e o
/// `native-lib.cpp` — sete arquivos do upstream, repetidos a cada recurso. Com driver por jogo,
/// frame generation (off/auto/2x), métricas e perfis por GPU no roadmap, esse caminho tornaria
/// cada merge do upstream mais caro que o recurso que o motivou.
///
/// Aqui todas as opções do fork moram em UMA tabela declarativa e em UMA seção de INI (`[Fork]`).
/// Acrescentar uma opção é acrescentar uma linha nessa tabela: nenhum arquivo do upstream muda,
/// nenhuma função de JNI nova é escrita, e a UI pode se construir sozinha a partir da tabela.
///
/// **Override por jogo sai de graça.** A leitura passa pelo `SettingsInterface` em camadas que o
/// PCSX2 já monta (base + jogo + input), então uma chave `[Fork]` escrita no INI do jogo prevalece
/// sobre a global exatamente como qualquer opção do upstream — sem código nosso para isso.
///
/// A escrita, por outro lado, é sempre na camada BASE. Persistir na camada de jogo é atribuição do
/// frontend, que é quem sabe onde o INI do jogo vive; frontends que já fazem isso escrevem a mesma
/// chave `[Fork]` e a leitura acima a respeita.
namespace ForkConfig
{
	/// Seção única no INI. Nunca colide com nada do upstream, e um `grep '\[Fork\]'` mostra tudo
	/// que este fork acrescentou à configuração do usuário.
	inline constexpr const char* SECTION = "Fork";

	enum class Type : u8
	{
		Bool,
		Int,
		Float,
		String,
	};

	/// Índice estável de cada opção. A ordem aqui e a da tabela em ForkConfig.cpp são a mesma
	/// coisa, verificada por static_assert — um desalinhamento silencioso faria uma opção ler o
	/// valor da vizinha.
	enum class Option : u32
	{
		/// Fase 2: mede FPS real x apresentado, frametime, 1% low. Desligada por padrão porque
		/// medir tem custo, mesmo pequeno, e nada no fork entra ligado sem evidência.
		PresentationMetricsEnabled,
		/// Fase 2: desenha a linha de cadência no OSD. Separada da anterior de propósito — medir
		/// para o log e mostrar na tela são decisões diferentes, e o benchmark da Fase 6 vai
		/// querer medir sem poluir a captura de tela.
		PresentationMetricsOverlay,

		// --- seleção de driver Vulkan (Fase 5) ---
		//
		// Moram aqui, e não em um estado próprio do frontend, exatamente para herdar o override
		// por jogo: a leitura passa pelo SettingsInterface em camadas, então estas chaves no INI
		// de um jogo prevalecem sobre as globais sem uma linha de código nossa.

		/// `inherit` (padrão), `system` ou `custom`.
		///
		/// `inherit` existe porque "vazio" não consegue distinguir "este jogo não opina" de "este
		/// jogo quer o driver do sistema" — e sem essa distinção não dá para forçar o driver do
		/// sistema em um jogo específico enquanto o global é Turnip, que é justamente o caso de
		/// uso mais comum (um jogo que quebra no Turnip).
		DriverMode,
		/// Diretório do pacote, com barra ao final (contrato do adrenotools).
		DriverDir,
		/// Soname da biblioteca, ex.: `libvulkan_freedreno.so`.
		DriverName,
		/// Diretório onde o driver pode escrever o cache dele.
		DriverRedirectDir,
		/// Diretório das bibliotecas de hook do adrenotools.
		DriverHookLibDir,
		/// Id do pacote, só para a UI casar a seleção com a lista instalada.
		DriverId,

		// --- frame generation (Fase 7) ---

		/// `off` (padrão), `auto` ou `2x`.
		FrameGenMode,
		/// Teto de tempo, em ms, para produzir o quadro gerado.
		FrameGenBudgetMs,
		/// FPS real abaixo disto não engata: suavizar emulação lenta é mascarar.
		FrameGenMinRealFps,

		Count,
	};

	struct OptionDesc
	{
		Option option;
		/// Chave no INI, dentro de `[Fork]`. Estável para sempre: renomear quebra a configuração
		/// já salva do usuário.
		const char* key;
		Type type;
		bool default_bool;
		s32 default_int;
		float default_float;
		const char* default_string;
		/// Texto curto para a UI se construir sozinha, sem uma tabela paralela em cada frontend.
		const char* description;
	};

	/// A tabela inteira, para UI e ferramentas.
	std::span<const OptionDesc> GetOptions();
	const OptionDesc& GetOption(Option option);
	/// Busca pela chave do INI. Devolve nullptr para chave desconhecida — o que acontece quando um
	/// INI mais novo é aberto por um binário mais velho, e ignorar é a resposta certa.
	const OptionDesc* FindOption(std::string_view key);

	// --- leitura (caminho quente: sem lock, sem alocação) ---

	bool GetBool(Option option);
	s32 GetInt(Option option);
	float GetFloat(Option option);
	/// Strings saem por cópia: são lidas em decisões, não por quadro.
	std::string GetString(Option option);

	/// Valor formatado como texto, para JNI/UI genéricas.
	std::string GetValueAsString(Option option);

	// --- escrita ---

	/// Aplica em memória sem persistir. Usado pelo carregamento e por testes.
	void SetValueFromString(Option option, std::string_view value);

	/// Escreve na camada base e persiste. Devolve false para valor inválido para o tipo.
	bool SetAndSave(Option option, std::string_view value);

	// --- ciclo de vida ---

	/// Lê todas as opções do `si` em camadas. Chamado de VMManager::LoadSettings, que é onde o
	/// PCSX2 já reconstrói a configuração — assim aplicar configuração é um caminho só, e o
	/// override por jogo aparece no mesmo instante que o do upstream.
	void LoadSettings(const SettingsInterface& si);

	/// Volta tudo ao padrão. Não persiste.
	void ResetToDefaults();

	/// Como a seleção de driver deve ser aplicada, depois de resolvida.
	enum class DriverSelection : u8
	{
		/// Nada a fazer — mantém o que o frontend já tiver configurado.
		Inherit,
		/// Força o driver do sistema, mesmo que o global seja um Turnip.
		System,
		/// Usa os caminhos configurados.
		Custom,
	};

	/// Resolve o modo lido do INI. Pura, para poder ser testada sem Vulkan e sem Android.
	///
	/// Um modo `custom` sem os caminhos necessários vira `System`, não `Inherit`: uma configuração
	/// quebrada tem que cair em algo previsível e visível, e "inherit" deixaria o jogo rodando com
	/// o driver de outro jogo.
	DriverSelection ResolveDriverSelection(std::string_view mode, std::string_view dir,
		std::string_view name, std::string_view hook_lib_dir);

	/// Chamado depois de cada LoadSettings/SetAndSave, para os módulos reagirem (ligar métrica,
	/// trocar driver, etc). Registre no arranque; não há remoção, porque nada aqui é temporário.
	using ChangeCallback = void (*)();
	void RegisterChangeCallback(ChangeCallback callback);
} // namespace ForkConfig
