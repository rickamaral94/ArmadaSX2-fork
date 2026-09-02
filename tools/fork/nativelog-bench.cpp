// Mede as duas estratégias de escrita do native.log, lado a lado, com o mesmo fluxo de entrada.
//
// Por que existe: o commit que trocou "fflush a cada bloco" por coalescimento de 200 ms afirmou
// um ganho SEM medir nada. O raciocínio era sólido — um fflush por bloco de no máximo 511 bytes,
// contra um por janela — mas raciocínio sólido não é evidência, e a regra do projeto é que
// otimização sem número fica registrada como hipótese, não adotada como fato.
//
// Compilar e rodar (não faz parte de nenhum build; é ferramenta de bancada):
//     c++ -std=c++20 -O2 -o /tmp/nativelog-bench tools/fork/nativelog-bench.cpp -lpthread
//     /tmp/nativelog-bench
//
// O que mede, por cenário e por estratégia: blocos recebidos, bytes gravados, número de flushes,
// tempo de parede no caminho de escrita, e o ATRASO MÁXIMO entre um bloco chegar e estar
// efetivamente no disco — que é a métrica que decide se o coalescimento custou evidência numa
// queda. Um ganho de escrita que jogue fora a última linha antes do crash não é ganho.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto kFlushInterval = std::chrono::milliseconds(200);
constexpr long kMaxBytes = 8L * 1024 * 1024;

struct Result
{
	long blocks = 0;
	long bytes = 0;
	long flushes = 0;
	long rotations = 0;
	double write_path_ms = 0.0;   // tempo de parede dentro do caminho de escrita
	double max_persist_ms = 0.0;  // maior atraso entre o bloco chegar e ir ao disco
};

struct Feed
{
	int count = 0;
	size_t size = 0;
	std::chrono::milliseconds gap{0};  // 0 = rajada; > 0 dorme de verdade entre os blocos
};

/// Estratégia ANTIGA: fflush a cada bloco lido do pipe.
Result RunPerBlock(const Feed& feed, const char* path)
{
	Result r;
	std::FILE* f = std::fopen(path, "wb");
	const std::string line(feed.size, 'x');
	long written = 0;
	for (int i = 0; i < feed.count; i++)
	{
		if (feed.gap.count() > 0)
			std::this_thread::sleep_for(feed.gap);
		const auto arrived = Clock::now();
		const auto t0 = arrived;
		std::fwrite(line.data(), 1, line.size(), f);
		std::fflush(f);
		written += static_cast<long>(line.size());
		r.flushes++;
		if (written > kMaxBytes)
		{
			if (!std::freopen(nullptr, "wb", f))
				std::perror("freopen");
			written = 0;
			r.rotations++;
		}
		const auto t1 = Clock::now();
		r.write_path_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
		// Persistido no mesmo instante em que chegou ao consumidor: o atraso é só o custo da
		// própria escrita. É a vantagem real desta estratégia, e o que o coalescimento negocia.
		r.max_persist_ms = std::max(r.max_persist_ms,
			std::chrono::duration<double, std::milli>(t1 - arrived).count());
		r.blocks++;
		r.bytes += static_cast<long>(line.size());
	}
	std::fclose(f);
	return r;
}

/// Estratégia ATUAL: coalescimento por prazo, com o pendente mais antigo rastreado.
Result RunCoalesced(const Feed& feed, const char* path)
{
	Result r;
	std::FILE* f = std::fopen(path, "wb");
	const std::string line(feed.size, 'x');
	long written = 0;
	bool dirty = false;
	auto last_flush = Clock::time_point{};
	Clock::time_point oldest_pending{};

	auto flush = [&](Clock::time_point now) {
		if (!dirty)
			return;
		std::fflush(f);
		dirty = false;
		last_flush = now;
		r.flushes++;
		r.max_persist_ms = std::max(r.max_persist_ms,
			std::chrono::duration<double, std::milli>(now - oldest_pending).count());
	};

	for (int i = 0; i < feed.count; i++)
	{
		if (feed.gap.count() > 0)
			std::this_thread::sleep_for(feed.gap);
		const auto arrived = Clock::now();
		const auto t0 = arrived;
		std::fwrite(line.data(), 1, line.size(), f);
		written += static_cast<long>(line.size());
		if (!dirty)
			oldest_pending = arrived;
		dirty = true;
		if (written > kMaxBytes)
		{
			// freopen descarta o buffer: o pendente sai ANTES, senão a rotação comeria justamente
			// as últimas linhas.
			flush(Clock::now());
			if (!std::freopen(nullptr, "wb", f))
				std::perror("freopen");
			written = 0;
			r.rotations++;
		}
		else if (Clock::now() - last_flush >= kFlushInterval)
		{
			flush(Clock::now());
		}
		const auto t1 = Clock::now();
		r.write_path_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
		r.blocks++;
		r.bytes += static_cast<long>(line.size());
	}
	// O que o poll() do código real faz quando a rajada termina e o log fica em silêncio: sem
	// isso o último bloco ficaria pendente para sempre, que é exatamente o caso de uma queda
	// logo depois do erro.
	flush(Clock::now() + kFlushInterval);
	std::fclose(f);
	return r;
}

void Report(const char* scenario, const Result& old_r, const Result& new_r)
{
	std::printf("\n%s\n", scenario);
	std::printf("  %-22s %12s %12s\n", "", "por bloco", "coalescido");
	std::printf("  %-22s %12ld %12ld\n", "blocos", old_r.blocks, new_r.blocks);
	std::printf("  %-22s %12ld %12ld\n", "bytes", old_r.bytes, new_r.bytes);
	std::printf("  %-22s %12ld %12ld\n", "flushes", old_r.flushes, new_r.flushes);
	std::printf("  %-22s %12ld %12ld\n", "rotacoes", old_r.rotations, new_r.rotations);
	std::printf("  %-22s %12.2f %12.2f\n", "tempo de escrita (ms)", old_r.write_path_ms, new_r.write_path_ms);
	std::printf("  %-22s %12.2f %12.2f\n", "atraso max ate disco (ms)", old_r.max_persist_ms, new_r.max_persist_ms);
	if (old_r.flushes > 0)
		std::printf("  %-22s %11.1fx menos flushes\n", "resultado",
			static_cast<double>(old_r.flushes) / std::max(1L, new_r.flushes));
}
} // namespace

int main()
{
	const char* a = "/tmp/nativelog-bench-a.log";
	const char* b = "/tmp/nativelog-bench-b.log";

	{
		// Rajada: compilação de shader, erro de Vulkan repetindo, boot. É o caso que motivou a
		// mudança, e o único em que o coalescimento tem o que coalescer.
		const Feed f{20000, 200, std::chrono::milliseconds(0)};
		Report("Rajada — 20.000 blocos de 200 B, sem intervalo", RunPerBlock(f, a), RunCoalesced(f, b));
	}
	{
		// Log esparso: uma linha a cada 250 ms. O prazo já venceu quando o bloco chega, então
		// cada linha continua indo ao disco na hora — o coalescimento não deve custar nada aqui.
		// Dorme de verdade: com carimbo simulado isto virava outra rajada e reportava um ganho
		// que o desenho NAO preve — o esparso deve custar um flush por linha nas duas
		// estrategias, porque o prazo ja venceu quando o bloco chega.
		const Feed f{20, 120, std::chrono::milliseconds(250)};
		Report("Esparso — 20 blocos, 1 a cada 250 ms (dorme de verdade)", RunPerBlock(f, a), RunCoalesced(f, b));
	}
	{
		// Passa dos 8 MiB e força rotação, que é onde um flush esquecido comeria as últimas
		// linhas: freopen descarta o buffer.
		const Feed f{30000, 400, std::chrono::milliseconds(0)};
		Report("Rotacao — 12 MB, cruza o teto de 8 MiB", RunPerBlock(f, a), RunCoalesced(f, b));
	}

	std::remove(a);
	std::remove(b);
	return 0;
}
