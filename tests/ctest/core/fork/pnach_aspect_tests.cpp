// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkPnachAspect.h"

#include <gtest/gtest.h>

using ForkPnachAspect::Kind;
using ForkPnachAspect::Parse;

// O que SEMPRE funcionou, e não pode parar de funcionar. Medido: 18 carregamentos de jogo, ao
// longo das alphas, aplicaram 1.7777777 por pedido de pnach.
TEST(ForkPnachAspect, PlainRatiosKeepWorking)
{
	EXPECT_EQ(Parse("16:9").kind, Kind::Ratio);
	EXPECT_NEAR(Parse("16:9").ratio, 16.0f / 9.0f, 1e-5f);
	EXPECT_NEAR(Parse("4:3").ratio, 4.0f / 3.0f, 1e-5f);
	EXPECT_NEAR(Parse("21:9").ratio, 21.0f / 9.0f, 1e-5f);
	EXPECT_NEAR(Parse("10:7").ratio, 10.0f / 7.0f, 1e-5f);
}

// "Stretch" é uma das formas da lista oficial do PCSX2 e era recusada. Custo medido: NFS
// Underground 2 pede três vezes, as três falham, e o jogo fica com a geometria de widescreen
// espremida no 4:3 do modo Auto.
TEST(ForkPnachAspect, StretchIsAModeAndNotARatio)
{
	EXPECT_EQ(Parse("Stretch").kind, Kind::Stretch);
	// O pnach é escrito à mão por gente diferente; exigir a capitalização exata seria transformar
	// um detalhe de digitação em falha silenciosa de renderização.
	EXPECT_EQ(Parse("stretch").kind, Kind::Stretch);
	EXPECT_EQ(Parse("STRETCH").kind, Kind::Stretch);
	EXPECT_EQ(Parse("  Stretch  ").kind, Kind::Stretch);
	// E não vem com razão nenhuma: quem chama tem que usar o override de MODO.
	EXPECT_EQ(Parse("Stretch").ratio, 0.0f);
}

// "19.5:9" também está na lista oficial, e caía porque o dividendo era lido como `uint`.
TEST(ForkPnachAspect, FractionalDividendsParse)
{
	const auto r = Parse("19.5:9");
	EXPECT_EQ(r.kind, Kind::Ratio);
	EXPECT_NEAR(r.ratio, 19.5f / 9.0f, 1e-5f);
}

// O que continua sendo recusado — e é bom que seja.
TEST(ForkPnachAspect, GarbageIsStillRefused)
{
	EXPECT_EQ(Parse("").kind, Kind::Invalid);
	EXPECT_EQ(Parse("   ").kind, Kind::Invalid);
	EXPECT_EQ(Parse("banana").kind, Kind::Invalid);
	EXPECT_EQ(Parse("16").kind, Kind::Invalid) << "sem divisor não é razão";
	EXPECT_EQ(Parse("16:0").kind, Kind::Invalid) << "divisão por zero";
	EXPECT_EQ(Parse("0:9").kind, Kind::Invalid) << "razão nula";
	EXPECT_EQ(Parse("-16:9").kind, Kind::Invalid) << "razão negativa";
	EXPECT_EQ(Parse("16/9").kind, Kind::Invalid) << "o separador é dois-pontos";
	// "Auto 4:3/3:2" e "Custom" são nomes válidos de MODO no PCSX2, mas não fazem sentido como
	// override de pnach: "Auto" já é o padrão sobre o qual o override age, e "Custom" precisa de
	// um valor que o pnach não fornece. Recusar os dois é o comportamento certo.
	EXPECT_EQ(Parse("Auto 4:3/3:2").kind, Kind::Invalid);
	EXPECT_EQ(Parse("Custom").kind, Kind::Invalid);
}

// Lixo colado no fim passava pelo parser antigo, que parava de ler assim que tinha os três campos.
// Um erro de digitação virava um aspecto silenciosamente errado em vez de um erro no log.
TEST(ForkPnachAspect, TrailingGarbageIsNotSilentlyAccepted)
{
	EXPECT_EQ(Parse("16:9abc").kind, Kind::Invalid);
	EXPECT_EQ(Parse("16:9 lixo").kind, Kind::Invalid);
}
