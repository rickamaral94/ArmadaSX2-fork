// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkBridge.h"

#include "Fork/ForkBenchmark.h"
#include "Fork/ForkConfig.h"
#include "Fork/ForkDriverIdentity.h"
#include "Fork/ForkDriverPackage.h"
#include "Fork/ForkFrameGen.h"
#include "Fork/ForkGpuCapabilities.h"
#include "Fork/ForkLsfgPackage.h"

#include "fmt/format.h"

namespace
{
	/// Divide "dominio.acao:argumento" em (comando, argumento). O argumento pode conter ':' —
	/// caminhos de arquivo contêm — então só o PRIMEIRO separa.
	std::pair<std::string_view, std::string_view> SplitRequest(std::string_view request)
	{
		const size_t colon = request.find(':');
		if (colon == std::string_view::npos)
			return {request, {}};
		return {request.substr(0, colon), request.substr(colon + 1)};
	}

	std::string Error(std::string_view message)
	{
		return fmt::format(R"({{"ok":false,"error":"{}"}})", ForkBridge::EscapeJson(message));
	}

	std::string Boolean(bool value)
	{
		return value ? "true" : "false";
	}

	std::string InspectDriver(std::string_view path)
	{
		if (path.empty())
			return Error("caminho vazio");

		const ForkDriverPackage::Inspection inspection = ForkDriverPackage::InspectLibrary(std::string(path));
		return fmt::format(
			R"({{"ok":{},"verdict":"{}","reason":"{}","sha256":"{}","size":{},)"
			R"("elf":{{"magic":{},"bits64":{},"littleEndian":{},"machine":{},"type":{}}}}})",
			Boolean(inspection.IsUsable()), ForkDriverPackage::VerdictToString(inspection.verdict),
			ForkBridge::EscapeJson(ForkDriverPackage::VerdictReason(inspection.verdict)), inspection.sha256,
			inspection.size_bytes, Boolean(inspection.elf.has_elf_magic), Boolean(inspection.elf.is_64bit),
			Boolean(inspection.elf.is_little_endian), inspection.elf.machine, inspection.elf.type);
	}

	std::string DriverStatus()
	{
		const ForkDriverIdentity::Identity identity = ForkDriverIdentity::Get();
		return fmt::format(
			R"({{"ok":true,"probed":{},"outcome":"{}","reason":"{}","unexpected":{},)"
			R"("activeDriver":"{}","driverName":"{}","driverInfo":"{}","mesa":"{}",)"
			R"("vulkan":"{}","gpu":"{}","requested":"{}","sha256":"{}","error":"{}",)"
			R"("identityConfirmed":{}}})",
			Boolean(identity.probed), ForkDriverIdentity::OutcomeToString(identity.outcome),
			ForkBridge::EscapeJson(ForkDriverIdentity::OutcomeReason(identity.outcome)),
			Boolean(ForkDriverIdentity::IsUnexpected(identity.outcome)),
			GpuProfileDetector::DriverToString(identity.active_driver),
			ForkBridge::EscapeJson(identity.driver_name), ForkBridge::EscapeJson(identity.driver_info),
			identity.mesa.known ? fmt::format("{}.{}.{}", identity.mesa.major, identity.mesa.minor,
									  identity.mesa.patch)
								: std::string(),
			ForkGpuCapabilities::FormatVulkanVersion(identity.vulkan_api_version),
			ForkBridge::EscapeJson(identity.gpu_name), ForkBridge::EscapeJson(identity.requested_driver),
			identity.package_sha256, ForkBridge::EscapeJson(identity.load_error),
			Boolean(identity.driver_properties_available));
	}

	std::string GpuCapabilities()
	{
		const ForkGpuCapabilities::Capabilities caps = ForkGpuCapabilities::Get();
		return fmt::format(
			R"({{"ok":true,"probed":{},"turnip":"{}","reason":"{}","supported":{},)"
			R"("gpu":"{}","vendor":"{}","architecture":"{}","vulkan":"{}","androidSdk":{}}})",
			Boolean(caps.probed), ForkGpuCapabilities::TurnipSupportToString(caps.turnip),
			ForkBridge::EscapeJson(ForkGpuCapabilities::TurnipSupportReason(caps.turnip)),
			Boolean(caps.turnip == ForkGpuCapabilities::TurnipSupport::Supported),
			ForkBridge::EscapeJson(caps.gpu_name), GpuProfileDetector::RuntimeProfileToString(caps.vendor),
			GpuProfileDetector::ArchitectureToString(caps.architecture),
			ForkGpuCapabilities::FormatVulkanVersion(caps.vulkan_api_version), caps.android_sdk);
	}

	std::string InspectLsfgPackage(std::string_view path)
	{
		if (path.empty())
			return Error("caminho vazio");

		const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::Inspect(std::string(path));
		return fmt::format(
			R"({{"ok":{},"verdict":"{}","reason":"{}","size":{},"pe32plus":{},"machine":{},)"
			R"("rcdata":{},"standardFamily":{},"performanceFamily":{}}})",
			Boolean(inspection.IsUsable()), ForkLsfgPackage::VerdictToString(inspection.verdict),
			ForkBridge::EscapeJson(ForkLsfgPackage::VerdictReason(inspection.verdict)),
			inspection.size_bytes, Boolean(inspection.is_pe32plus), inspection.machine,
			inspection.rcdata_count, Boolean(inspection.has_standard_family),
			Boolean(inspection.has_performance_family));
	}

	std::string FrameGenStatus()
	{
		// O aviso vem daqui, verbatim, em vez de a UI guardar a própria cópia. Uma cópia no
		// frontend pode ser reescrita de forma mais otimista — "FG aumenta o desempenho" — e é
		// exatamente essa frase que o projeto proíbe. A UI mostra o que o núcleo diz.
		const ForkFrameGen::Policy policy = ForkFrameGen::PolicyFromConfig();
		const ForkFrameGen::Decision decision = ForkFrameGen::GetLastDecision();
		return fmt::format(
			R"({{"ok":true,"mode":"{}","warning":"{}","state":"{}","reason":"{}",)"
			R"("framesToGenerate":{},"budgetMs":{:.2f},"minRealFps":{:.1f},"statusLine":"{}"}})",
			ForkFrameGen::ModeToString(policy.mode), ForkBridge::EscapeJson(ForkFrameGen::USER_WARNING),
			ForkFrameGen::StateToString(decision.state),
			ForkBridge::EscapeJson(ForkFrameGen::ReasonText(decision.reason)), decision.frames_to_generate,
			policy.budget_ms, policy.min_real_fps, ForkBridge::EscapeJson(ForkFrameGen::StatusLine(decision)));
	}

	std::string ConfigOptions()
	{
		// A UI se constrói a partir daqui em vez de manter uma tabela paralela que sai de sincronia
		// com pcsx2/Fork/ForkConfig.cpp na primeira opção nova.
		std::string out = R"({"ok":true,"section":")";
		out += ForkConfig::SECTION;
		out += R"(","options":[)";

		bool first = true;
		for (const ForkConfig::OptionDesc& desc : ForkConfig::GetOptions())
		{
			if (!first)
				out += ',';
			first = false;

			const char* type = "string";
			switch (desc.type)
			{
				case ForkConfig::Type::Bool:
					type = "bool";
					break;
				case ForkConfig::Type::Int:
					type = "int";
					break;
				case ForkConfig::Type::Float:
					type = "float";
					break;
				case ForkConfig::Type::String:
					type = "string";
					break;
			}

			out += fmt::format(R"({{"key":"{}","type":"{}","value":"{}","description":"{}"}})",
				ForkBridge::EscapeJson(desc.key), type,
				ForkBridge::EscapeJson(ForkConfig::GetValueAsString(desc.option)),
				ForkBridge::EscapeJson(desc.description ? desc.description : ""));
		}

		out += "]}";
		return out;
	}
} // namespace

std::string ForkBridge::EscapeJson(std::string_view text)
{
	std::string out;
	out.reserve(text.size() + 8);
	for (const char ch : text)
	{
		switch (ch)
		{
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				// Controles precisam de \u; bytes altos passam intactos, porque a saída é UTF-8 e
				// quebrar uma sequência multibyte aqui corromperia nomes de arquivo e acentos das
				// nossas próprias mensagens.
				if (static_cast<unsigned char>(ch) < 0x20)
					out += fmt::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
				else
					out += ch;
				break;
		}
	}
	return out;
}

std::string ForkBridge::Query(std::string_view request)
{
	const auto [command, argument] = SplitRequest(request);

	if (command == "driver.inspect")
		return InspectDriver(argument);
	if (command == "driver.status")
		return DriverStatus();
	if (command == "gpu.capabilities")
		return GpuCapabilities();
	if (command == "config.options")
		return ConfigOptions();
	if (command == "framegen.status")
		return FrameGenStatus();
	if (command == "lsfg.inspect")
		return InspectLsfgPackage(argument);
	if (command == "benchmark.begin")
	{
		if (argument.empty())
			return Error("rótulo vazio: uma execução sem nome não serve para comparar");
		ForkBenchmark::Begin(argument);
		return fmt::format(R"({{"ok":true,"running":true,"label":"{}"}})", EscapeJson(argument));
	}
	if (command == "benchmark.end")
	{
		if (!ForkBenchmark::IsRunning())
			return Error("nenhuma execução em andamento");
		const ForkBenchmark::Run run = ForkBenchmark::End();
		return fmt::format(
			R"({{"ok":true,"label":"{}","realFps":{:.3f},"presentedFps":{:.3f},"low1Fps":{:.3f},)"
			R"("durationSeconds":{:.1f},"driverAsRequested":{},"shaderCompiles":{}}})",
			EscapeJson(run.label), run.real_fps, run.presented_fps, run.low1_fps, run.duration_seconds,
			run.driver_as_requested ? "true" : "false", run.shader_compiles);
	}
	if (command == "benchmark.status")
	{
		return fmt::format(R"({{"ok":true,"running":{},"label":"{}","runs":{}}})",
			ForkBenchmark::IsRunning() ? "true" : "false", EscapeJson(ForkBenchmark::CurrentLabel()),
			ForkBenchmark::GetRuns().size());
	}
	if (command == "benchmark.runs")
		return ForkBenchmark::ToJson();
	if (command == "benchmark.clear")
	{
		ForkBenchmark::ClearRuns();
		return R"({"ok":true})";
	}

	// Consulta desconhecida acontece de verdade: APK novo com Kotlin velho, ou o contrário. Um
	// erro nomeado permite ao frontend degradar; string vazia seria indistinguível de falha.
	return Error(fmt::format("consulta desconhecida: {}", command));
}
