// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSGPUProfile.h"

#include "common/Pcsx2Defs.h"

#include <string>

/// Veredito único sobre o que ESTE aparelho suporta, para o resto do fork não precisar repetir a
/// pergunta — nem respondê-la de formas diferentes em lugares diferentes.
///
/// A base já resolve muita coisa (`GpuProfileDetector` devolve arquitetura Adreno 6xx/7xx/8xx,
/// identidade de driver e uma tabela de defeitos conhecidos). O que faltava era a conclusão:
/// **podemos oferecer troca de driver Turnip aqui?** Espalhar essa decisão por UI, carregador e
/// telas de configuração é como se acaba tentando carregar um driver freedreno em uma Mali.
///
/// A regra dura, que não se negocia: **Turnip é freedreno, logo é Adreno.** Mali, PowerVR, Xclipse
/// e qualquer coisa não reconhecida ficam de fora, independentemente de versão de Android ou de o
/// usuário insistir. Xclipse tem pacotes próprios em formato AdrenoTools, mas não são Turnip e não
/// entram neste veredito.
namespace ForkGpuCapabilities
{
	/// Piso de Android para carregar driver customizado. `VKLoader` chama
	/// `adrenotools_open_libvulkan` com `tmpLibDir = nullptr`, ou seja, pelo caminho memfd, que
	/// exige API 29. Abaixo disso a funcionalidade não é "arriscada", é inexistente — e prometer
	/// na UI o que o carregador vai recusar é pior que esconder.
	inline constexpr u32 MIN_ANDROID_SDK_FOR_CUSTOM_DRIVER = 29;

	enum class TurnipSupport : u8
	{
		/// Ainda não sondado — nenhum renderer subiu. A UI deve tratar como "não sei ainda", não
		/// como "não".
		Unknown,
		Supported,
		/// Plataforma sem o mecanismo (desktop, iOS).
		NotAndroid,
		/// Mali, PowerVR, Xclipse, Apple ou não reconhecida.
		UnsupportedVendor,
		/// Adreno velha demais para builds de Turnip (abaixo da série 6xx).
		UnsupportedAdrenoGeneration,
		/// Adreno compatível, mas Android antigo demais para o carregador.
		UnsupportedAndroidVersion,
	};

	struct Capabilities
	{
		/// False até o primeiro renderer publicar. Distinguir "não sondado" de "não suportado" é o
		/// que evita a UI dizer "seu aparelho não suporta" antes de ter olhado.
		bool probed = false;
		RuntimeGpuProfile vendor = RuntimeGpuProfile::Unknown;
		MobileGpuArchitecture architecture = MobileGpuArchitecture::Unknown;
		u16 model_number = 0;
		std::string gpu_name = "Unknown";
		/// Qual driver está ATIVO agora (Turnip do usuário ou o do sistema).
		MobileGpuDriver active_driver = MobileGpuDriver::Unknown;
		/// `VkPhysicalDeviceProperties::apiVersion`, empacotado.
		u32 vulkan_api_version = 0;
		u32 android_sdk = 0;
		TurnipSupport turnip = TurnipSupport::Unknown;
	};

	/// A regra, isolada de qualquer estado para poder ser testada exaustivamente. Toda a Fase 3 se
	/// resume a esta função estar certa.
	TurnipSupport EvaluateTurnipSupport(
		RuntimeGpuProfile vendor, MobileGpuArchitecture architecture, u32 android_sdk, bool is_android);

	/// Publica o veredito a partir do perfil que o renderer acabou de resolver. Chamado uma vez de
	/// GSDeviceVK::Create.
	void Publish(const GpuProfileSelection& profile, u32 vulkan_api_version);

	Capabilities Get();

	/// Atalho para a UI. False enquanto não sondado — a UI deve consultar o motivo antes de
	/// afirmar qualquer coisa ao usuário.
	bool IsTurnipCapable();

	const char* TurnipSupportToString(TurnipSupport value);
	/// Frase pronta para a UI explicar por que a seção está indisponível, em vez de só apagá-la.
	const char* TurnipSupportReason(TurnipSupport value);

	/// "1.3.281" a partir do inteiro empacotado do Vulkan.
	std::string FormatVulkanVersion(u32 packed);

	/// Uma linha para o log e para o relatório de compatibilidade (Fase 7).
	std::string DescribeForLog();
} // namespace ForkGpuCapabilities
