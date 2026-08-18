// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <string_view>

/// Porta ÚNICA entre o frontend e os módulos do fork: texto entra, JSON sai.
///
/// A alternativa seria uma função JNI por consulta — uma para inspecionar pacote, outra para o
/// estado do driver, outra para as capacidades da GPU, e mais uma a cada recurso. Cada uma dessas
/// exige editar `NativeApp.java` e `native-lib.cpp`, ambos do upstream, e o custo se repete para
/// sempre. É a mesma armadilha que a superfície de configuração evitou ao usar `setSetting`
/// genérico: **uma porta bem definida custa uma vez.**
///
/// O preço é despacho por string, que o compilador não confere. Ele é pago com testes: cada
/// consulta tem caso próprio, e uma consulta desconhecida responde um erro estruturado em vez de
/// string vazia — porque "vazio" é indistinguível de "falhou" do lado Kotlin.
///
/// Formato da requisição: `dominio.acao` ou `dominio.acao:argumento`.
/// Formato da resposta: sempre um objeto JSON. Sempre com a chave `ok`.
namespace ForkBridge
{
	/// Consultas suportadas:
	///
	///   `driver.inspect:<caminho>`  valida um .so de driver e devolve veredito + SHA-256
	///   `driver.status`             o que está rodando de fato (Fase 4, item 2)
	///   `gpu.capabilities`          suporte a Turnip neste aparelho e o porquê (Fase 3)
	///   `config.options`            a tabela de opções do fork, para a UI se construir sozinha
	///   `benchmark.begin:<rótulo>`  inicia uma execução medida (Fase 6)
	///   `benchmark.end`             encerra e devolve o resultado
	///   `benchmark.status`          estado atual
	///   `benchmark.runs`            todas as execuções, em JSON
	///   `benchmark.clear`           descarta as execuções guardadas
	///
	/// Nunca devolve string vazia: erro também é JSON.
	std::string Query(std::string_view request);

	/// Escapa uma string para dentro de JSON. Exposto porque é a parte com risco real de bug —
	/// aspas, contrabarra e controles em nome de arquivo vindo de armazenamento externo.
	std::string EscapeJson(std::string_view text);
} // namespace ForkBridge
