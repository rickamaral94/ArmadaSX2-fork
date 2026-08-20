package com.armsx2.ui.theme

import androidx.compose.ui.graphics.Color

// Identidade do ArmadaSX2.
//
// A paleta anterior era azul, herdada do projeto de origem, e o nome das constantes dizia isso
// (`ArmsBlue`). Renomear junto com o valor é deliberado: uma constante chamada `ArmsBlue`
// guardando vermelho é uma mentira que sobrevive a todo mundo que ler o arquivo depois.
//
// Só o TEMA PADRÃO muda. A lista de temas selecionáveis continua inteira — inclusive o azul —
// porque trocar a identidade da marca não é o mesmo que tirar a escolha de quem já usa o app.

/** Vermelho de marca. Base do tema claro, onde precisa de peso contra fundo branco. */
val ArmadaRed = Color(0xFFE1483B)

/** O mesmo vermelho clareado para servir de acento sobre fundo escuro.
 *
 *  Um acento tem que ser MAIS CLARO que o fundo em tema escuro, senão o contraste vira ilegível;
 *  por isso a marca tem dois tons em vez de um. */
val ArmadaRedBright = Color(0xFFFF8B7D)

/** Secundária, num tom de rosa-carmim — vizinho do vermelho, mas de matiz distinta o bastante
 *  para os dois não se confundirem quando aparecem lado a lado num mesmo controle. */
val ArmadaCrimson = Color(0xFFD65A7E)

/** Terciária em bronze quente. Fecha a paleta pelo lado quente sem competir com o acento. */
val ArmadaBronze = Color(0xFFD9A05B)

// Noite: bordô profundo no lugar do azul-marinho. Escuro e dessaturado de propósito — capas de
// jogo é que devem saltar da tela, não o fundo.
val NightBackground = Color(0xFF1A0C0A)
val NightSurface = Color(0xFF261311)
val NightSurfaceRaised = Color(0xFF331B18)
val NightOutline = Color(0xFF5A302B)
val NightText = Color(0xFFFFF3F1)
val NightTextMuted = Color(0xFFC3A29D)

// Dia: branco levemente aquecido, para o vermelho não parecer avulso sobre um cinza frio.
val DayBackground = Color(0xFFFDF6F4)
val DaySurface = Color(0xFFFFFFFF)
val DaySurfaceRaised = Color(0xFFF6E8E4)
val DayOutline = Color(0xFFE0C6C0)
val DayText = Color(0xFF221312)
val DayTextMuted = Color(0xFF7A5B56)

val Success = Color(0xFF51D79A)
val Warning = Color(0xFFFFC857)

/** Erro/destrutivo. Empurrado para o rosa-magenta de propósito.
 *
 *  Numa marca vermelha, "erro" e "acento" colidem: o usuário perde o sinal de perigo justamente
 *  porque a interface inteira é vermelha. Deslocar a matiz mantém a leitura de alerta sem sair da
 *  família quente. */
val Danger = Color(0xFFFF4D6D)
