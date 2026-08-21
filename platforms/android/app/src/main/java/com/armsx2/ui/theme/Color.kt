package com.armsx2.ui.theme

import androidx.compose.ui.graphics.Color

// Identidade do ArmadaSX2.
//
// A paleta original era azul, herdada do projeto de origem, e o nome das constantes dizia isso
// (`ArmsBlue`). Ela passou brevemente por vermelho e agora é ROXO ESCURO, a pedido — o vermelho
// ficou forte demais na tela. Renomear junto com o valor é deliberado, e é a razão de este
// arquivo já ter sido renomeado duas vezes: uma constante chamada `ArmsBlue` guardando vermelho
// é uma mentira que sobrevive a todo mundo que ler o arquivo depois.
//
// "Forte demais" é queixa de INTENSIDADE, não de matiz, então a troca não foi só girar o tom: os
// roxos abaixo são mais dessaturados e mais profundos do que os vermelhos que substituíram. Os
// pares de contraste foram conferidos — todo texto e todo acento fica acima de 4,5:1 sobre o
// fundo em que é desenhado, nos dois temas.
//
// Só o TEMA PADRÃO muda. A lista de temas selecionáveis continua inteira — inclusive o azul —
// porque trocar a identidade da marca não é o mesmo que tirar a escolha de quem já usa o app.

/** Roxo de marca. Base do tema claro, onde precisa de peso contra fundo branco. */
val ArmadaPurple = Color(0xFF5B3E96)

/** O mesmo roxo clareado para servir de acento sobre fundo escuro.
 *
 *  Um acento tem que ser MAIS CLARO que o fundo em tema escuro, senão o contraste vira ilegível;
 *  por isso a marca tem dois tons em vez de um. */
val ArmadaPurpleLight = Color(0xFFB9A3E8)

/** Secundária, num orquídea — vizinha do roxo, mas de matiz distinta o bastante para as duas não
 *  se confundirem quando aparecem lado a lado num mesmo controle. */
val ArmadaOrchid = Color(0xFFC07BD4)

/** Terciária em bronze quente. Fecha a paleta pelo lado oposto do círculo sem competir com o
 *  acento — é o único tom quente da identidade, e contra roxo ele rende mais do que rendia contra
 *  vermelho, então sobreviveu à troca sem mudar um dígito. */
val ArmadaBronze = Color(0xFFD9A05B)

// Noite: quase-preto arroxeado no lugar do bordô. Escuro e dessaturado de propósito — capas de
// jogo é que devem saltar da tela, não o fundo.
val NightBackground = Color(0xFF120B1F)
val NightSurface = Color(0xFF1B1230)
val NightSurfaceRaised = Color(0xFF261A40)
val NightOutline = Color(0xFF453466)
val NightText = Color(0xFFF3EFFB)
val NightTextMuted = Color(0xFFB0A4C6)

// Dia: branco levemente arroxeado, para o roxo não parecer avulso sobre um cinza neutro.
val DayBackground = Color(0xFFF8F5FD)
val DaySurface = Color(0xFFFFFFFF)
val DaySurfaceRaised = Color(0xFFEDE6F7)
val DayOutline = Color(0xFFD4C8E6)
val DayText = Color(0xFF1B1426)
val DayTextMuted = Color(0xFF62576F)

val Success = Color(0xFF51D79A)
val Warning = Color(0xFFFFC857)

/** Erro/destrutivo, em vermelho franco.
 *
 *  Este valor mudou POR CAUSA da troca de marca, e vale registrar o raciocínio invertendo. Numa
 *  marca vermelha, "erro" e "acento" colidiam — o usuário perdia o sinal de perigo justamente
 *  porque a interface inteira era vermelha —, então o erro tinha sido empurrado para o magenta.
 *  Com marca roxa a colisão passa a ser com o magenta, e o vermelho volta a estar livre: é o
 *  sinal de perigo mais direto que existe, e agora nada mais na interface o disputa. */
val Danger = Color(0xFFFF5C5C)

// --- Azul de origem, preservado como TEMA SELECIONÁVEL -------------------------------------
//
// Estes valores são o esquema escuro original do ARMSX2, e não são decoração histórica.
//
// O seletor de temas tem uma opção rotulada "Blue", e até agora ela apontava para o esquema
// PADRÃO (NightScheme) — que era azul apenas enquanto a marca era azul. Repintar o padrão de
// vermelho, na etapa 1, repintou a opção "Blue" junto, em silêncio: quem escolhesse azul recebia
// a cor da marca com um rótulo dizendo outra coisa. O comentário no topo deste arquivo afirmava
// que a lista continuava inteira "inclusive o azul", e essa afirmação estava errada desde então.
//
// Com o azul morando aqui, o rótulo volta a dizer a verdade e a promessa passa a ser verificável
// em vez de aspiracional. Trocar a identidade da marca não pode custar a escolha de quem já usa.
val LegacyBlue = Color(0xFF73A8FF)
val LegacyCyan = Color(0xFF48D7F0)
val LegacyViolet = Color(0xFF8B7CFF)
val LegacyBlueBackground = Color(0xFF0A1C36)
val LegacyBlueSurface = Color(0xFF13284A)
val LegacyBlueSurfaceRaised = Color(0xFF1A3357)
val LegacyBlueOutline = Color(0xFF2F4E76)
val LegacyBlueText = Color(0xFFF2F6FF)
val LegacyBlueTextMuted = Color(0xFF9BAAC0)
val LegacyBlueDanger = Color(0xFFFF6B7A)
