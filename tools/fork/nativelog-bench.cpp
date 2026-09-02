// Mede as duas estratégias de escrita do native.log, lado a lado, com o mesmo fluxo de entrada.
//
// Por que existe: o commit que trocou "fflush a cada bloco" por coalescimento de 200 ms afirmou
// um ganho SEM medir nada. O raciocínio era sólido — um fflush por bloco de no máximo 511 bytes,
// contra um por janela — mas raciocínio sólido não é evidência, e a regra do projeto é que
// otimização sem número fica registrada como hipótese, não adotada como fato.
//
// O QUE O PIPE REAL ENSINOU, e que o modelo ingênuo deste arquivo errava: o kernel já coalesce.
// 20.000 escritas de 200 B no pipe chegam ao consumidor como ~7.800 leituras de até 511 B, não
// como 20.000. Logo a versão antiga NÃO fazia 20.000 flushes — fazia ~7.800. O ganho continua
// existindo e é grande, mas qualquer comparação que use o número de ESCRITAS como se fosse o de
// leituras superestima o custo antigo em ~2,5x. Os cenários "Pipe ·" abaixo medem o laço de
// verdade; os de cima medem só a política de coalescimento.
//
// Compilar e rodar (não faz parte de nenhum build; é ferramenta de bancada):
//     c++ -std=c++20 -O2 -o /tmp/nativelog-bench tools/fork/nativelog-bench.cpp -lpthread
//     /tmp/nativelog-bench
//
// TERMINOLOGIA — e ela importa aqui mais que em qualquer outro lugar deste projeto.
//
// `fflush()` NAO grava no armazenamento. Ele apenas esvazia o buffer da libc para o kernel; o
// dado passa a estar no page cache do sistema, e daí para o chip de memória flash quem decide é
// o kernel, quando quiser. Chamar isso de "gravado no disco" é a confusão que faz alguém
// concluir que 200 ms de coalescimento é a janela de perda numa queda — não é.
//
// Os três estados que este benchmark distingue:
//
//   * ESCRITO NO BUFFER — fwrite() retornou. Some se o PROCESSO morrer.
//   * ENTREGUE AO SISTEMA — fflush() retornou. Sobrevive à morte do processo, inclusive a um
//     SIGSEGV do emulador, porque o kernel já tem o dado. NÃO sobrevive a queda de energia.
//   * SINCRONIZADO — fdatasync()/fsync() retornou. Só aqui há promessa de durabilidade física,
//     e ainda assim limitada ao que o hardware honra.
//
// Para o problema real — testador de portátil relatando o que houve antes de o emulador cair —
// o que importa é ENTREGUE AO SISTEMA, e é isso que a métrica mede. A variante sincronizada
// existe só para medir o preço da durabilidade física, e não está no código de produção.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <atomic>
#include <mutex>
#include <poll.h>
#include <unistd.h>
#include <cerrno>

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
	// Maior atraso entre um bloco chegar ao consumidor e ser ENTREGUE AO SISTEMA (fflush).
	// Não é durabilidade física: ver a nota de terminologia no topo.
	double max_handoff_ms = 0.0;
	double sync_ms = 0.0;  // tempo gasto em fdatasync, quando a variante sincronizada esta ligada
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
		r.max_handoff_ms = std::max(r.max_handoff_ms,
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
		r.max_handoff_ms = std::max(r.max_handoff_ms,
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

/// Consumidor com o MESMO formato do código de produção: pipe real, poll() com prazo só quando
/// há pendência, e saída por fim de escritor. O benchmark acima mede a política de coalescimento
/// isoladamente; este mede o LAÇO, onde moram os erros que a política sozinha não tem como ter:
/// POLLHUP tratado como dado, EINTR tratado como erro, e o último bloco de uma rajada ficando
/// pendente para sempre porque read() bloqueia e ninguém mais chega.
struct PipeResult
{
	long blocks = 0;
	long bytes = 0;
	long flushes = 0;
	long wakeups = 0;      // retornos de poll(), incluindo timeouts — custo de CPU ocioso
	long timeouts = 0;     // wakeups por PRAZO: é o que fecha a última linha de uma rajada
	long eintr = 0;        // interrupções por sinal, que não podem ser confundidas com erro
	bool saw_hup = false;  // o escritor fechou e o laço percebeu, em vez de girar
	long lost = 0;         // blocos escritos no pipe que não chegaram ao arquivo
	double mutex_ms = 0.0; // tempo segurando o mutex, que é o que a thread de UI disputaria
	double sync_ms = 0.0;
};

/// `quiet_before_close` faz o produtor CALAR antes de fechar. É o único jeito de exercitar o
/// caminho de timeout do poll — a razão de ele existir. Sem esse silêncio, um bloco sempre chega
/// antes do prazo vencer e o flush sai pelo caminho de chegada; o último bloco de uma rajada que
/// termina em silêncio ficaria pendente para sempre, que é exatamente o caso de uma queda logo
/// depois do erro.
PipeResult RunPipeLoop(int count, size_t size, std::chrono::microseconds gap, bool do_fsync,
	std::chrono::milliseconds quiet_before_close = std::chrono::milliseconds(0))
{
	PipeResult r;
	int fds[2];
	if (pipe(fds) != 0)
		return r;

	std::atomic<bool> produced_all{false};
	std::thread produtor([&] {
		const std::string line(size, 'y');
		for (int i = 0; i < count; i++)
		{
			if (gap.count() > 0)
				std::this_thread::sleep_for(gap);
			ssize_t n = write(fds[1], line.data(), line.size());
			(void)n;
		}
		produced_all = true;
		if (quiet_before_close.count() > 0)
			std::this_thread::sleep_for(quiet_before_close);
		// Fim NORMAL do escritor: fecha a ponta de escrita, o que faz o poll do consumidor
		// devolver POLLHUP. Um laço que trate isso como dado disponível gira para sempre.
		close(fds[1]);
	});

	std::FILE* f = std::fopen("/tmp/nativelog-bench-pipe.log", "wb");
	std::mutex mtx;
	bool dirty = false;
	auto last_flush = Clock::time_point{};
	char buf[512];

	auto flush = [&](Clock::time_point now) {
		if (!dirty)
			return;
		const auto t0 = Clock::now();
		{
			std::lock_guard<std::mutex> lock(mtx);
			std::fflush(f);
			if (do_fsync)
			{
				const auto s0 = Clock::now();
				// fdatasync: o ÚNICO ponto deste arquivo que pede durabilidade física. Está aqui
				// para medir o preço dela, e não no código de produção.
				fdatasync(fileno(f));
				r.sync_ms += std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
			}
		}
		r.mutex_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		dirty = false;
		last_flush = now;
		r.flushes++;
	};

	while (true)
	{
		struct pollfd pfd = {fds[0], POLLIN, 0};
		const int ready = poll(&pfd, 1, dirty ? (int)kFlushInterval.count() : -1);
		r.wakeups++;
		if (ready < 0)
		{
			if (errno == EINTR)
			{
				r.eintr++;
				continue;
			}
			break;
		}
		if (ready == 0)
		{
			r.timeouts++;
			flush(Clock::now());
			continue;
		}
		const ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
		if (n <= 0)
		{
			// n == 0 com POLLIN|POLLHUP é o fim normal: o escritor fechou e o pipe secou.
			if (n < 0 && errno == EINTR)
			{
				r.eintr++;
				continue;
			}
			if (pfd.revents & POLLHUP)
				r.saw_hup = true;
			break;
		}
		std::fwrite(buf, 1, (size_t)n, f);
		dirty = true;
		r.blocks++;
		r.bytes += n;
		if (Clock::now() - last_flush >= kFlushInterval)
			flush(Clock::now());
	}
	// Encerramento com dados pendentes: sem isto a última linha antes de uma queda se perde.
	flush(Clock::now() + kFlushInterval);
	std::fclose(f);
	produtor.join();
	close(fds[0]);

	const long esperado = (long)(count * size);
	r.lost = esperado - r.bytes;
	return r;
}

void ReportPipe(const char* cenario, const PipeResult& r)
{
	std::printf("\n%s\n", cenario);
	std::printf("  blocos %ld · bytes %ld · flushes %ld · wakeups %ld (timeouts %ld) · EINTR %ld\n",
		r.blocks, r.bytes, r.flushes, r.wakeups, r.timeouts, r.eintr);
	std::printf("  POLLHUP visto: %s · bytes perdidos: %ld · mutex %.2f ms",
		r.saw_hup ? "sim" : "nao", r.lost, r.mutex_ms);
	if (r.sync_ms > 0.0)
		std::printf(" · fdatasync %.2f ms", r.sync_ms);
	std::printf("\n");
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
	std::printf("  %-22s %12.2f %12.2f\n", "atraso max ate o SO (ms)", old_r.max_handoff_ms, new_r.max_handoff_ms);
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
		// cada linha continua sendo entregue ao SO na hora — o coalescimento nao custa nada aqui.
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

	// --- Laço real: pipe, duas threads, poll() --------------------------------------------------
	ReportPipe("Pipe · rajada curta (500 blocos, sem intervalo)",
		RunPipeLoop(500, 200, std::chrono::microseconds(0), false));
	ReportPipe("Pipe · rajada longa (20.000 blocos)",
		RunPipeLoop(20000, 200, std::chrono::microseconds(0), false));
	ReportPipe("Pipe · esparso (30 blocos, 1 a cada 20 ms)",
		RunPipeLoop(30, 120, std::chrono::microseconds(20000), false));
	ReportPipe("Pipe · rajada que termina em SILENCIO (exercita o timeout do poll)",
		RunPipeLoop(500, 200, std::chrono::microseconds(0), false, std::chrono::milliseconds(500)));
	ReportPipe("Pipe · rajada longa COM fdatasync (custo da durabilidade fisica)",
		RunPipeLoop(20000, 200, std::chrono::microseconds(0), true));

	std::remove(a);
	std::remove(b);
	std::remove("/tmp/nativelog-bench-pipe.log");
	return 0;
}
