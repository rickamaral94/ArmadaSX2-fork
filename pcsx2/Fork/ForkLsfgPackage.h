// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <span>
#include <string>
#include <vector>

/// Validação do arquivo de Frame Generation que o USUÁRIO fornece (o `Lossless.dll` do Lossless
/// Scaling), no momento em que ele o escolhe.
///
/// ## Por que não basta perguntar ao `GSLsfg`
///
/// A tela de import perguntava `NativeApp.lsfgAvailability(caminho)` e recusava o arquivo quando a
/// resposta era `DllUnreadable`. Só que `GSLsfg::GetUnavailableReason()` responde as travas de
/// HARDWARE primeiro — não é Vulkan, não é Adreno 7xx — e nesses casos **nem chega a abrir o
/// arquivo**. Ou seja: num aparelho incompatível, depois que um jogo já subiu (é quando as
/// capacidades passam a ser conhecidas), a validação aceitava silenciosamente qualquer coisa: um
/// .txt, um download interrompido, o DLL errado. O caminho ficava gravado parecendo correto e o
/// erro só aparecia muito depois, em outro aparelho ou com outro renderer, como "falha ao
/// inicializar" — a mensagem que menos ajuda.
///
/// Este módulo responde só sobre o ARQUIVO, sem GPU, sem renderer e sem depender de o backend de
/// LSFG estar compilado. E responde mais do que "sim ou não": diz qual família de shaders o
/// arquivo carrega, porque "você escolheu o arquivo errado" e "seu Lossless Scaling é antigo
/// demais" são problemas diferentes, e só um deles se resolve escolhendo outro arquivo.
///
/// ## Nada de proprietário mora aqui
///
/// O que existe neste módulo são OFFSETS para dentro do arquivo do usuário — os mesmos ids de
/// recurso que o extrator já usa, em `GSLsfgShaderTable.h`. Nenhum shader, nenhum byte do
/// Lossless Scaling é copiado, lido para dentro do repositório ou redistribuído.
namespace ForkLsfgPackage
{
	/// Do mais específico para o mais genérico: o primeiro problema encontrado é o reportado.
	enum class Verdict : u8
	{
		Ok,
		/// O arquivo não existe.
		Missing,
		/// Existe mas não pôde ser lido (permissão, IO).
		Unreadable,
		/// Não começa com "MZ", ou não há assinatura "PE\0\0" onde o cabeçalho DOS aponta. O caso
		/// real mais comum é uma página de erro HTML salva com nome de .dll.
		NotAPortableExecutable,
		/// Tem magic de PE mas acaba antes do que o próprio cabeçalho declara — download
		/// interrompido. Só reportado quando o magic confere, pela mesma razão do inspetor de
		/// drivers: mandar rebaixar um arquivo que nunca foi um DLL não ajuda ninguém.
		Truncated,
		/// PE válido, mas sem seção de recursos. Não é um Lossless.dll.
		NoResources,
		/// PE com recursos, mas sem nenhuma família de shaders completa. O arquivo é um DLL de
		/// verdade — só não é este. Ou é uma versão de Lossless Scaling velha demais.
		NoShaderFamily,
	};

	/// O que o arquivo declara e o que ele carrega. Exposto para log, UI e diagnóstico.
	struct Inspection
	{
		Verdict verdict = Verdict::Missing;
		u64 size_bytes = 0;
		/// PE32+ (x86-64) ou PE32. Registrado, **não** usado para reprovar: o extrator lê recursos,
		/// e recurso não tem arquitetura. Reprovar por isso inventaria uma restrição que o código
		/// que consome o arquivo não tem.
		bool is_pe32plus = false;
		u16 machine = 0;
		/// Quantos recursos RCDATA o arquivo tem ao todo.
		u32 rcdata_count = 0;
		/// LSFG 3.1 — a família padrão.
		bool has_standard_family = false;
		/// LSFG 3.1p — a família leve. Uma versão do Lossless Scaling costuma trazer uma ou outra;
		/// é por isso que o pedido de 3.1p sabe cair para 3.1 em vez de falhar.
		bool has_performance_family = false;

		bool IsUsable() const { return verdict == Verdict::Ok; }
	};

	/// Lê o cabeçalho e caminha a árvore de recursos. Não carrega os blobs: a checagem é de
	/// PRESENÇA, e um Lossless.dll tem ~7 MB de shaders que a tela de configuração não precisa
	/// ler para dizer se o arquivo serve.
	Inspection Inspect(const std::string& path);

	/// A parte com toda a lógica, pura e sem IO, para ser exercitada sem um DLL de verdade em
	/// disco. Recebe o arquivo inteiro em memória.
	Inspection InspectBytes(std::span<const u8> file);

	/// Ids de recurso que cada família exige, derivados de `GSLsfgShaderTable` — nunca digitados
	/// de novo aqui. Uma segunda cópia dos números divergiria na primeira versão do Lossless
	/// Scaling que renumerasse um recurso, e a cópia divergente é justamente a que diria ao
	/// usuário que o arquivo dele está bom quando não está.
	std::vector<u32> RequiredResourceIds(bool performance_family);

	const char* VerdictToString(Verdict value);
	/// Frase para a UI. Um import recusado sem motivo legível vira relato de bug.
	const char* VerdictReason(Verdict value);
} // namespace ForkLsfgPackage
