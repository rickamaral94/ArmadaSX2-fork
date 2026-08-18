// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkBridge.h"

#include "Fork/ForkConfig.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
	bool Contains(const std::string& haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string::npos;
	}
} // namespace

// Consulta desconhecida acontece de verdade: APK novo com Kotlin velho, ou o contrário. Erro
// nomeado permite degradar; string vazia seria indistinguível de "a chamada falhou".
TEST(ForkBridge, UnknownQueriesAnswerAStructuredError)
{
	const std::string response = ForkBridge::Query("coisa.inexistente");
	EXPECT_FALSE(response.empty());
	EXPECT_TRUE(Contains(response, "\"ok\":false"));
	EXPECT_TRUE(Contains(response, "consulta desconhecida"));
	EXPECT_TRUE(Contains(response, "coisa.inexistente"));
}

TEST(ForkBridge, EveryResponseCarriesTheOkKey)
{
	for (const char* request : {"driver.status", "gpu.capabilities", "config.options",
			 "driver.inspect:/nao/existe.so", "nada"})
	{
		const std::string response = ForkBridge::Query(request);
		EXPECT_TRUE(Contains(response, "\"ok\":")) << request;
		EXPECT_EQ(response.front(), '{') << request;
		EXPECT_EQ(response.back(), '}') << request;
	}
}

// Caminhos de arquivo contêm ':' (armazenamento externo, content URIs convertidos), então só o
// PRIMEIRO separa comando de argumento.
TEST(ForkBridge, ArgumentsMayContainColons)
{
	const std::string response = ForkBridge::Query("driver.inspect:/storage/emulated/0:foo/driver.so");
    // Não existe, mas tem que ter sido tratado como inspeção — e não como consulta desconhecida.
	EXPECT_TRUE(Contains(response, "\"verdict\""));
	EXPECT_FALSE(Contains(response, "consulta desconhecida"));
}

TEST(ForkBridge, InspectReportsVerdictAndReasonForAMissingFile)
{
	const std::string response = ForkBridge::Query("driver.inspect:/nao/existe/libvulkan_freedreno.so");
	EXPECT_TRUE(Contains(response, "\"ok\":false"));
	EXPECT_TRUE(Contains(response, "MissingLibrary"));
	EXPECT_TRUE(Contains(response, "\"reason\""));
	EXPECT_TRUE(Contains(response, "\"elf\""));
}

TEST(ForkBridge, InspectRejectsAnEmptyPath)
{
	EXPECT_TRUE(Contains(ForkBridge::Query("driver.inspect:"), "\"ok\":false"));
	EXPECT_TRUE(Contains(ForkBridge::Query("driver.inspect"), "\"ok\":false"));
}

// A UI se constrói a partir desta consulta em vez de manter uma tabela paralela que sai de
// sincronia com ForkConfig.cpp na primeira opção nova.
TEST(ForkBridge, ConfigOptionsDescribesTheWholeTable)
{
	const std::string response = ForkBridge::Query("config.options");
	EXPECT_TRUE(Contains(response, "\"ok\":true"));
	EXPECT_TRUE(Contains(response, ForkConfig::SECTION));

	for (const ForkConfig::OptionDesc& desc : ForkConfig::GetOptions())
	{
		EXPECT_TRUE(Contains(response, desc.key)) << desc.key;
		EXPECT_TRUE(Contains(response, desc.description)) << desc.key;
	}
}

TEST(ForkBridge, DriverStatusAndGpuCapabilitiesAnswerBeforeAnyRenderer)
{
	// Antes de qualquer renderer subir as duas respondem "não sondado" — e não um erro, porque a
	// UI precisa distinguir "ainda não sei" de "falhou".
	const std::string driver = ForkBridge::Query("driver.status");
	EXPECT_TRUE(Contains(driver, "\"ok\":true"));
	EXPECT_TRUE(Contains(driver, "\"probed\""));

	const std::string gpu = ForkBridge::Query("gpu.capabilities");
	EXPECT_TRUE(Contains(gpu, "\"ok\":true"));
	EXPECT_TRUE(Contains(gpu, "\"turnip\""));
	EXPECT_TRUE(Contains(gpu, "\"reason\""));
}

// A parte com risco real de bug: nome de arquivo vindo de armazenamento externo pode conter
// aspas, contrabarra e controles, e uma resposta malformada quebra o parser do lado Kotlin.
TEST(ForkBridge, JsonEscapingHandlesHostileText)
{
	EXPECT_EQ(ForkBridge::EscapeJson(R"(aspas " aqui)"), R"(aspas \" aqui)");
	EXPECT_EQ(ForkBridge::EscapeJson(R"(barra \ aqui)"), R"(barra \\ aqui)");
	EXPECT_EQ(ForkBridge::EscapeJson("linha\nnova"), "linha\\nnova");
	EXPECT_EQ(ForkBridge::EscapeJson("tab\there"), "tab\\there");
	EXPECT_EQ(ForkBridge::EscapeJson(std::string("\x01")), "\\u0001");
	EXPECT_EQ(ForkBridge::EscapeJson(""), "");
}

// Acentos e outros bytes altos passam intactos: a saída é UTF-8, e quebrar uma sequência
// multibyte corromperia nomes de arquivo e as nossas próprias mensagens em português.
TEST(ForkBridge, Utf8PassesThroughUnbroken)
{
	const std::string accented = "não é uma biblioteca nativa";
	EXPECT_EQ(ForkBridge::EscapeJson(accented), accented);
}
