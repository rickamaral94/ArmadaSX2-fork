#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
# SPDX-License-Identifier: GPL-3.0+
"""Resume o fluxo de comandos de um GS dump (.gs ou .gs.zst) sem precisar de GPU.

Para que serve: um dump replayado mede desempenho, mas isso exige o aparelho. Ler o dump como
DADO responde outras perguntas, e responde em qualquer máquina — quanto trabalho a cena manda
para o GS, quais equações de blending o jogo usa de verdade, e quanto disso é blending real
contra blending que resolve para opaco.

Este script NÃO reimplementa a decisão de nível de blending do GSRendererHW. Essa depende de
estado de runtime (barreiras, sobreposição de primitivas, faixa de alpha por draw vinda da
texture cache) que o dump sozinho não carrega. O que sai daqui é o dado cru; a decisão continua
sendo medida no aparelho.

Formato do arquivo, conforme GSDumpFile::ReadFile (pcsx2/GS/GSLzma.cpp):

    u32 crc; u32 state_size; state[state_size];
    se crc == 0xFFFFFFFF: state[] começa com GSDumpHeader e o savestate real vem logo depois
    regs[8192]
    pacotes: u8 id — 0 Transfer (u8 path, u32 len, payload), 1 VSync (1B),
                     2 ReadFIFO2 (4B), 3 Registers (8192B)

Uso:
    tools/fork/gsdump-stats.py captura.gs.zst
    tools/fork/gsdump-stats.py --frames 60 --json captura.gs
"""

import argparse
import collections
import json
import struct
import subprocess
import sys

# ---------------------------------------------------------------- constantes do GS

ABD = {0: "Cs", 1: "Cd", 2: "0"}          # operandos A, B, D do ALPHA
CC = {0: "As", 1: "Ad", 2: "FIX"}         # fator C
CD = 1                                     # índice do "Cd" em ABD

PRIMNAME = {0: "point", 1: "line", 2: "linestrip", 3: "tri", 4: "tristrip",
            5: "trifan", 6: "sprite", 7: "invalid"}

# nibbles do campo REGS do GIFtag
REG_RGBAQ = 0x1
REG_XYZF2 = 0x4
REG_XYZ2 = 0x5
REG_AD = 0xE

# endereços usados nas escritas A+D (pcsx2/GS/GSRegs.h)
AD_PRIM = 0x00
AD_RGBAQ = 0x01
AD_XYZF2 = 0x04
AD_XYZ2 = 0x05
AD_PRMODECONT = 0x1A
AD_PRMODE = 0x1B
AD_ALPHA_1 = 0x42
AD_ALPHA_2 = 0x43

OPAQUE_ALPHA = 0x80  # 128: na escala do PS2 é "1.0", então (Cs-Cd)*As>>7 + Cd == Cs


def open_dump(path):
    """Devolve os bytes do dump, descomprimindo .zst se preciso."""
    if not path.endswith(".zst"):
        return open(path, "rb").read()
    try:
        import zstandard
    except ImportError:
        pass
    else:
        with open(path, "rb") as fh:
            out = bytearray()
            reader = zstandard.ZstdDecompressor().stream_reader(fh)
            while True:
                chunk = reader.read(1 << 22)
                if not chunk:
                    break
                out += chunk
            return bytes(out)
    try:
        return subprocess.run(["zstd", "-dc", path], check=True,
                              stdout=subprocess.PIPE).stdout
    except (OSError, subprocess.CalledProcessError):
        sys.exit("preciso do módulo python 'zstandard' ou do binário 'zstd' para ler .zst")


def decode_alpha(v):
    """ALPHA = (A-B)*C>>7 + D. Layout: A:2 B:2 C:2 D:2 pad:24 FIX:8."""
    lo = v & 0xFFFFFFFF
    return (lo & 3, (lo >> 2) & 3, (lo >> 4) & 3, (lo >> 6) & 3, (v >> 32) & 0xFF)


def eq_str(key):
    a, b, c, d, fix = key
    factor = CC[c] if c != 2 else f"FIX={fix}"
    return f"({ABD[a]}-{ABD[b]})*{factor}>>7 + {ABD[d]}"


class GSState:
    """Só o estado que muda a interpretação de um kick de vértice."""

    def __init__(self):
        self.alpha = [(0, 1, 0, 1, 0), (0, 1, 0, 1, 0)]  # contexto 1 e 2
        self.prim_type = 0
        self.prim_abe = 0
        self.prim_ctx = 0
        self.prmode_abe = 0
        self.prmode_ctx = 0
        self.prmodecont = 1  # reset do GS: atributos vêm do PRIM, não do PRMODE
        self.vertex_alpha = OPAQUE_ALPHA

    def set_prim(self, v):
        self.prim_type = v & 7
        self.prim_abe = (v >> 6) & 1
        self.prim_ctx = (v >> 9) & 1

    def set_prmode(self, v):
        # PRMODE repete os bits do PRIM a partir do IIP; o tipo de primitiva continua no PRIM.
        self.prmode_abe = (v >> 6) & 1
        self.prmode_ctx = (v >> 9) & 1

    @property
    def abe(self):
        return self.prim_abe if self.prmodecont else self.prmode_abe

    @property
    def ctx(self):
        return self.prim_ctx if self.prmodecont else self.prmode_ctx


class Tally:
    def __init__(self):
        self.by_eq = collections.Counter()        # equação -> kicks
        self.by_eq_opaque = collections.Counter() # equação -> kicks com As==128
        self.prims = collections.Counter()
        self.alpha_hist = collections.Counter()
        self.opaque_kicks = 0                     # ABE desligado
        self.kicks = 0
        self.image_qwords = 0

    def kick(self, st):
        self.kicks += 1
        self.prims[PRIMNAME.get(st.prim_type, "?")] += 1
        if not st.abe:
            self.opaque_kicks += 1
            return
        a, b, c, d, fix = st.alpha[st.ctx]
        # FIX só entra na equação quando C==2; mantê-lo na chave fora disso quebraria a MESMA
        # equação em várias linhas.
        key = (a, b, c, d, fix if c == 2 else 0)
        self.by_eq[key] += 1
        self.alpha_hist[st.vertex_alpha] += 1
        if st.vertex_alpha == OPAQUE_ALPHA and c == 0:
            # C == As e As == 128 => o resultado é exatamente Cs, ou seja, opaco de fato.
            self.by_eq_opaque[key] += 1


def walk_gif(payload, st, tally):
    """Percorre os GIFtags de um pacote Transfer."""
    o, n = 0, len(payload)
    while o + 16 <= n:
        lo, hi = struct.unpack_from("<QQ", payload, o)
        o += 16
        nloop = lo & 0x7FFF
        pre = (lo >> 46) & 1
        prim = (lo >> 47) & 0x7FF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        if pre:
            st.set_prim(prim)
        if flg == 2:  # IMAGE: payload cru, sem registradores
            tally.image_qwords += nloop
            o += nloop * 16
            continue
        if flg == 3:  # IMAGE2, desabilitado no hardware
            continue
        regs = [(hi >> (4 * i)) & 0xF for i in range(16)]
        if flg == 0:  # PACKED: 16 bytes por registrador
            for _ in range(nloop):
                for r in range(nreg):
                    if o + 16 > n:
                        return
                    d0, d1 = struct.unpack_from("<QQ", payload, o)
                    o += 16
                    rid = regs[r]
                    if rid == REG_AD:
                        addr = d1 & 0xFF
                        if addr == AD_ALPHA_1:
                            st.alpha[0] = decode_alpha(d0)
                        elif addr == AD_ALPHA_2:
                            st.alpha[1] = decode_alpha(d0)
                        elif addr == AD_PRIM:
                            st.set_prim(d0)
                        elif addr == AD_PRMODE:
                            st.set_prmode(d0)
                        elif addr == AD_PRMODECONT:
                            st.prmodecont = d0 & 1
                        elif addr == AD_RGBAQ:
                            # RGBAQ como REGISTRADOR: R,G,B,A são os bytes 0..3 e Q é float nos
                            # bits 32..63. Layout diferente do PACKED, onde A fica no bit 96.
                            st.vertex_alpha = (d0 >> 24) & 0xFF
                        elif addr in (AD_XYZF2, AD_XYZ2):
                            # Muitos jogos emitem o vértice como escrita A+D em vez de usar o
                            # nibble de REGS. Ignorar este caminho zera dumps inteiros.
                            tally.kick(st)
                    elif rid == REG_RGBAQ:
                        st.vertex_alpha = (d1 >> 32) & 0xFF
                    elif rid in (REG_XYZF2, REG_XYZ2):
                        tally.kick(st)
        else:  # REGLIST: 8 bytes por registrador, alinhado a duas qwords
            total = nloop * nreg
            for k in range(total):
                if o + 8 > n:
                    return
                o += 8
                if regs[k % nreg] in (REG_XYZF2, REG_XYZ2):
                    tally.kick(st)
            if total % 2:
                o += 8


def parse(path, max_frames=None):
    b = open_dump(path)
    o = 0
    crc, state_bytes = struct.unpack_from("<II", b, o)
    o += 8
    serial, screenshot = "", None
    if crc == 0xFFFFFFFF:
        (state_version, state_size, serial_off, serial_size, real_crc,
         shot_w, shot_h, _shot_off, _shot_size) = struct.unpack_from("<9I", b, o)
        if serial_size:
            serial = b[o + serial_off:o + serial_off + serial_size].decode("ascii", "replace")
        crc = real_crc
        screenshot = (shot_w, shot_h)
        o += state_bytes + state_size
    else:
        o += state_bytes
    o += 8192  # bloco de registradores

    st, tally = GSState(), Tally()
    frames = 0
    transfers = 0
    transfer_bytes = 0
    per_frame = []
    frame_start = (0, 0)
    truncated = False
    n = len(b)
    while o < n:
        pid = b[o]
        o += 1
        if pid == 0:
            o += 1  # path
            (ln,) = struct.unpack_from("<I", b, o)
            o += 4
            if o + ln > n:
                truncated = True
                break
            walk_gif(memoryview(b)[o:o + ln], st, tally)
            o += ln
            transfers += 1
            transfer_bytes += ln
        elif pid == 1:
            o += 1
            frames += 1
            per_frame.append((transfers - frame_start[0], transfer_bytes - frame_start[1]))
            frame_start = (transfers, transfer_bytes)
            if max_frames and frames >= max_frames:
                break
        elif pid == 2:
            o += 4
        elif pid == 3:
            o += 8192
        else:
            truncated = True
            break

    return dict(path=path, crc=f"{crc:08X}", serial=serial, screenshot=screenshot,
                frames=frames, transfers=transfers, transfer_bytes=transfer_bytes,
                per_frame=per_frame, tally=tally, truncated=truncated, size=n)


def report(r):
    t = r["tally"]
    print(f"\n===== {r['path']}")
    shot = f"{r['screenshot'][0]}x{r['screenshot'][1]}" if r["screenshot"] else "-"
    print(f"serial={r['serial'] or '-'} crc={r['crc']} screenshot={shot} "
          f"descomprimido={r['size']:,}B")
    if r["truncated"]:
        print("AVISO: fluxo terminou antes do esperado (dump truncado); "
              "os números abaixo cobrem só o que foi lido")
    print(f"frames: {r['frames']}   transferências GIF: {r['transfers']:,}   "
          f"bytes de comando: {r['transfer_bytes']:,}")
    pf = r["per_frame"]
    if pf:
        tr = sorted(p[0] for p in pf)
        by = sorted(p[1] for p in pf)
        mid = len(tr) // 2
        print(f"  por frame — transferências p50={tr[mid]:,} (min {tr[0]:,} max {tr[-1]:,})")
        print(f"              bytes         p50={by[mid]:,} (min {by[0]:,} max {by[-1]:,})")
    if r["frames"]:
        print(f"  vértices por frame: {t.kicks // r['frames']:,}")
    print(f"primitivas: {dict(t.prims)}")
    if t.image_qwords:
        print(f"qwords de IMAGE (upload para a memória do GS): {t.image_qwords:,}")

    total = t.kicks
    blended = total - t.opaque_kicks
    print(f"\nkicks de vértice: {total:,}")
    print(f"  ABE desligado:  {t.opaque_kicks:,} ({t.opaque_kicks * 100 / max(total, 1):.1f}%)")
    print(f"  ABE ligado:     {blended:,} ({blended * 100 / max(total, 1):.1f}%)")
    if not blended:
        return
    print(f"\n{'equação':<32} {'kicks':>11} {'%':>6} {'As=128':>10} {'obs':<22}")
    for key, cnt in t.by_eq.most_common(12):
        op = t.by_eq_opaque.get(key, 0)
        notes = []
        if key[0] == key[1]:
            notes.append("A==B")
        if CD in (key[0], key[1], key[3]):
            notes.append("lê o destino")
        pct_op = f"{op * 100 / cnt:.0f}%" if cnt else "-"
        print(f"{eq_str(key):<32} {cnt:>11,} {cnt * 100 / blended:>5.1f}% "
              f"{pct_op:>10} {', '.join(notes) or '-':<22}")

    reads_dest = sum(c for k, c in t.by_eq.items() if CD in (k[0], k[1], k[3]))
    really = blended - sum(t.by_eq_opaque.values())
    print(f"\nlê o framebuffer de destino: {reads_dest:,} "
          f"({reads_dest * 100 / max(total, 1):.1f}% dos kicks)")
    print(f"blending que MUDA o pixel (As != 128 quando C=As): {really:,} "
          f"({really * 100 / max(total, 1):.1f}% dos kicks)")
    print("  -> o resto tem ABE ligado mas resolve para Cs; é opaco na prática.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumps", nargs="+", help="arquivos .gs ou .gs.zst")
    ap.add_argument("--frames", type=int, default=None,
                    help="para depois de N frames (dumps longos)")
    ap.add_argument("--json", action="store_true", help="saída legível por máquina")
    a = ap.parse_args()
    out = []
    for f in a.dumps:
        r = parse(f, a.frames)
        if a.json:
            t = r["tally"]
            out.append(dict(
                path=r["path"], serial=r["serial"], crc=r["crc"], frames=r["frames"],
                transfers=r["transfers"], transfer_bytes=r["transfer_bytes"],
                kicks=t.kicks, kicks_abe_off=t.opaque_kicks,
                kicks_effectively_opaque=sum(t.by_eq_opaque.values()),
                prims=dict(t.prims), image_qwords=t.image_qwords,
                blend={eq_str(k): v for k, v in t.by_eq.most_common()},
                truncated=r["truncated"]))
        else:
            report(r)
    if a.json:
        print(json.dumps(out, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
