// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSGPUProfile.h"

#include "common/Pcsx2Defs.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Qual driver Vulkan está REALMENTE rodando — perguntado ao dispositivo que subiu, não ao
/// `meta.json` do pacote.
///
/// Por que isto importa mais do que parece: o carregamento de driver customizado tem um fallback
/// **silencioso por desenho**. Se `adrenotools_open_libvulkan` falha, `VKLoader` cai no loader do
/// sistema e o boot continua — o que é a decisão certa, porque derrubar o emulador porque um
/// driver importado não abriu seria pior. O efeito colateral é que, hoje, o usuário que selecionou
/// Turnip e está rodando o blob da Qualcomm **não tem como saber**.
///
/// Isso contamina tudo o que vem depois: um A/B da Fase 6 que compare "System vs Turnip A" sem
/// verificar o que subiu pode estar comparando o driver do sistema com ele mesmo, e concluir que
/// "Turnip não muda nada". Um relatório de compatibilidade da Fase 7 registraria um driver que
/// nunca rodou.
///
/// Este módulo cruza três fontes e emite um veredito:
///   1. o que o usuário PEDIU (VKLoader: havia seleção? o handle abriu?);
///   2. o que o dispositivo DIZ ser (`VkPhysicalDeviceDriverProperties`);
///   3. a versão do Mesa extraída do `driverInfo`, que é onde o freedreno a publica.
namespace ForkDriverIdentity
{
	enum class LoadOutcome : u8
	{
		/// Nenhum renderer subiu ainda.
		Unknown,
		/// Não havia driver customizado selecionado — o driver do sistema é o esperado.
		SystemDriverByChoice,
		/// Driver customizado selecionado, aberto, e o dispositivo se identifica como Turnip.
		CustomDriverActive,
		/// **O caso silencioso.** Havia seleção, o handle não abriu, o sistema assumiu.
		FellBackToSystem,
		/// O handle abriu, mas o ICD ativo não se identifica como Turnip. Pacote repackado,
		/// renomeado, ou algo que não é o que diz ser.
		CustomOpenedButNotTurnip,
	};

	/// Versão do Mesa, quando o driver a publica. Turnip põe algo como "Mesa 25.2.0-devel
	/// (git-1a2b3c4)" em `driverInfo`; o blob da Qualcomm põe outra coisa ou nada.
	struct MesaVersion
	{
		bool known = false;
		u32 major = 0;
		u32 minor = 0;
		u32 patch = 0;
		/// Texto original, preservado porque a parte depois do número (`-devel`, `git-...`) é o
		/// que distingue duas builds do mesmo release — exatamente o que um relatório de
		/// compatibilidade precisa.
		std::string raw;
	};

	struct Identity
	{
		bool probed = false;
		LoadOutcome outcome = LoadOutcome::Unknown;

		std::string requested_driver; ///< soname pedido, vazio se nenhum
		std::string load_error; ///< por que o carregamento falhou, quando falhou

		MobileGpuDriver active_driver = MobileGpuDriver::Unknown;
		std::string driver_name; ///< VkPhysicalDeviceDriverProperties::driverName
		std::string driver_info; ///< ...::driverInfo
		MesaVersion mesa;

		u32 vulkan_api_version = 0;
		u32 driver_version_raw = 0;
		std::string gpu_name;

		/// Falso quando VK_KHR_driver_properties não existe. Sem ele não dá para afirmar qual ICD
		/// está ativo, e o módulo se recusa a acusar divergência — errar para o lado de não
		/// alarmar é melhor que gritar lobo.
		bool driver_properties_available = false;

		/// SHA-256 do pacote selecionado, quando o frontend o informa (Fase 4, item 1).
		std::string package_sha256;
	};

	// --- lógica pura (testável sem Vulkan, sem dispositivo) ---

	MesaVersion ParseMesaVersion(std::string_view driver_info);

	LoadOutcome EvaluateOutcome(
		bool requested, bool opened, MobileGpuDriver active_driver, bool driver_properties_available);

	const char* OutcomeToString(LoadOutcome value);
	/// Frase para a UI e para o log.
	const char* OutcomeReason(LoadOutcome value);
	/// True quando o usuário precisa ser avisado: o que está rodando não é o que ele pediu.
	bool IsUnexpected(LoadOutcome value);

	// --- estado ---

	struct PublishInput
	{
		bool requested = false;
		bool opened = false;
		std::string requested_driver;
		std::string load_error;
		MobileGpuDriver active_driver = MobileGpuDriver::Unknown;
		std::string driver_name;
		std::string driver_info;
		std::string gpu_name;
		u32 vulkan_api_version = 0;
		u32 driver_version_raw = 0;
		bool driver_properties_available = false;
	};

	void Publish(const PublishInput& input);
	Identity Get();
	/// Informado pelo frontend ao selecionar o pacote; sobrevive à publicação do renderer.
	void NoteSelectedPackageSha256(std::string sha256);

	/// Uma linha para o log e para o relatório de compatibilidade (Fase 7).
	std::string DescribeForLog();

	// --- chave de cache de pipeline ---

	/// Identificador curto e estável do driver ativo, para dar a cada driver o SEU arquivo de
	/// cache de pipeline.
	///
	/// O cache de pipeline já é seguro: o `pipelineCacheUUID` muda entre drivers (a spec exige) e
	/// a validação existente rejeita um blob de outro driver. O problema é outro — o arquivo tem
	/// nome fixo, ou seja, UM slot. Trocar de driver invalida e **sobrescreve** o do anterior, e
	/// voltar recompila tudo. No A/B da Fase 6 (System x Turnip A x Turnip B) cada troca pagaria
	/// compilação a frio, e "tempo de compilação de shader" é justamente uma das métricas medidas:
	/// o número mediria o primeiro boot, não o regime.
	///
	/// Deriva de vendorID + deviceID + driverID + driverVersion + pipelineCacheUUID, que é
	/// exatamente o conjunto que a validação do upstream confere.
	std::string PipelineCacheKey(u32 vendor_id, u32 device_id, u32 driver_id, u32 driver_version,
		std::span<const u8> pipeline_cache_uuid);

	/// Quais arquivos de cache podar, dado o conjunto existente e quantos manter.
	///
	/// Um arquivo por driver acumula: cada atualização do Turnip gera uma chave nova, e um blob de
	/// pipeline chega a dezenas de MB no armazenamento de um celular. Mantém os `keep` mais
	/// recentes (por mtime) e devolve o resto — o ativo NUNCA é podado, mesmo que seja o mais
	/// antigo, porque podá-lo forçaria a recompilação que a chave existe para evitar.
	struct CacheFileEntry
	{
		std::string path;
		s64 modified_time = 0;
	};
	std::vector<std::string> SelectStalePipelineCaches(
		std::vector<CacheFileEntry> entries, const std::string& active_path, size_t keep);
} // namespace ForkDriverIdentity
