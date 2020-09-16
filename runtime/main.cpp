#include <signal.h>
#include "runtime_impl.hpp"

constexpr std::chrono::seconds ILLIXR_RUN_DURATION_DEFAULT {60};

ILLIXR::runtime* r;

static void signal_handler(int) {
	if (r) {
		r->stop();
	}
}

class cancellable_sleep {
public:
	template <typename T, typename R>
	bool sleep(std::chrono::duration<T, R> duration) {
		auto wake_up_time = std::chrono::system_clock::now() + duration;
		while (!_m_terminate.load() && std::chrono::system_clock::now() < wake_up_time) {
			std::this_thread::sleep_for(std::chrono::milliseconds{100});
		}
		return _m_terminate.load();
	}
	void cancel() {
		_m_terminate.store(true);
	}
private:
	std::atomic<bool> _m_terminate {false};
};

int main(int argc, const char * argv[]) {
	r = ILLIXR::runtime_factory(nullptr);

	for (int i = 1; i < argc; ++i) {
		r->load_so(argv[i]);
	}

	// Two ways of shutting down:
	// Ctrl+C
	signal(SIGINT, signal_handler);

	// And timer
	std::chrono::seconds run_duration = 
		getenv("ILLIXR_RUN_DURATION")
		? std::chrono::seconds{std::stol(std::string{getenv("ILLIXR_RUN_DURATION")})}
		: ILLIXR_RUN_DURATION_DEFAULT
	;

	cancellable_sleep cs;
	std::thread th{[&]{
		cs.sleep(run_duration);
		r->stop();
	}};

	r->wait(); // blocks until shutdown is r->stop()

	// cancel our sleep, so we can join the other thread
	cs.cancel();
	th.join();

	delete r;
	return 0;
}
