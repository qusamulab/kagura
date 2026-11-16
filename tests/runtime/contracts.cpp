#include "kagura/runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static void check(bool OK) {
  if (!OK) {
    std::fputs("runtime contract failed\n", stderr);
    std::abort();
  }
}

// Stable detector inputs; these tests exercise report authentication plumbing,
// replay commits and buffering independently of the host's security state.
extern "C" {
int kagura_device_key(uint8_t out[16]) {
  std::memset(out, 7, 16);
  return 1;
}
int kagura_jailbreak_detected(void) { return 0; }
int kagura_magisk_present(void) { return 0; }
int kagura_xposed_present(void) { return 0; }
int kagura_suspicious_lib_loaded(void) { return 0; }
int kagura_check_injected_dlls(void) { return 0; }
int kagura_check_frida_port(void) { return 0; }
int kagura_check_emulator(void) { return 0; }
int kagura_check_tracer_pid(void) { return 0; }
int kagura_check_debugger_present(void) { return 0; }
int kagura_check_remote_debugger(void) { return 0; }
int kagura_check_nt_debug_port(void) { return 0; }
int kagura_macho_tampered(void) { return 0; }
int kagura_elf_tampered(void) { return 0; }
int kagura_pe_checksum_valid(void) { return 1; }
}

struct Signing {
  std::string Body;
  std::atomic<int> *Barrier = nullptr;
};

// Deliberately a fixture, not a MAC implementation. Production signers must
// use HMAC-SHA256; the runtime never substitutes a checksum for their result.
static int sign(const uint8_t *Body, size_t Len, uint8_t Out[32], void *Ctx) {
  auto &S = *static_cast<Signing *>(Ctx);
  S.Body.assign(reinterpret_cast<const char *>(Body), Len);
  if (S.Barrier) {
    ++*S.Barrier;
    while (S.Barrier->load() < 8)
      std::this_thread::yield();
  }
  std::memset(Out, 0xa5, 32);
  return 1;
}
static int fail(const uint8_t *, size_t, uint8_t[32], void *) { return 0; }

int main() {
  kagura_clear_events();
  std::vector<std::thread> Threads;
  for (int t = 0; t < 8; ++t)
    Threads.emplace_back([] {
      for (int i = 0; i < 12; ++i)
        kagura_log_event(1, i);
    });
  for (auto &T : Threads)
    T.join();
  check(kagura_event_count(1) == 96);
  check(kagura_suspicion_score() == 480);
  for (int i = 0; i < 10000; ++i)
    kagura_log_event(1, i);
  check(kagura_event_count(1) == 128);
  int Count = 0;
  kagura_flush_events(
      [](uint8_t, uint32_t, void *Ctx) {
        ++*static_cast<int *>(Ctx);
        kagura_clear_events(); // Reentrant callback must not deadlock or alter
                               // iteration.
        kagura_log_event(2, 42);
      },
      &Count);
  check(Count == 128 && kagura_event_count(2) == 1);

  Signing S;
  char Out[1024];
  check(kagura_integrity_report_build("deadbeef", Out, sizeof(Out)) == -1);
  check(Out[0] == 0);
  for (const char *Nonce : {"", "bad\"json", "xyz",
                            "0123456789abcdef0123456789abcdef0123456789abcdef01"
                            "23456789abcdef12345"})
    check(kagura_integrity_report_build_signed(Nonce, Out, sizeof(Out), sign,
                                               &S) == -1);
  check(kagura_integrity_report_build_signed("deadbeef", Out, sizeof(Out), fail,
                                             nullptr) == -1);
  check(kagura_integrity_report_build_signed("deadbeef", Out, 16, sign, &S) ==
        -1);
  int N = kagura_integrity_report_build_signed("deadbeef", Out, sizeof(Out),
                                               sign, &S);
  check(N > 0 && static_cast<size_t>(N) == std::strlen(Out));
  check(S.Body.find("\"version\":2") != std::string::npos);
  check(S.Body.find("HMAC-SHA256") != std::string::npos);
  std::string ExpectedMAC;
  for (int I = 0; I < 32; ++I)
    ExpectedMAC += "a5";
  check(std::string(Out) == S.Body.substr(0, S.Body.size() - 1) +
                                ",\"mac\":\"" + ExpectedMAC + "\"}");
  check(kagura_integrity_report_build_signed("deadbeef", Out, sizeof(Out), sign,
                                             &S) == -1);
  check(Out[0] == 0);

  Threads.clear();
  std::atomic<int> Barrier{0}, Successes{0};
  for (int t = 0; t < 8; ++t)
    Threads.emplace_back([&] {
      Signing Local;
      Local.Barrier = &Barrier;
      char Buffer[1024];
      if (kagura_integrity_report_build_signed(
              "aabbccdd", Buffer, sizeof(Buffer), sign, &Local) > 0)
        ++Successes;
    });
  for (auto &T : Threads)
    T.join();
  check(Successes == 1);
}
